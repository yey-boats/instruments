// midl_render_apply.cpp — MIDL doc -> freeform LVGL screen orchestration.
//
// Turns a parsed MIDL document into one or more registered LVGL screens via:
//   solve_screen -> map_element (enum bridge) -> create_freeform -> register.
//
// Two entry points:
//   - apply_all(doc): register EVERY screen in doc["screens"] (up to
//     ui::MAX_SCREENS) and enable `screen <id|next|prev>` navigation. This is
//     the multi-screen path used at boot.
//   - apply_doc(doc, screen_id): single-screen convenience — builds all screens
//     via apply_all then shows the requested one (or doc settings.defaultScreen
//     / the first screen). Kept for the `midl-render` console command and the
//     ConfigApplyMidl pump case.
//
// Mirrors the legacy layout_renderer.cpp flow (per-slot RendererSlot pool +
// template-generated refresh trampolines) but uses the MIDL solver instead of
// the editor JSON shape.
//
// DEVICE-ONLY: this TU includes <lvgl.h> (via ui_layouts.h) and ui_screens.h.
// It is NOT listed in the native build_src_filter — only in esp32-4848s040.
// The pure mapper (token_to_kind / map_element) and the pure selectors
// (select_screen / find_element) live in midl_render.cpp which IS in the
// native build.
//
// MEMORY TRAPS (CLAUDE.md):
//   - The per-screen arena array `s_arenas` is PSRAM-allocated (NOT a static
//     .bss array — that would starve internal SRAM / NimBLE).
//   - Each arena is `memset`-cleared in place before building (never
//     `arena = MidlScreenArena{}`, which builds a ~1 KB temporary on the task
//     stack — same trap as `out = Config{}`).
//   - All build/`lv_obj_*` work runs on the UI task (apply_all/apply_doc are
//     called from setup() and the app pump, both the UI/LVGL task).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_render.h"

#include "midl_solve.h"  // midl::solve_screen, PlacementSet, SolveStatus, SOLVE_OK
#include "ui_layouts.h"  // ui::layouts::create_freeform, update_freeform, ScreenVariantSpec, Rect
#include "ui_screens.h"  // ui::Screen, register_screen, replace_screen, MAX_SCREENS
#include "signalk.h"     // boat::current_view, boat::View
#include "net.h"         // net::logf

#include "esp_heap_caps.h"

#include <string.h>

// Global (main.cpp): true while the current touch contact is a drag/swipe, not a
// tap. At global scope so zoom_back_cb references ::g_pointer_dragging.
extern volatile bool g_pointer_dragging;

namespace midl::render {

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::Rect;
using ui::layouts::ScreenVariantSpec;
using ui::layouts::TemplateId;

// ---------------------------------------------------------------------------
// Per-screen arena. One per registered MIDL screen. Function-lifetime via the
// PSRAM-allocated s_arenas[] pool below.
//
// Layout: MetricBinding[] stores NON-OWNING const char* into the id/label/unit
// char arrays in this same arena, so they all outlive the screen. The spec and
// rect table live here too so the refresh trampoline can reach them without
// capturing any stack variable.
// ---------------------------------------------------------------------------

static constexpr size_t MAX_TILES = midl::FirmwareLimits::max_tiles_per_screen;  // 4
static constexpr size_t STR_CAP = midl::FirmwareLimits::str_len;                 // 32

struct MidlScreenArena {
    // String storage backing the MetricBinding non-owning pointers.
    char ids[MAX_TILES][STR_CAP];
    char labels[MAX_TILES][STR_CAP];
    char units[MAX_TILES][STR_CAP];
    char actions[MAX_TILES][STR_CAP];  // button action target (nav/command)
    char zooms[MAX_TILES][STR_CAP];    // per-element zoom screen id (string form)

    // The MetricBinding table passed to create_freeform / update_freeform.
    MetricBinding metrics[MAX_TILES];

    // Pixel rects produced by the solver.
    Rect rects[MAX_TILES];

    // Screen id / title stored here so Screen.id/title point at this arena.
    char screen_id[STR_CAP];
    char screen_title[STR_CAP];

    // The ScreenVariantSpec used by create_freeform and the refresh trampoline.
    // .metrics points at this->metrics[]; .metric_count is set per build.
    ScreenVariantSpec spec;

    // The most-recently built LVGL root.  The trampoline reads it from here.
    lv_obj_t *root;

    // true once this arena holds a live built screen.
    bool live;
};

// Compile-time footprint guard (Part B): the whole arena pool is
// ui::MAX_SCREENS * sizeof(MidlScreenArena), PSRAM-allocated. Each arena grows
// with the spec-derived tile count, so hold the pool under the budget — a bumped
// maxTiles that would overrun fails the BUILD, not the device.
static_assert(
    ui::MAX_SCREENS * sizeof(MidlScreenArena) <= midl::MIDL_ARENA_PSRAM_BUDGET,
    "MIDL arena pool exceeds MIDL_ARENA_PSRAM_BUDGET; raise the budget or lower maxTiles");

// ---------------------------------------------------------------------------
// Per-screen arena pool. PSRAM-allocated array of ui::MAX_SCREENS (16) arenas.
//
// NOT a static .bss array: 16 * sizeof(MidlScreenArena) (~16 KB) in internal
// SRAM would starve NimBLE / the LVGL draw buffers (CLAUDE.md "live Config must
// be PSRAM-allocated" trap). Allocated lazily on first use; serial UI-task
// access makes the lazy init race-free.
// ---------------------------------------------------------------------------
static MidlScreenArena *s_arenas = nullptr;
static size_t s_arena_count = 0;  // number of arenas currently holding a live screen

static MidlScreenArena *arenas() {
    if (!s_arenas) {
        s_arenas = (MidlScreenArena *)heap_caps_calloc(ui::MAX_SCREENS, sizeof(MidlScreenArena),
                                                       MALLOC_CAP_SPIRAM);
        if (!s_arenas) {
            net::logf("[midl-render] PSRAM alloc of arena pool failed (%u bytes)",
                      (unsigned)(ui::MAX_SCREENS * sizeof(MidlScreenArena)));
        }
    }
    return s_arenas;
}

// ---------------------------------------------------------------------------
// Refresh + collect-paths trampolines — one per arena slot. ui::Screen.refresh
// / collect_paths take no user_data, so the slot index is baked into each
// template instantiation. Mirrors layout_renderer.cpp's refresh_slot<N> /
// collect_slot<N> tables.
// ---------------------------------------------------------------------------
template <size_t N> static void midl_refresh_n() {
    if (!s_arenas) return;
    MidlScreenArena &a = s_arenas[N];
    if (!a.live || !a.root) return;
    boat::View d;
    boat::current_view(d);
    ui::layouts::update_freeform(a.root, a.spec, d);
}

template <size_t N> static void midl_collect_n(sk::SubscriptionSet &out) {
    if (!s_arenas) return;
    MidlScreenArena &a = s_arenas[N];
    if (!a.live) return;
    ui::layouts::collect_paths(a.spec, out);
}

// Trampoline tables indexed by slot. ui::MAX_SCREENS == 16.
static void (*const s_refresh_fns[ui::MAX_SCREENS])() = {
    midl_refresh_n<0>,  midl_refresh_n<1>,  midl_refresh_n<2>,  midl_refresh_n<3>,
    midl_refresh_n<4>,  midl_refresh_n<5>,  midl_refresh_n<6>,  midl_refresh_n<7>,
    midl_refresh_n<8>,  midl_refresh_n<9>,  midl_refresh_n<10>, midl_refresh_n<11>,
    midl_refresh_n<12>, midl_refresh_n<13>, midl_refresh_n<14>, midl_refresh_n<15>,
};

static const ui::CollectPathsFn s_collect_fns[ui::MAX_SCREENS] = {
    midl_collect_n<0>,  midl_collect_n<1>,  midl_collect_n<2>,  midl_collect_n<3>,
    midl_collect_n<4>,  midl_collect_n<5>,  midl_collect_n<6>,  midl_collect_n<7>,
    midl_collect_n<8>,  midl_collect_n<9>,  midl_collect_n<10>, midl_collect_n<11>,
    midl_collect_n<12>, midl_collect_n<13>, midl_collect_n<14>, midl_collect_n<15>,
};

// ---------------------------------------------------------------------------
// build_screen_into: solve + map + create_freeform for ONE screen object,
// writing all state into `arena` and registering/replacing it on the screen
// manager with the trampolines at slot `refresh_index`. Runs ON THE UI TASK.
//
// Shared by apply_all (per-screen loop) and apply_doc (single screen).
// Returns true if the screen was built and registered.
// ---------------------------------------------------------------------------
static bool build_screen_into(JsonVariantConst screen_obj, const char *id, MidlScreenArena &arena,
                              size_t refresh_index) {
    if (!screen_obj.is<JsonObjectConst>()) {
        net::logf("[midl-render] screen object is not a JSON object");
        return false;
    }
    if (refresh_index >= ui::MAX_SCREENS) {
        net::logf("[midl-render] refresh_index %u out of range", (unsigned)refresh_index);
        return false;
    }

    // --- Solve layout ---
    midl::PlacementSet placements;
    midl::SolveStatus st = midl::solve_screen(screen_obj["layout"], {0, 0, 480, 480}, placements);
    if (st != midl::SOLVE_OK) {
        net::logf("[midl-render] solve_screen failed: status=%d", (int)st);
        return false;
    }
    if (placements.count == 0) {
        net::logf("[midl-render] solve_screen produced 0 placements");
        return false;
    }

    // --- Wipe the arena IN PLACE (never `arena = MidlScreenArena{}` — that
    // builds a ~1 KB stack temporary; see CLAUDE.md memset trap). ---
    memset(&arena, 0, sizeof(arena));

    JsonObjectConst elements_node = screen_obj["elements"].as<JsonObjectConst>();

    size_t n = placements.count;
    if (n > MAX_TILES) n = MAX_TILES;

    for (size_t i = 0; i < n; ++i) {
        const midl::Placement &pl = placements.items[i];
        // Look up the element by KEY via find_element (pure, host-tested):
        // explicit strcmp, not elements_node[pl.element] — pl.element is the
        // solver's own buffer and ArduinoJson's operator[] can miss it via a
        // pointer-identity fast path (observed in the headless sim).
        JsonVariantConst el = find_element(elements_node, pl.element);

        bool ok = map_element(el, pl.element, arena.metrics[i], arena.ids[i], arena.labels[i],
                              arena.units[i], arena.actions[i], arena.zooms[i]);
        if (!ok) {
            // Unknown element: leave as zero-init MetricBinding (None source -> "--").
            // Copy at least the id so the tile chrome shows something.
            strncpy(arena.ids[i], pl.element, STR_CAP - 1);
            arena.ids[i][STR_CAP - 1] = 0;
            arena.metrics[i].id = arena.ids[i];
            arena.metrics[i].label = arena.ids[i];
            arena.metrics[i].unit = arena.units[i];  // "" (zero-init)
            arena.metrics[i].source = MetricSource::None;
        }

        // Build the Rect from solver output. Field order: x,y,w,h.
        arena.rects[i] = {pl.rect.x, pl.rect.y, pl.rect.w, pl.rect.h};
    }

    // --- Build the ScreenVariantSpec ---
    strncpy(arena.screen_id, id, STR_CAP - 1);
    arena.screen_id[STR_CAP - 1] = 0;

    // Title: prefer screen-level title field, else screen id.
    const char *title_src = screen_obj["title"] | id;
    strncpy(arena.screen_title, title_src, STR_CAP - 1);
    arena.screen_title[STR_CAP - 1] = 0;

    arena.spec.screen_id = arena.screen_id;
    arena.spec.title = arena.screen_title;
    arena.spec.template_id = TemplateId::QuadGrid;  // irrelevant; create_freeform called directly
    arena.spec.metrics = arena.metrics;
    arena.spec.metric_count = (uint8_t)n;
    arena.spec.variant_flags = 0;

    // --- Build the LVGL freeform screen ---
    lv_obj_t *root = ui::layouts::create_freeform(nullptr, arena.spec, arena.rects);
    if (!root) {
        net::logf("[midl-render] create_freeform returned null for '%s'", arena.screen_id);
        return false;
    }
    arena.root = root;
    arena.live = true;

    // --- Fill a ui::Screen and register/replace ---
    ui::Screen scr = {};
    scr.id = arena.screen_id;
    scr.title = arena.screen_title;
    scr.root = root;
    scr.refresh = s_refresh_fns[refresh_index];
    scr.hidden = false;
    scr.build_fn = nullptr;  // eager: root is pre-supplied
    scr.collect_paths = s_collect_fns[refresh_index];

    // Replace if this id is already registered; otherwise add a new screen.
    // NOTE: replace_screen copies scr.collect_paths from the Screen struct, so no
    // separate set_screen_collect_paths call is needed here — it would cause a
    // spurious subscription re-diff on every hot-reload.
    if (!ui::replace_screen(arena.screen_id, scr)) {
        ui::register_screen(scr);
        net::logf("[midl-render] registered new screen '%s' (%zu tiles)", arena.screen_id, n);
    } else {
        net::logf("[midl-render] replaced screen '%s' (%zu tiles)", arena.screen_id, n);
    }

    return true;
}

// ---------------------------------------------------------------------------
// count_buildable_screens: dry pre-validation pass for apply_all. Counts how
// many `screens[]` entries are object screens whose layout solves to >=1
// placement — i.e. would yield a usable screen if built. Touches NO arena and
// NO LVGL state, so it is safe to run BEFORE reset_screens() (which tears down
// the old screen set). Used to guarantee we never reset_screens() unless the
// new doc has at least one buildable screen — a bad push must not blank the
// device. Runs ON THE UI TASK (no LVGL calls, but keep it there for consistency).
// ---------------------------------------------------------------------------
static size_t count_buildable_screens(JsonArrayConst screens) {
    size_t usable = 0;
    for (JsonVariantConst sv : screens) {
        if (usable >= ui::MAX_SCREENS) break;
        if (!sv.is<JsonObjectConst>()) continue;
        midl::PlacementSet placements;
        midl::SolveStatus st = midl::solve_screen(sv["layout"], {0, 0, 480, 480}, placements);
        if (st == midl::SOLVE_OK && placements.count > 0) ++usable;
    }
    return usable;
}

// ===========================================================================
// Fullscreen tap-to-zoom (MIDL). Tapping any zoomable value/instrument tile on
// a MIDL screen builds a transient single-element full-screen render of the SAME
// element and shows it; tapping the zoom view returns to the screen it came from.
//
// The MIDL build has no static "zoom" screen (that one only exists in the legacy
// non-MIDL build), so we install ui::layouts::set_zoom_fullscreen_handler() at
// apply_all() time. The tile tap handler in ui_layouts.cpp routes a fullscreen-
// self tap (zoom_target == nullptr / "auto") here.
//
// MEMORY TRAPS (CLAUDE.md): the zoom arena is PSRAM (heap_caps_calloc), the
// MetricBinding's strings are DEEP-COPIED into it (the source screen's arena may
// be rebuilt by a later apply_all), and all work runs on the UI/LVGL task (the
// tap callback dispatches there). No large struct is built on the stack.
// ===========================================================================

static constexpr const char *ZOOM_SCREEN_ID = "__zoom__";

// Dedicated PSRAM arena for the single zoomed element. Separate from s_arenas[]
// so a zoom never clobbers a live screen's backing store. Lazily allocated.
static MidlScreenArena *s_zoom_arena = nullptr;
// Screen id to return to when the zoom view is dismissed (captured at zoom time).
static char s_zoom_return_id[STR_CAP] = {0};

static MidlScreenArena *zoom_arena() {
    if (!s_zoom_arena) {
        s_zoom_arena =
            (MidlScreenArena *)heap_caps_calloc(1, sizeof(MidlScreenArena), MALLOC_CAP_SPIRAM);
        if (!s_zoom_arena) net::logf("[midl-render] PSRAM alloc of zoom arena failed");
    }
    return s_zoom_arena;
}

// Refresh + collect trampolines for the zoom screen (it is registered as its own
// screen; the freeform update path drives the single tile from the fused boat::View).
static void zoom_refresh() {
    if (!s_zoom_arena || !s_zoom_arena->live || !s_zoom_arena->root) return;
    boat::View d;
    boat::current_view(d);
    ui::layouts::update_freeform(s_zoom_arena->root, s_zoom_arena->spec, d);
}
static void zoom_collect(sk::SubscriptionSet &out) {
    if (!s_zoom_arena || !s_zoom_arena->live) return;
    ui::layouts::collect_paths(s_zoom_arena->spec, out);
}

// Return from the zoom view to the screen it was launched from: re-show the
// captured return id, falling back to screen 0 if it went stale.
static void zoom_return() {
    if (s_zoom_return_id[0] && ui::show_by_id(s_zoom_return_id)) return;
    ui::show(0);
}

// Tap on the zoom view -> return to the screen it was launched from.
// Tap on the zoom view returns. A drag/swipe is handled by the swipe-dismiss path
// (dismiss_zoom via the ShowScreen handler), so ignore the click when dragging.
// g_pointer_dragging is the global from main.cpp (declared at file scope above).
static void zoom_back_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (g_pointer_dragging) return;
    zoom_return();
}

// Dismiss the zoom screen on a swipe/gesture (Change B / cause 2). The zoom is a
// hidden registered screen, so the touch-task swipe detector's next()/prev()
// would skip PAST it to a sibling (feels like "scroll wrong"), and its
// up=settings / down=dashboard maps would navigate AWAY instead of returning.
// The swipe detector calls this first: if the zoom view is the current screen we
// own the gesture and return to the launching screen; otherwise we report false
// and the detector handles the swipe normally. Runs on the UI task (the detector
// posts here from the touch task only after confirming current_id == __zoom__,
// and ui::show is the same UI-task path the rest of nav uses).
bool dismiss_zoom() {
    const char *cur = ui::current_id();
    if (!cur || strcmp(cur, ZOOM_SCREEN_ID) != 0) return false;
    zoom_return();
    return true;
}

// Build (or rebuild) the zoom screen for one metric and show it. Runs on the UI
// task (invoked from the tile tap callback). DEEP-COPIES the binding's strings
// into the zoom arena so they outlive the source screen.
static void zoom_to_fullscreen(const MetricBinding &m) {
    MidlScreenArena *za = zoom_arena();
    if (!za) return;

    // Capture where to return BEFORE we switch screens.
    const char *cur = ui::current_id();
    strncpy(s_zoom_return_id, cur ? cur : "", STR_CAP - 1);
    s_zoom_return_id[STR_CAP - 1] = 0;

    // Wipe the arena in place (never `*za = MidlScreenArena{}` — stack-temp trap).
    memset(za, 0, sizeof(*za));

    // Deep-copy the metric + its strings into the zoom arena (slot 0).
    MetricBinding &zm = za->metrics[0];
    zm = m;  // copies scalar fields + (dangling) const char* — fixed up below
    strncpy(za->ids[0], m.id ? m.id : "", STR_CAP - 1);
    strncpy(za->labels[0], m.label ? m.label : "", STR_CAP - 1);
    strncpy(za->units[0], m.unit ? m.unit : "", STR_CAP - 1);
    zm.id = za->ids[0];
    zm.label = za->labels[0];
    zm.unit = za->units[0];
    // The zoomed tile must NOT re-zoom (tap returns instead). zoomable=false with a
    // non-null zoom_target makes create_freeform's interactivity check resolve to
    // ZOOM_NONE, so it wires no tap on the tile — the root back handler owns taps.
    zm.zoomable = false;
    za->zooms[0][0] = 0;
    zm.zoom_target = za->zooms[0];  // non-null empty -> zoom_action == ZOOM_NONE
    // target_screen/command are non-owning pointers into the source arena; clear
    // them so the zoom tile carries no stale nav/command action.
    zm.target_screen = nullptr;
    zm.command = nullptr;

    // One full-screen rect: the fullscreen composites (windrose/autopilot/
    // windsteer) and the hero-number numeric painter both key on w>=400 && h>=400.
    za->rects[0] = {0, 0, 480, 480};

    strncpy(za->screen_id, ZOOM_SCREEN_ID, STR_CAP - 1);
    strncpy(za->screen_title, "Zoom", STR_CAP - 1);
    za->spec.screen_id = za->screen_id;
    za->spec.title = za->screen_title;
    za->spec.template_id = TemplateId::QuadGrid;
    za->spec.metrics = za->metrics;
    za->spec.metric_count = 1;
    za->spec.variant_flags = 0;

    lv_obj_t *root = ui::layouts::create_freeform(nullptr, za->spec, za->rects);
    if (!root) {
        net::logf("[midl-render] zoom create_freeform failed");
        return;
    }
    // Make the whole view a tap-to-return target. The single tile cleared its own
    // CLICKABLE (zoomable=false above), so the tap lands on the root.
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, zoom_back_cb, LV_EVENT_CLICKED, nullptr);
    // Scroll hardening (Change B / cause 1): the transform-scaled hero glyph
    // (paint_numeric_body -> fit_hero_scale) can otherwise be counted into the
    // parent's scrollable content under LVGL v9, making the zoom screen
    // scroll/jitter even though SCROLLABLE was cleared in create_freeform. Force
    // NO scrolling on the zoom root (and disable chaining/elastic/momentum so a
    // swipe never starts a scroll on it). The single tile root + flex row + hero
    // label are hardened the same way in build_tile/paint_numeric_body.
    lv_obj_set_scroll_dir(root, LV_DIR_NONE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    za->root = root;
    za->live = true;

    ui::Screen scr = {};
    scr.id = za->screen_id;
    scr.title = za->screen_title;
    scr.root = root;
    scr.refresh = zoom_refresh;
    scr.hidden = true;  // not in the swipe rotation; reached only by a tile tap
    scr.build_fn = nullptr;
    scr.collect_paths = zoom_collect;

    if (!ui::replace_screen(ZOOM_SCREEN_ID, scr)) ui::register_screen(scr);
    ui::show_by_id(ZOOM_SCREEN_ID);
    net::logf("[midl-render] zoom -> '%s' (return='%s')", zm.id, s_zoom_return_id);
}

// ---------------------------------------------------------------------------
// apply_all: install EXACTLY the doc's screens, atomically replacing any
// previous MIDL screen set. Runs ON THE UI TASK.
//
// Ordering (CLAUDE.md screen-lifecycle fix):
//   1. PRE-VALIDATE: dry-count buildable object screens WITHOUT touching arenas
//      or LVGL. If zero, bail BEFORE any teardown so a bad push can't blank the
//      device (the current screen set stays live).
//   2. reset_screens(): tear down the previous MIDL screen set FIRST. This is
//      mandatory before rebuilding arenas: building writes into s_arenas[],
//      which the OLD screens' refresh trampolines (midl_refresh_n<N>) alias —
//      reset removes those screens so their trampolines never fire on the new,
//      half-built arena data. reset_screens() parks LVGL on a blank screen so
//      the invariant "always one active screen" holds during teardown.
//   3. Rebuild arenas[0..n-1] fresh and register each screen.
//   4. Show the default screen.
//
// Net effect: after apply_all the screen manager contains EXACTLY this doc's
// screens, in order, each with a correct arena + trampoline. A subsequent
// apply_all fully replaces them.
//
// Returns the number of screens successfully built.
// ---------------------------------------------------------------------------
size_t apply_all(JsonVariantConst doc) {
    if (!arenas()) return 0;  // PSRAM alloc failed; logged in arenas()

    // Install the fullscreen-zoom handler so a tap on any zoomable MIDL tile
    // (zoom_target == nullptr) builds the transient full-screen view. Idempotent;
    // the handler is a file-static here. reset_screens() below tears down any
    // prior __zoom__ screen, and the next tap rebuilds it fresh.
    ui::layouts::set_zoom_fullscreen_handler(zoom_to_fullscreen);
    // The prior __zoom__ screen (if any) is about to be torn down by
    // reset_screens(); drop the dangling arena root so a stale rebuild can't reuse
    // it. The arena is re-memset on the next tap anyway.
    if (s_zoom_arena) {
        s_zoom_arena->live = false;
        s_zoom_arena->root = nullptr;
    }

    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    if (screens.isNull()) {
        net::logf("[midl-render] apply_all: doc 'screens' is not an array");
        return 0;
    }

    // --- (1) PRE-VALIDATE before any teardown. A doc that yields 0 buildable
    // screens must NOT reset_screens() — that would blank the device. Bail and
    // keep the current screen set live. ---
    size_t usable = count_buildable_screens(screens);
    if (usable == 0) {
        net::logf("[midl-render] apply_all: 0 buildable screens; keeping current set");
        return 0;
    }

    // --- (2) Tear down the previous MIDL screen set FIRST. Must precede arena
    // rebuild: the old trampolines alias s_arenas[] and would fire on the
    // half-built new data otherwise. reset_screens() parks on a blank screen so
    // LVGL always has an active root during the teardown. ---
    ui::reset_screens();
    s_arena_count = 0;

    // --- (3) Build the doc's screens fresh into arena slots 0..n-1. ---
    size_t built = 0;
    for (JsonVariantConst sv : screens) {
        if (built >= ui::MAX_SCREENS) {
            net::logf("[midl-render] apply_all: more than %u screens; truncating",
                      (unsigned)ui::MAX_SCREENS);
            break;
        }
        if (!sv.is<JsonObjectConst>()) continue;

        const char *id = sv["id"] | (const char *)nullptr;
        char id_fallback[STR_CAP];
        if (!id || !id[0]) {
            // Synthesize a stable id so navigation still works.
            snprintf(id_fallback, sizeof(id_fallback), "midl%u", (unsigned)built);
            id = id_fallback;
        }

        if (build_screen_into(sv, id, s_arenas[built], built)) {
            ++built;
        }
    }

    s_arena_count = built;
    if (built == 0) {
        // Should not happen: pre-validation found >=1 buildable screen, but a
        // solve/build raced or failed. The old set is already gone (reset above)
        // and LVGL is parked on the blank screen — log and return.
        net::logf("[midl-render] apply_all: no screens built after reset (pre-validate said %u)",
                  (unsigned)usable);
        return 0;
    }

    // Show the default screen. defaultScreen may live under settings or at the
    // doc root; honor either. If absent/unmatched, the first registered screen
    // is already showing (register_screen auto-shows index 0).
    const char *def =
        doc["settings"]["defaultScreen"] | (doc["defaultScreen"] | (const char *)nullptr);
    if (def && def[0]) {
        ui::show_by_id(def);
    } else {
        // No explicit default: make sure a registered screen is actually shown.
        // After reset_screens() LVGL is parked on the blank root; the first
        // register_screen() auto-loads index 0, but force it here so a live-push
        // apply can never leave the device stranded on the parking screen (the
        // boot path has no parking root, so this is a no-op there).
        ui::show(0);
    }

    net::logf("[midl-render] apply_all built %zu screen(s)", built);
    return built;
}

// ---------------------------------------------------------------------------
// apply_doc: single-screen convenience. Runs ON THE UI TASK.
//
// Builds ALL screens in the doc via apply_all (so navigation between them works
// regardless of which entry point fired), then shows `screen_id`. If screen_id
// is null/empty/unmatched, apply_all's default-screen logic (or the first
// registered screen) governs which screen is visible.
//
// This keeps the `midl-render [screenId]` command and the ConfigApplyMidl pump
// case working while sharing the multi-screen build path — there is no longer a
// separate single-screen build, so a doc applied via either route registers all
// its screens.
//
// Returns true if at least one screen was built (and, when screen_id is given
// and matched, it was shown).
// ---------------------------------------------------------------------------
bool apply_doc(JsonVariantConst doc, const char *screen_id) {
    size_t built = apply_all(doc);
    if (built == 0) return false;

    if (screen_id && screen_id[0]) {
        if (!ui::show_by_id(screen_id)) {
            net::logf("[midl-render] apply_doc: screen '%s' not found; default shown", screen_id);
        }
    }
    return true;
}

}  // namespace midl::render
