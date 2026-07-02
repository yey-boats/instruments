#include "layout_renderer.h"
#include "ui_layouts.h"  // lv_obj_t* builders + full LVGL types

#include "layout_loader.h"
#include "subscription_set.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_data.h"
#include "signalk.h"
#include "net.h"

#include <string.h>

namespace ui::layout_render {

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::ScreenVariantSpec;
using ui::layouts::TemplateId;
using ui::layouts::WidgetKind;

// Native unit suffix for an editor-pushed tile that didn't carry one. The
// device renders values in fixed SI-derived units (kn / m / nm / °C / ...),
// so a tile's unit label is fully determined by its bound source. Sources
// whose formatted value already embeds its qualifier (AWA/TWA carry a P/S
// side letter, SOC and XTE embed % / side, Position/APState are text) return
// "" so we never double-print. Mirrors the unit strings the built-in
// QuadGrid presets assign by hand.
static const char *unit_for_source(MetricSource s) {
    switch (s) {
    case MetricSource::AWS_kn:
    case MetricSource::TWS_kn:
    case MetricSource::SOG_kn:
    case MetricSource::VMG_kn:
        return "kn";
    case MetricSource::COG_deg:
    case MetricSource::HDG_deg:
    case MetricSource::BTW_deg:
    case MetricSource::CTS_deg:
        return "\xC2\xB0";  // UTF-8 degree sign
    case MetricSource::Depth_m:
    case MetricSource::DepthKeel_m:
    case MetricSource::XTE:
        return "m";
    case MetricSource::WaterTemp_C:
        return "\xC2\xB0\x43";  // °C
    case MetricSource::BatteryV:
        return "V";
    case MetricSource::DTW:
        return "nm";
    default:
        return "";  // AWA/TWA/SOC (embedded qualifier), Position, APState, None
    }
}

// Bounded pool: one renderer slot per layout::MAX_SCREENS so the
// MetricBinding tables stay alive for the screen's lifetime without
// dynamic allocation each apply.
struct RendererSlot {
    char screen_id[layout::STR_LEN];
    char screen_title[layout::STR_LEN];
    MetricBinding tiles[layout::MAX_TILES_PER_SCREEN];
    char tile_ids[layout::MAX_TILES_PER_SCREEN][layout::STR_LEN];
    char tile_labels[layout::MAX_TILES_PER_SCREEN][layout::STR_LEN];
    ScreenVariantSpec spec;
    bool in_use;
};

static RendererSlot s_slots[layout::MAX_SCREENS];

// Owning state for a built layout screen: keeps the spec + bindings
// alive for the screen's lifetime and gives the refresh callback a
// stable pointer to feed update().
struct LayoutScreenState {
    lv_obj_t *root;
    RendererSlot *slot;
};

static LayoutScreenState s_states[layout::MAX_SCREENS];

// Refresh trampolines - one per slot since ui::Screen.refresh has no
// user_data parameter. The slot index is baked into each function.
template <int N> static void refresh_slot() {
    if (!s_states[N].root || !s_states[N].slot) return;
    boat::View d;
    boat::current_view(d);
    ui::layouts::update(s_states[N].root, s_states[N].slot->spec, d);
}

static void (*const refresh_fns[layout::MAX_SCREENS])() = {
    refresh_slot<0>, refresh_slot<1>, refresh_slot<2>, refresh_slot<3>,
    refresh_slot<4>, refresh_slot<5>, refresh_slot<6>, refresh_slot<7>,
};

// Slice 3: per-slot path collectors. An authored screen subscribes exactly the
// paths its tiles bind (resolved through source_to_path) so the subscription
// manager unsubscribes everything else when it's shown.
template <int N> static void collect_slot(sk::SubscriptionSet &out) {
    if (!s_states[N].slot) return;
    ui::layouts::collect_paths(s_states[N].slot->spec, out);
}

static const ui::CollectPathsFn collect_fns[layout::MAX_SCREENS] = {
    collect_slot<0>, collect_slot<1>, collect_slot<2>, collect_slot<3>,
    collect_slot<4>, collect_slot<5>, collect_slot<6>, collect_slot<7>,
};

static lv_obj_t *build_slot(RendererSlot *slot, size_t slot_index) {
    lv_obj_t *root = ui::layouts::create(nullptr, slot->spec);
    s_states[slot_index].root = root;
    s_states[slot_index].slot = slot;
    return root;
}

size_t apply() {
    if (!layout::loaded()) return 0;
    const layout::Config &cfg = layout::current();
    size_t replaced = 0;

    // Walk every layout screen and translate tiles into a slot. Skip
    // screens that have no editor-style widget bindings (so the user's
    // hardcoded screen keeps rendering its own tiles).
    for (size_t i = 0; i < cfg.screen_count && i < layout::MAX_SCREENS; ++i) {
        const layout::Screen &ls = cfg.screens[i];
        if (ls.tile_count == 0) continue;

        // Detect editor-shape: at least one tile has a `widget` string.
        bool editor_shape = false;
        for (size_t t = 0; t < ls.tile_count && !editor_shape; ++t)
            if (ls.tiles[t].widget[0]) editor_shape = true;
        if (!editor_shape) continue;

        RendererSlot &slot = s_slots[i];
        strncpy(slot.screen_id, ls.id, sizeof(slot.screen_id) - 1);
        slot.screen_id[sizeof(slot.screen_id) - 1] = 0;
        strncpy(slot.screen_title, ls.title[0] ? ls.title : ls.id, sizeof(slot.screen_title) - 1);
        slot.screen_title[sizeof(slot.screen_title) - 1] = 0;

        uint8_t count = ls.tile_count < layout::MAX_TILES_PER_SCREEN
                            ? (uint8_t)ls.tile_count
                            : (uint8_t)layout::MAX_TILES_PER_SCREEN;
        for (uint8_t t = 0; t < count; ++t) {
            const layout::Tile &lt = ls.tiles[t];
            MetricBinding &mb = slot.tiles[t];
            mb = MetricBinding{};
            // Tile caption: prefer explicit title, then last segment of path.
            const char *label = lt.title[0] ? lt.title : lt.id;
            strncpy(slot.tile_labels[t], label, sizeof(slot.tile_labels[t]) - 1);
            slot.tile_labels[t][sizeof(slot.tile_labels[t]) - 1] = 0;
            // Per the MetricBinding contract (ui_layouts_types.h) `id` is the
            // TILE's stable id, not the screen's. Copied into the slot so the
            // pointer outlives the (replaceable) layout::Config source.
            strncpy(slot.tile_ids[t], lt.id, sizeof(slot.tile_ids[t]) - 1);
            slot.tile_ids[t][sizeof(slot.tile_ids[t]) - 1] = 0;
            mb.id = slot.tile_ids[t];
            mb.label = slot.tile_labels[t];
            mb.source = path_to_source(lt.primary_path);
            // Editor-pushed tiles carry no unit string; derive the native unit
            // from the bound source so SOG/DEPTH/BATT/etc. are not unitless on
            // the panel (the built-in presets set this by hand).
            mb.unit = unit_for_source(mb.source);
            mb.accent = 0x57c7d8;
            mb.target_screen = nullptr;
            mb.extras_count = 0;
            mb.kind = widget_to_kind(lt.widget);
            // Slice 6: carry the authored per-field zoom. `zoom` points into
            // the live (PSRAM) layout::Config, which outlives this screen, so
            // the tap handler reads the target string directly. zoom_target is
            // ALWAYS non-NULL for an authored tile (a parsed "" string) so the
            // tap handler's NULL == "legacy hardcoded tile" sentinel never
            // triggers here; an authored non-zoomable tile resolves to
            // ZOOM_NONE instead of the legacy auto-zoom fallback.
            mb.zoomable = lt.zoomable;
            mb.zoom_target = lt.zoom;
        }
        slot.spec.screen_id = slot.screen_id;
        slot.spec.title = slot.screen_title;
        slot.spec.template_id = TemplateId::QuadGrid;
        slot.spec.metrics = slot.tiles;
        slot.spec.metric_count = count;
        slot.spec.variant_flags = 0;
        slot.in_use = true;

        // Build the new root from the slot and replace the registered screen.
        lv_obj_t *root = build_slot(&slot, i);
        if (!root) continue;
        ui::Screen new_screen = {};
        new_screen.id = slot.screen_id;
        new_screen.title = slot.screen_title;
        new_screen.root = root;
        new_screen.refresh = refresh_fns[i];
        new_screen.hidden = false;
        new_screen.build_fn = nullptr;
        if (ui::replace_screen(ls.id, new_screen)) {
            ui::set_screen_collect_paths(slot.screen_id, collect_fns[i]);
            ++replaced;
            net::logf("[layout-render] replaced screen %s (%u tiles, editor-shape)", ls.id,
                      (unsigned)count);
        }
    }
    return replaced;
}

}  // namespace ui::layout_render
