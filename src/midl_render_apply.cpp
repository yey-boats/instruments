// midl_render_apply.cpp — MIDL doc -> freeform LVGL screen orchestration.
//
// Task 3 (Slice 2): apply_doc() runs on the UI task and turns a parsed MIDL
// document into a registered LVGL screen via:
//   solve_screen -> map_element (enum bridge) -> create_freeform -> register.
//
// Mirrors the legacy layout_renderer.cpp:145-233 flow but uses the MIDL
// solver instead of the editor JSON shape.
//
// DEVICE-ONLY: this TU includes <lvgl.h> (via ui_layouts.h) and ui_screens.h.
// It is NOT listed in the native build_src_filter — only in esp32-4848s040.
// The pure mapper (token_to_kind / map_element) lives in midl_render.cpp which
// IS in the native build.
//
// SINGLE-SCREEN LIMITATION (Slice 2): the session arena is a single
// function-static block. A second apply_doc() call rebuilds it in place,
// replacing the previous MIDL screen. Multi-screen support is deferred to
// Slice 5 (cutover). Document this clearly so the constraint is obvious.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_render.h"

#include "midl_solve.h"  // midl::solve_screen, PlacementSet, SolveStatus, SOLVE_OK
#include "ui_layouts.h"  // ui::layouts::create_freeform, update_freeform, ScreenVariantSpec, Rect
#include "ui_screens.h"  // ui::Screen, register_screen, replace_screen
#include "signalk.h"     // sk::copyData, sk::Data
#include "net.h"         // net::logf

#include <string.h>

namespace midl::render {

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::Rect;
using ui::layouts::ScreenVariantSpec;
using ui::layouts::TemplateId;

// ---------------------------------------------------------------------------
// Session-lived arena. Function-static; safe because apply_doc runs
// serially on the single LVGL task (and Slice 2 supports only one MIDL
// screen at a time — Slice 5 will move to a per-slot heap allocation).
//
// Layout: MetricBinding[] stores NON-OWNING const char* into id/label/unit
// arrays in this same arena, so they all outlive the screen. The spec and
// rect table are stored here too so the refresh trampoline can reach them
// without capturing any stack variable.
// ---------------------------------------------------------------------------

static constexpr size_t MAX_TILES = midl::FirmwareLimits::max_tiles_per_screen;  // 4
static constexpr size_t STR_CAP = midl::FirmwareLimits::str_len;                 // 32

struct MidlScreenArena {
    // String storage backing the MetricBinding non-owning pointers.
    char ids[MAX_TILES][STR_CAP];
    char labels[MAX_TILES][STR_CAP];
    char units[MAX_TILES][STR_CAP];

    // The MetricBinding table passed to create_freeform / update_freeform.
    MetricBinding metrics[MAX_TILES];

    // Pixel rects produced by the solver.
    Rect rects[MAX_TILES];

    // Screen id / title stored here so Screen.id/title point at this arena.
    char screen_id[STR_CAP];
    char screen_title[STR_CAP];

    // The ScreenVariantSpec used by create_freeform and the refresh trampoline.
    // .metrics points at this->metrics[]; .metric_count is set per apply_doc call.
    ScreenVariantSpec spec;

    // The most-recently built LVGL root.  The trampoline reads it from here.
    lv_obj_t *root;

    // true once the first apply_doc has succeeded and a screen is live.
    bool live;
};

static MidlScreenArena s_arena;

// ---------------------------------------------------------------------------
// Refresh trampoline — stored in ui::Screen.refresh (no user_data param).
// Reads directly from s_arena which is single-screen-safe for Slice 2.
// ---------------------------------------------------------------------------
static void midl_refresh() {
    if (!s_arena.live || !s_arena.root) return;
    sk::Data d;
    sk::copyData(d);
    ui::layouts::update_freeform(s_arena.root, s_arena.spec, d);
}

// ---------------------------------------------------------------------------
// Collect-paths trampoline for the per-screen subscription manager.
// ---------------------------------------------------------------------------
static void midl_collect_paths(sk::SubscriptionSet &out) {
    if (!s_arena.live) return;
    ui::layouts::collect_paths(s_arena.spec, out);
}

// ---------------------------------------------------------------------------
// apply_doc: main entry point.  Runs ON THE UI TASK (caller guarantees this).
// ---------------------------------------------------------------------------
bool apply_doc(JsonVariantConst doc, const char *screen_id) {
    // --- Step 1: locate the target screen in doc["screens"] ---
    JsonVariantConst screens_node = doc["screens"];
    JsonVariantConst screen_obj;

    if (!screens_node.is<JsonObjectConst>()) {
        net::logf("[midl-render] doc missing 'screens' object");
        return false;
    }

    if (screen_id && screen_id[0]) {
        screen_obj = screens_node[screen_id];
    }

    // Fallback: use the first screen when screen_id is null/missing/not found.
    if (!screen_obj.is<JsonObjectConst>()) {
        for (JsonPairConst kv : screens_node.as<JsonObjectConst>()) {
            if (kv.value().is<JsonObjectConst>()) {
                screen_obj = kv.value();
                // kv.key().c_str() is owned by `doc` and is valid for the lifetime of
                // this call; we copy it into s_arena.screen_id before doc could be freed.
                screen_id = kv.key().c_str();
                break;
            }
        }
    }

    if (!screen_obj.is<JsonObjectConst>()) {
        net::logf("[midl-render] no usable screen found in doc");
        return false;
    }

    // --- Step 2: solve layout ---
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

    // --- Step 3: map each placement into the session arena ---
    // Wipe the arena so stale data from a previous call doesn't leak.
    // IMPORTANT: memset the POD arena in-place (NOT `s_arena = MidlScreenArena{}`
    // — that would create a 34+ KB temporary on the task stack; see CLAUDE.md
    // "layout::parse() must memset(&out,0,sizeof(out))" trap).
    memset(&s_arena, 0, sizeof(s_arena));

    JsonVariantConst elements_node = screen_obj["elements"];

    size_t n = placements.count;
    if (n > MAX_TILES) n = MAX_TILES;

    for (size_t i = 0; i < n; ++i) {
        const midl::Placement &pl = placements.items[i];
        JsonVariantConst el = elements_node[pl.element];

        bool ok = map_element(el, pl.element, s_arena.metrics[i], s_arena.ids[i], s_arena.labels[i],
                              s_arena.units[i]);
        if (!ok) {
            // Unknown element: leave as zero-init MetricBinding (None source -> "--").
            // Copy at least the id so the tile chrome shows something.
            strncpy(s_arena.ids[i], pl.element, STR_CAP - 1);
            s_arena.ids[i][STR_CAP - 1] = 0;
            s_arena.metrics[i].id = s_arena.ids[i];
            s_arena.metrics[i].label = s_arena.ids[i];
            s_arena.metrics[i].unit = s_arena.units[i];  // "" (zero-init)
            s_arena.metrics[i].source = MetricSource::None;
        }

        // Build the Rect from solver output. Field order: x,y,w,h — matches ui::layouts::Rect.
        s_arena.rects[i] = {pl.rect.x, pl.rect.y, pl.rect.w, pl.rect.h};
    }

    // --- Step 4: build the ScreenVariantSpec ---
    strncpy(s_arena.screen_id, screen_id, STR_CAP - 1);
    s_arena.screen_id[STR_CAP - 1] = 0;

    // Title: prefer doc-level title field, else screen_id.
    const char *title_src = screen_obj["title"] | screen_id;
    strncpy(s_arena.screen_title, title_src, STR_CAP - 1);
    s_arena.screen_title[STR_CAP - 1] = 0;

    s_arena.spec.screen_id = s_arena.screen_id;
    s_arena.spec.title = s_arena.screen_title;
    s_arena.spec.template_id = TemplateId::QuadGrid;  // irrelevant; create_freeform called directly
    s_arena.spec.metrics = s_arena.metrics;
    s_arena.spec.metric_count = (uint8_t)n;
    s_arena.spec.variant_flags = 0;

    // --- Step 5: build the LVGL freeform screen ---
    lv_obj_t *root = ui::layouts::create_freeform(nullptr, s_arena.spec, s_arena.rects);
    if (!root) {
        net::logf("[midl-render] create_freeform returned null");
        return false;
    }
    s_arena.root = root;
    s_arena.live = true;

    // --- Step 6: fill a ui::Screen and register/replace ---
    ui::Screen scr = {};
    scr.id = s_arena.screen_id;
    scr.title = s_arena.screen_title;
    scr.root = root;
    scr.refresh = midl_refresh;
    scr.hidden = false;
    scr.build_fn = nullptr;  // eager: root is pre-supplied
    scr.collect_paths = midl_collect_paths;

    // Replace if this id is already registered; otherwise add it as a new screen.
    // NOTE: replace_screen copies scr.collect_paths from the Screen struct, so no
    // separate set_screen_collect_paths call is needed here — it would cause a
    // spurious subscription re-diff on every hot-reload.
    if (!ui::replace_screen(s_arena.screen_id, scr)) {
        ui::register_screen(scr);
        net::logf("[midl-render] registered new screen '%s' (%zu tiles)", s_arena.screen_id, n);
    } else {
        net::logf("[midl-render] replaced screen '%s' (%zu tiles)", s_arena.screen_id, n);
    }

    return true;
}

}  // namespace midl::render
