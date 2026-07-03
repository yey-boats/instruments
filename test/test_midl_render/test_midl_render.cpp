#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "midl_render.h"
#include "layout.h"           // layout::zoom_action / ZOOM_NONE — the interactivity gate
#include "layout_renderer.h"  // ui::layout_render::path_to_source (enum bridge checks)

using ui::layouts::MetricBinding;
using ui::layouts::MetricSource;
using ui::layouts::WidgetKind;

// NOTE: mapOne returns a MetricBinding whose id/label/unit pointers refer to
// function-static buffers (idb/lab/unit). Do NOT call mapOne twice and compare
// both results — the second call overwrites those buffers, invalidating the first
// MetricBinding's string pointers.
static ui::layouts::DialMarker s_marker_slots[ui::layouts::MAX_DIAL_MARKERS];
static ui::layouts::DialSector s_sector_slots[2 * ui::layouts::MAX_DIAL_SECTORS];
static char s_style_paths[4][96];

static MetricBinding mapOne(const char *json, const char *id) {
    JsonDocument doc;
    deserializeJson(doc, json);
    static char idb[32], lab[32], unit[32], act[32], zoom[32], path[96], dir[96], aval[32];
    static midl::render::StyleAlloc style;
    style = midl::render::StyleAlloc{};
    style.markers = s_marker_slots;
    style.marker_cap = ui::layouts::MAX_DIAL_MARKERS;
    style.sectors = s_sector_slots;
    style.sector_cap = 2 * ui::layouts::MAX_DIAL_SECTORS;
    style.paths = s_style_paths;
    style.path_cap = 4;
    MetricBinding mb{};
    midl::render::map_element(doc.as<JsonVariantConst>(), id, mb, idb, lab, unit, act, zoom, path,
                              dir, aval, &style);
    return mb;
}

// Legacy 8-arg call (no path buffers) — mirrors sim_midl.cpp; dynamic paths
// must degrade to NULL (source None -> "--"), never dangle.
static MetricBinding mapOneNoPathBufs(const char *json, const char *id) {
    JsonDocument doc;
    deserializeJson(doc, json);
    static char idb[32], lab[32], unit[32], act[32], zoom[32];
    MetricBinding mb{};
    midl::render::map_element(doc.as<JsonVariantConst>(), id, mb, idb, lab, unit, act, zoom);
    return mb;
}

void test_token_to_kind() {
    TEST_ASSERT_EQUAL(WidgetKind::Numeric, midl::render::token_to_kind("single-value"));
    TEST_ASSERT_EQUAL(WidgetKind::WindRose, midl::render::token_to_kind("windrose"));
    TEST_ASSERT_EQUAL(WidgetKind::WindSteer, midl::render::token_to_kind("windsteer"));
    TEST_ASSERT_EQUAL(WidgetKind::Compass, midl::render::token_to_kind("compass"));
    TEST_ASSERT_EQUAL(WidgetKind::Gauge, midl::render::token_to_kind("gauge"));
    // Firmware extension token (not yet in the midl catalog — upstream follow-up).
    TEST_ASSERT_EQUAL(WidgetKind::Clinometer, midl::render::token_to_kind("clinometer"));
    TEST_ASSERT_EQUAL(WidgetKind::Numeric, midl::render::token_to_kind("frobnicate"));
}

void test_map_clinometer_binds_attitude_roll() {
    // The attitude object path bridges to the Roll_deg enum source (the
    // clinometer's primary reading); pitch rides the painter's secondary.
    MetricBinding m = mapOne(
        R"({"type":"clinometer","name":"HEEL",
            "bindings":{"value":{"kind":"signalk","path":"navigation.attitude"}}})",
        "heel");
    TEST_ASSERT_EQUAL(WidgetKind::Clinometer, m.kind);
    TEST_ASSERT_EQUAL(MetricSource::Roll_deg, m.source);
    TEST_ASSERT_NULL(m.path);  // enum hit -> no dynamic-path retention
}

void test_map_rudder_center_zero_bar() {
    // E2: rudder as a centre-zero strip — the bar + style.center pattern
    // covers it with no dedicated painter (update_gauge_bar center branch).
    // style.range is the schema-canonical spelling (library docs use it).
    MetricBinding m = mapOne(
        R"({"type":"bar","name":"RUDDER","format":{"unit":"deg","decimals":0},
            "style":{"range":[-35,35],"center":0},
            "bindings":{"value":{"kind":"signalk","path":"steering.rudderAngle"}}})",
        "rud");
    TEST_ASSERT_EQUAL(WidgetKind::Bar, m.kind);
    TEST_ASSERT_EQUAL(MetricSource::Rudder_deg, m.source);
    TEST_ASSERT_TRUE(m.center_bar);
    TEST_ASSERT_EQUAL_FLOAT(-35.0f, m.range_min);
    TEST_ASSERT_EQUAL_FLOAT(35.0f, m.range_max);
}

void test_style_range_fallback_and_format_range_wins() {
    // format.range (legacy editor spelling) wins over style.range when both
    // are authored; style.range alone is honored (demo-doc Engine gauges).
    MetricBinding a = mapOne(
        R"({"type":"gauge","format":{"range":[0,10]},"style":{"range":[0,99]},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "g1");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.range_min);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, a.range_max);
    MetricBinding b = mapOne(
        R"({"type":"gauge","style":{"range":[0,3600]},
            "bindings":{"value":{"kind":"signalk","path":"propulsion.main.revolutions"}}})",
        "rpm");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b.range_min);
    TEST_ASSERT_EQUAL_FLOAT(3600.0f, b.range_max);
}

void test_engine_paths_bridge_to_enum_sources() {
    // The MIDL library Engine screen binds propulsion.main.* — every path must
    // hit the typed enum bridge so the tiles read the fused Snapshot fields.
    using ui::layout_render::path_to_source;
    TEST_ASSERT_EQUAL(MetricSource::EngineRpm, path_to_source("propulsion.main.revolutions"));
    TEST_ASSERT_EQUAL(MetricSource::EngineCoolant_C, path_to_source("propulsion.main.temperature"));
    TEST_ASSERT_EQUAL(MetricSource::EngineOilP_bar, path_to_source("propulsion.main.oilPressure"));
    TEST_ASSERT_EQUAL(MetricSource::EngineFuelRate_lph,
                      path_to_source("propulsion.main.fuel.rate"));
    // A non-canonical instance stays dynamic (raw path retained + subscribed).
    TEST_ASSERT_EQUAL(MetricSource::None, path_to_source("propulsion.port.revolutions"));
}

void test_map_single_value() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"SOG","format":{"unit":"kn"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_EQUAL(WidgetKind::Numeric, m.kind);
    TEST_ASSERT_EQUAL_STRING("SOG", m.label);
    TEST_ASSERT_EQUAL_STRING("kn", m.unit);
    TEST_ASSERT_EQUAL_STRING("sog", m.id);
    TEST_ASSERT_EQUAL(MetricSource::SOG_kn, m.source);
}

void test_map_unknown_path_is_none() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"made.up.path"}}})",
        "x");
    TEST_ASSERT_EQUAL(MetricSource::None, m.source);
    // Audit item 8: the raw path is RETAINED for the dynamic PathStore
    // resolver instead of dropping the tile to a dead "--".
    TEST_ASSERT_NOT_NULL(m.path);
    TEST_ASSERT_EQUAL_STRING("made.up.path", m.path);
    TEST_ASSERT_TRUE(m.zoomable);     // a dynamic tile is a real value tile
    TEST_ASSERT_NULL(m.zoom_target);  // fullscreen-self
}

void test_map_unknown_path_without_buf_degrades() {
    MetricBinding m = mapOneNoPathBufs(
        R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"made.up.path"}}})",
        "x");
    TEST_ASSERT_EQUAL(MetricSource::None, m.source);
    TEST_ASSERT_NULL(m.path);  // no caller buffer -> no dangling retention
    TEST_ASSERT_FALSE(m.zoomable);
}

void test_map_known_path_does_not_retain() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_EQUAL(MetricSource::SOG_kn, m.source);
    TEST_ASSERT_NULL(m.path);  // enum bridge hit: typed fast path stays
}

void test_map_compass_label_fallback_to_id() {
    MetricBinding m = mapOne(
        R"({"type":"compass","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}}})",
        "hdg");
    TEST_ASSERT_EQUAL(WidgetKind::Compass, m.kind);
    TEST_ASSERT_EQUAL_STRING("hdg", m.label);  // no name -> label falls back to id
}

void test_accent_hex_string() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","style":{"color":"#1a2b3c"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog_color");
    TEST_ASSERT_EQUAL_HEX32(0x1A2B3Cu, m.accent);
}

void test_accent_integer() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","style":{"color":255},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog_intcolor");
    TEST_ASSERT_EQUAL_HEX32(255u, m.accent);
}

// A gauge bound to the rudder with an explicit format.range + precision should
// carry both through to the painter binding.
void test_format_range_and_precision() {
    MetricBinding m = mapOne(
        R"({"type":"gauge","name":"RUDDER","format":{"range":[-35,35],"precision":1,"unit":"deg"},
            "bindings":{"value":{"kind":"signalk","path":"steering.rudderAngle"}}})",
        "rudder");
    TEST_ASSERT_EQUAL(WidgetKind::Gauge, m.kind);
    TEST_ASSERT_EQUAL(MetricSource::Rudder_deg, m.source);
    TEST_ASSERT_EQUAL_FLOAT(-35.0f, m.range_min);
    TEST_ASSERT_EQUAL_FLOAT(35.0f, m.range_max);
    TEST_ASSERT_EQUAL_INT8(1, m.precision);
    TEST_ASSERT_EQUAL_STRING("deg", m.unit);
}

// Absent format.range/precision must leave the additive fields at their defaults
// (range_min==range_max==0, precision==-1) so legacy painter behavior is preserved.
void test_format_absent_defaults() {
    MetricBinding m = mapOne(
        R"({"type":"gauge","format":{"unit":"kn"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog_gauge");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.range_min);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.range_max);
    TEST_ASSERT_EQUAL_INT8(-1, m.precision);
}

void test_action_nav_sets_target_screen() {
    MetricBinding m = mapOne(
        R"({"type":"button","name":"NAV","action":{"kind":"nav","target":"steering"}})", "go");
    TEST_ASSERT_EQUAL(WidgetKind::Button, m.kind);
    TEST_ASSERT_NOT_NULL(m.target_screen);
    TEST_ASSERT_EQUAL_STRING("steering", m.target_screen);
    TEST_ASSERT_NULL(m.command);
}

void test_action_command_sets_command() {
    MetricBinding m = mapOne(
        R"({"type":"button","name":"TACK","action":{"kind":"command","target":"tack"}})", "tk");
    TEST_ASSERT_EQUAL(WidgetKind::Button, m.kind);
    TEST_ASSERT_NOT_NULL(m.command);
    TEST_ASSERT_EQUAL_STRING("tack", m.command);
    TEST_ASSERT_NULL(m.target_screen);
}

void test_action_put_kind_rides_command_slot() {
    // Race-screen tack dialect: action.kind "put" (SignalK PUT to a dotted
    // path) maps onto the command slot; button_action_cb routes a dotted,
    // space-free target through the SignalK PUT queue at dispatch time.
    MetricBinding m = mapOne(
        R"({"type":"button","name":"TACK",
            "action":{"kind":"put","target":"steering.autopilot.tack"}})",
        "tk");
    TEST_ASSERT_NOT_NULL(m.command);
    TEST_ASSERT_EQUAL_STRING("steering.autopilot.tack", m.command);
    TEST_ASSERT_NULL(m.target_screen);
}

void test_action_command_dotted_target_retained_verbatim() {
    // The demo doc authors kind "command" with a dotted SignalK path — the
    // mapper must retain it untouched (the PUT-vs-console split happens in
    // the tap handler, keyed on '.' + no space).
    MetricBinding m = mapOne(
        R"({"type":"button","action":{"kind":"command","target":"steering.autopilot.tack"}})",
        "tk");
    TEST_ASSERT_NOT_NULL(m.command);
    TEST_ASSERT_EQUAL_STRING("steering.autopilot.tack", m.command);
}

void test_action_absent_leaves_both_null() {
    MetricBinding m = mapOne(R"({"type":"button","name":"PLAIN"})", "p");
    TEST_ASSERT_NULL(m.command);
    TEST_ASSERT_NULL(m.target_screen);
}

void test_action_unknown_kind_ignored() {
    MetricBinding m =
        mapOne(R"({"type":"button","action":{"kind":"frobnicate","target":"whatever"}})", "x");
    TEST_ASSERT_NULL(m.command);
    TEST_ASSERT_NULL(m.target_screen);
}

// --- presentation-parity fields (audit wave 2) ------------------------------

void test_format_decimals_alias() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","format":{"decimals":2},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_EQUAL_INT8(2, m.precision);
}

void test_format_decimals_wins_over_precision() {
    // `decimals` is the canonical MIDL key (web formatValue reads only it);
    // `precision` is the firmware-historical alias.
    MetricBinding m = mapOne(
        R"({"type":"single-value","format":{"decimals":3,"precision":0},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_EQUAL_INT8(3, m.precision);
}

void test_dir_binding_enum_bridge() {
    MetricBinding m = mapOne(
        R"({"type":"compass",
            "bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"},
                        "dir":{"kind":"signalk","path":"navigation.courseRhumbline.bearingTrackTrue"}}})",
        "hdg");
    TEST_ASSERT_EQUAL(MetricSource::HDG_deg, m.source);
    TEST_ASSERT_EQUAL(MetricSource::CTS_deg, m.dir_source);
    TEST_ASSERT_NULL(m.dir_path);
}

void test_dir_binding_dynamic_path() {
    MetricBinding m = mapOne(
        R"({"type":"windrose",
            "bindings":{"value":{"kind":"signalk","path":"environment.wind.speedApparent"},
                        "dir":{"kind":"signalk","path":"environment.current.setTrue"}}})",
        "rose");
    TEST_ASSERT_EQUAL(MetricSource::AWS_kn, m.source);
    TEST_ASSERT_EQUAL(MetricSource::None, m.dir_source);
    TEST_ASSERT_NOT_NULL(m.dir_path);
    TEST_ASSERT_EQUAL_STRING("environment.current.setTrue", m.dir_path);
}

void test_dir_absent_defaults() {
    MetricBinding m = mapOne(
        R"({"type":"compass","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}}})",
        "hdg");
    TEST_ASSERT_EQUAL(MetricSource::None, m.dir_source);
    TEST_ASSERT_NULL(m.dir_path);
}

void test_side_flag_bool_and_string() {
    MetricBinding a = mapOne(
        R"({"type":"single-value","format":{"side":true},
            "bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.crossTrackError"}}})",
        "xte");
    TEST_ASSERT_TRUE(a.side);
    MetricBinding b = mapOne(
        R"({"type":"single-value","format":{"side":"port-stbd"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.crossTrackError"}}})",
        "xte");
    TEST_ASSERT_TRUE(b.side);
    MetricBinding c = mapOne(
        R"({"type":"single-value","format":{"side":false},
            "bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.crossTrackError"}}})",
        "xte");
    TEST_ASSERT_FALSE(c.side);
}

void test_size_roles() {
    const char *tpl =
        R"({"type":"single-value","style":{"size":"%s"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})";
    struct Case {
        const char *role;
        uint8_t want;
    };
    static const Case cases[] = {{"S", 1}, {"M", 2}, {"L", 3}, {"XL", 4}, {"Fill", 5}};
    for (const Case &c : cases) {
        char json[256];
        snprintf(json, sizeof(json), tpl, c.role);
        MetricBinding m = mapOne(json, "sog");
        TEST_ASSERT_EQUAL_UINT8(c.want, m.size_role);
    }
    // Legacy numeric px size stays 0 = auto ladder.
    MetricBinding m = mapOne(
        R"({"type":"single-value","style":{"size":40},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_EQUAL_UINT8(0, m.size_role);
}

void test_center_flag() {
    MetricBinding a = mapOne(
        R"({"type":"bar","style":{"center":0.5},
            "bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.crossTrackError"}}})",
        "xte");
    TEST_ASSERT_TRUE(a.center_bar);
    MetricBinding b = mapOne(
        R"({"type":"bar","bindings":{"value":{"kind":"signalk","path":"navigation.courseRhumbline.crossTrackError"}}})",
        "xte");
    TEST_ASSERT_FALSE(b.center_bar);
}

void test_zones_parsed() {
    MetricBinding m = mapOne(
        R"({"type":"gauge","format":{"range":[0,30],"unit":"m"},
            "style":{"zones":[{"lt":5,"color":"alarm"},{"lt":10,"color":"warn"},
                              {"lt":99,"color":"#00ff00"}]},
            "bindings":{"value":{"kind":"signalk","path":"environment.depth.belowTransducer"}}})",
        "depth");
    TEST_ASSERT_EQUAL_UINT8(3, m.zone_count);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, m.zones[0].lt);
    TEST_ASSERT_EQUAL(ui::layouts::ZoneColor::Alarm, m.zones[0].color);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, m.zones[1].lt);
    TEST_ASSERT_EQUAL(ui::layouts::ZoneColor::Warn, m.zones[1].color);
    TEST_ASSERT_EQUAL(ui::layouts::ZoneColor::Literal, m.zones[2].color);
    TEST_ASSERT_EQUAL_HEX32(0x00FF00u, m.zones[2].rgb);
}

void test_zones_malformed_entries_skipped() {
    // Missing/non-numeric lt entries are skipped; the cap holds at 4.
    MetricBinding m = mapOne(
        R"({"type":"gauge","style":{"zones":[{"color":"warn"},{"lt":"x","color":"warn"},
             {"lt":1,"color":"good"},{"lt":2,"color":"warn"},{"lt":3,"color":"alarm"},
             {"lt":4,"color":"good"},{"lt":5,"color":"warn"}]},
            "bindings":{"value":{"kind":"signalk","path":"environment.depth.belowTransducer"}}})",
        "depth");
    TEST_ASSERT_EQUAL_UINT8(4, m.zone_count);  // MAX_METRIC_ZONES
    TEST_ASSERT_EQUAL_FLOAT(1.0f, m.zones[0].lt);
}

void test_zones_absent_defaults() {
    MetricBinding m = mapOne(
        R"({"type":"gauge","bindings":{"value":{"kind":"signalk","path":"environment.depth.belowTransducer"}}})",
        "depth");
    TEST_ASSERT_EQUAL_UINT8(0, m.zone_count);
    TEST_ASSERT_FALSE(m.side);
    TEST_ASSERT_EQUAL_UINT8(0, m.size_role);
    TEST_ASSERT_FALSE(m.center_bar);
}

// --- pure presentation helpers (ui_layouts_types.h) -------------------------

void test_zone_pick_helper() {
    ui::layouts::MetricZone z[3] = {{5.0f, 0, ui::layouts::ZoneColor::Alarm},
                                    {10.0f, 0, ui::layouts::ZoneColor::Warn},
                                    {99.0f, 0, ui::layouts::ZoneColor::Good}};
    TEST_ASSERT_EQUAL_INT(0, ui::layouts::zone_pick(z, 3, 2.0));    // below first lt
    TEST_ASSERT_EQUAL_INT(1, ui::layouts::zone_pick(z, 3, 5.0));    // lt is exclusive
    TEST_ASSERT_EQUAL_INT(2, ui::layouts::zone_pick(z, 3, 50.0));   // top bucket
    TEST_ASSERT_EQUAL_INT(-1, ui::layouts::zone_pick(z, 3, 99.0));  // above all
    TEST_ASSERT_EQUAL_INT(-1, ui::layouts::zone_pick(z, 3, NAN));
}

void test_convert_si_display_helper() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 1.943844492, ui::layouts::convert_si_display(1.0, "kn"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 180.0,
                              ui::layouts::convert_si_display(3.14159265358979323846, "deg"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 50.0, ui::layouts::convert_si_display(0.5, "%"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, ui::layouts::convert_si_display(1852.0, "nm"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 26.85, ui::layouts::convert_si_display(300.0, "C"));
    // Unknown / absent unit renders the raw value.
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 7.5, ui::layouts::convert_si_display(7.5, "frobs"));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 7.5, ui::layouts::convert_si_display(7.5, nullptr));
}

void test_format_side_value_helper() {
    char buf[24];
    // Angle: wrapped to [-180,180]; positive -> S, negative -> P (web parity:
    // 270deg == -90 -> "90P"; 42 -> "42S").
    ui::layouts::format_side_value(270.0, true, -1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("90P", buf);
    ui::layouts::format_side_value(42.0, true, -1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("42S", buf);
    ui::layouts::format_side_value(-42.0, true, -1, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("42P", buf);
    // Plain offset: |v| at precision decimals + side from the sign.
    ui::layouts::format_side_value(-0.126, false, 2, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0.13P", buf);
    ui::layouts::format_side_value(0.126, false, 2, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("0.13S", buf);
    ui::layouts::format_side_value(NAN, false, 2, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("--", buf);
}

void test_unit_is_angle_helper() {
    TEST_ASSERT_TRUE(ui::layouts::unit_is_angle("deg"));
    TEST_ASSERT_TRUE(ui::layouts::unit_is_angle("rad"));
    TEST_ASSERT_TRUE(ui::layouts::unit_is_angle(""));  // web: null unit -> angle
    TEST_ASSERT_TRUE(ui::layouts::unit_is_angle(nullptr));
    TEST_ASSERT_FALSE(ui::layouts::unit_is_angle("kn"));
    TEST_ASSERT_FALSE(ui::layouts::unit_is_angle("nm"));
}

// --- zoom field -----------------------------------------------------------

void test_zoom_default_value_tile_is_zoomable_self() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"SOG","format":{"unit":"kn"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_TRUE(m.zoomable);
    TEST_ASSERT_NULL(m.zoom_target);  // fullscreen-self
}

// Non-zoomable MIDL tiles must NOT leave zoom_target == nullptr: that falls into
// create_freeform's legacy `source != None` interactivity fallback and would zoom
// anyway. They get an EMPTY (non-null) zoom_target so zoom_action() == ZOOM_NONE.
void test_zoom_button_not_zoomable_by_default() {
    MetricBinding m = mapOne(R"({"type":"button","name":"TACK"})", "tk");
    TEST_ASSERT_FALSE(m.zoomable);
    TEST_ASSERT_NOT_NULL(m.zoom_target);
    TEST_ASSERT_EQUAL_STRING("", m.zoom_target);
    TEST_ASSERT_EQUAL(layout::ZOOM_NONE, layout::zoom_action(m.zoomable, m.zoom_target));
}

void test_zoom_sourceless_tile_not_zoomable() {
    // No value binding -> source None -> not zoomable.
    MetricBinding m = mapOne(R"({"type":"single-value","name":"X"})", "x");
    TEST_ASSERT_FALSE(m.zoomable);
    TEST_ASSERT_NOT_NULL(m.zoom_target);
    TEST_ASSERT_EQUAL_STRING("", m.zoom_target);
    TEST_ASSERT_EQUAL(layout::ZOOM_NONE, layout::zoom_action(m.zoomable, m.zoom_target));
}

void test_zoom_false_disables() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"SOG","zoom":false,
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_FALSE(m.zoomable);
    TEST_ASSERT_NOT_NULL(m.zoom_target);
    TEST_ASSERT_EQUAL_STRING("", m.zoom_target);
    TEST_ASSERT_EQUAL(layout::ZOOM_NONE, layout::zoom_action(m.zoomable, m.zoom_target));
}

void test_zoom_true_keeps_self() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"SOG","zoom":true,
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_TRUE(m.zoomable);
    TEST_ASSERT_NULL(m.zoom_target);
}

void test_zoom_string_sets_target_screen() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"SOG","zoom":"speed_detail",
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_TRUE(m.zoomable);
    TEST_ASSERT_NOT_NULL(m.zoom_target);
    TEST_ASSERT_EQUAL_STRING("speed_detail", m.zoom_target);
}

// --- dial-fidelity wave: markers / sectors / hull / band / const / local ----

void test_markers_parsed() {
    MetricBinding m = mapOne(
        R"({"type":"compass",
            "bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}},
            "markers":[
              {"glyph":"diamond","color":"#ff0000",
               "dir":{"kind":"signalk","path":"navigation.courseOverGroundTrue"}},
              {"dir":{"kind":"signalk","path":"environment.current.setTrue"},
               "kind":"vector","color":"warn"},
              {"glyph":"frobnicate","dir":{"kind":"const","value":135}}
            ]})",
        "hdg");
    TEST_ASSERT_EQUAL_UINT8(3, m.marker_count);
    TEST_ASSERT_NOT_NULL(m.markers);
    // [0]: explicit glyph + literal colour + enum-bridge dir.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ui::GlyphId::Diamond, m.markers[0].glyph);
    TEST_ASSERT_EQUAL(ui::layouts::ZoneColor::Literal, m.markers[0].color);
    TEST_ASSERT_EQUAL_HEX32(0xFF0000u, m.markers[0].rgb);
    TEST_ASSERT_EQUAL(MetricSource::COG_deg, m.markers[0].dir_source);
    TEST_ASSERT_NULL(m.markers[0].dir_path);
    TEST_ASSERT_FALSE(m.markers[0].vector);
    // [1]: default glyph triangle, warn token, vector kind, dynamic dir path.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ui::GlyphId::Triangle, m.markers[1].glyph);
    TEST_ASSERT_EQUAL(ui::layouts::ZoneColor::Warn, m.markers[1].color);
    TEST_ASSERT_TRUE(m.markers[1].vector);
    TEST_ASSERT_EQUAL(MetricSource::None, m.markers[1].dir_source);
    TEST_ASSERT_NOT_NULL(m.markers[1].dir_path);
    TEST_ASSERT_EQUAL_STRING("environment.current.setTrue", m.markers[1].dir_path);
    // [2]: unknown glyph -> Circle (web dot fallback); const dir -> fixed bearing.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ui::GlyphId::Circle, m.markers[2].glyph);
    TEST_ASSERT_EQUAL(MetricSource::None, m.markers[2].dir_source);
    TEST_ASSERT_NULL(m.markers[2].dir_path);
    TEST_ASSERT_EQUAL_FLOAT(135.0f, m.markers[2].dir_const);
    // Non-const markers carry NaN dir_const (hidden until their source lives).
    TEST_ASSERT_TRUE(isnan(m.markers[0].dir_const));
}

void test_markers_capped_at_max() {
    char json[2048];
    int off = snprintf(json, sizeof(json),
                       R"({"type":"windrose","bindings":{"value":{"kind":"signalk",)"
                       R"("path":"environment.wind.speedApparent"}},"markers":[)");
    for (int i = 0; i < 14; ++i)
        off += snprintf(json + off, sizeof(json) - off, "%s{\"glyph\":\"circle\"}", i ? "," : "");
    snprintf(json + off, sizeof(json) - off, "]}");
    MetricBinding m = mapOne(json, "rose");
    TEST_ASSERT_EQUAL_UINT8(ui::layouts::MAX_DIAL_MARKERS, m.marker_count);  // cap 12
}

void test_markers_without_alloc_degrade() {
    // Legacy 8-arg call (no StyleAlloc) — markers/sectors must drop cleanly,
    // never dangle.
    MetricBinding m = mapOneNoPathBufs(
        R"({"type":"compass","markers":[{"glyph":"diamond"}],
            "style":{"sectors":[{"from":0,"to":30,"color":"port"}]},
            "bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}}})",
        "hdg");
    TEST_ASSERT_EQUAL_UINT8(0, m.marker_count);
    TEST_ASSERT_NULL(m.markers);
    TEST_ASSERT_EQUAL_UINT8(0, m.sector_count);
    TEST_ASSERT_NULL(m.sectors);
}

void test_markers_ignored_on_non_dial() {
    // Only compass/windrose advertise marker glyphs; a numeric tile must not
    // consume pool slots.
    MetricBinding m = mapOne(
        R"({"type":"single-value","markers":[{"glyph":"diamond"}],
            "bindings":{"value":{"kind":"signalk","path":"navigation.speedOverGround"}}})",
        "sog");
    TEST_ASSERT_EQUAL_UINT8(0, m.marker_count);
    TEST_ASSERT_NULL(m.markers);
}

void test_sectors_fixed() {
    MetricBinding m = mapOne(
        R"({"type":"windrose",
            "style":{"sectors":[{"from":-30,"to":0,"color":"port"},
                                {"from":0,"to":30,"color":"starboard"},
                                {"from":170,"to":190,"color":"#123456"}]},
            "bindings":{"value":{"kind":"signalk","path":"environment.wind.speedApparent"}}})",
        "rose");
    TEST_ASSERT_EQUAL_UINT8(3, m.sector_count);
    TEST_ASSERT_EQUAL_FLOAT(-30.0f, m.sectors[0].from);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.sectors[0].to);
    TEST_ASSERT_EQUAL(ui::layouts::ZoneColor::Port, m.sectors[0].color);
    TEST_ASSERT_EQUAL(MetricSource::None, m.sectors[0].from_source);
    TEST_ASSERT_NULL(m.sectors[0].from_path);
    TEST_ASSERT_EQUAL(ui::layouts::ZoneColor::Starboard, m.sectors[1].color);
    TEST_ASSERT_EQUAL(ui::layouts::ZoneColor::Literal, m.sectors[2].color);
    TEST_ASSERT_EQUAL_HEX32(0x123456u, m.sectors[2].rgb);
}

void test_sectors_bound_edges() {
    // A string edge names a bindings key (dynamic laylines, midl types.ts:57):
    // enum bridge for known paths, raw-path retention otherwise, const value
    // bindings resolve to a fixed edge.
    MetricBinding m = mapOne(
        R"({"type":"compass",
            "bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"},
                        "beat":{"kind":"signalk","path":"performance.beatAngle"},
                        "cts":{"kind":"signalk","path":"navigation.courseRhumbline.bearingTrackTrue"},
                        "nogo":{"kind":"const","value":45}},
            "style":{"sectors":[{"from":"beat","to":"cts","color":"warn"},
                                {"from":-15,"to":"nogo","color":"alarm"}]}})",
        "hdg");
    TEST_ASSERT_EQUAL_UINT8(2, m.sector_count);
    // beat: performance.beatAngle misses the enum bridge -> raw path retained.
    TEST_ASSERT_EQUAL(MetricSource::None, m.sectors[0].from_source);
    TEST_ASSERT_NOT_NULL(m.sectors[0].from_path);
    TEST_ASSERT_EQUAL_STRING("performance.beatAngle", m.sectors[0].from_path);
    // cts: enum bridge hit.
    TEST_ASSERT_EQUAL(MetricSource::CTS_deg, m.sectors[0].to_source);
    TEST_ASSERT_NULL(m.sectors[0].to_path);
    // Mixed fixed + const-binding edge.
    TEST_ASSERT_EQUAL_FLOAT(-15.0f, m.sectors[1].from);
    TEST_ASSERT_EQUAL_FLOAT(45.0f, m.sectors[1].to);
    TEST_ASSERT_EQUAL(MetricSource::None, m.sectors[1].to_source);
    TEST_ASSERT_NULL(m.sectors[1].to_path);
}

void test_sectors_malformed_skipped_and_capped() {
    // Unresolvable string edge -> sector skipped; count capped at 3.
    MetricBinding m = mapOne(
        R"({"type":"compass",
            "bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}},
            "style":{"sectors":[{"from":"missingKey","to":10,"color":"warn"},
                                {"from":0,"to":10,"color":"warn"},
                                {"from":10,"to":20,"color":"warn"},
                                {"from":20,"to":30,"color":"warn"},
                                {"from":30,"to":40,"color":"warn"}]}})",
        "hdg");
    TEST_ASSERT_EQUAL_UINT8(3, m.sector_count);  // MAX_DIAL_SECTORS
    TEST_ASSERT_EQUAL_FLOAT(0.0f, m.sectors[0].from);
}

void test_hull_and_shape() {
    MetricBinding m = mapOne(
        R"({"type":"compass","style":{"hull":true,"shape":"band"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}}})",
        "hdg");
    TEST_ASSERT_TRUE(m.hull);
    TEST_ASSERT_EQUAL(ui::layouts::DialShape::Band, m.shape);
    MetricBinding d = mapOne(
        R"({"type":"compass","bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}}})",
        "hdg");
    TEST_ASSERT_FALSE(d.hull);
    TEST_ASSERT_EQUAL(ui::layouts::DialShape::Round, d.shape);
    // Unknown shape token stays round.
    MetricBinding u = mapOne(
        R"({"type":"compass","style":{"shape":"frobnicate"},
            "bindings":{"value":{"kind":"signalk","path":"navigation.headingTrue"}}})",
        "hdg");
    TEST_ASSERT_EQUAL(ui::layouts::DialShape::Round, u.shape);
}

void test_const_numeric_binding() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"TGT","format":{"unit":"kn","decimals":1},
            "bindings":{"value":{"kind":"const","value":6.5}}})",
        "tgt");
    TEST_ASSERT_EQUAL(ui::layouts::BindKind::ConstBind, m.value_kind);
    TEST_ASSERT_EQUAL_FLOAT(6.5f, m.const_value);
    TEST_ASSERT_NULL(m.const_text);
    // Const subscribes NOTHING: no enum source, no retained path.
    TEST_ASSERT_EQUAL(MetricSource::None, m.source);
    TEST_ASSERT_NULL(m.path);
    // …but it is a real value tile: zoomable fullscreen-self.
    TEST_ASSERT_TRUE(m.zoomable);
    TEST_ASSERT_NULL(m.zoom_target);
}

void test_const_string_and_bool() {
    MetricBinding s =
        mapOne(R"({"type":"text","bindings":{"value":{"kind":"const","value":"MOORED"}}})", "st");
    TEST_ASSERT_EQUAL(ui::layouts::BindKind::ConstBind, s.value_kind);
    TEST_ASSERT_NOT_NULL(s.const_text);
    TEST_ASSERT_EQUAL_STRING("MOORED", s.const_text);
    TEST_ASSERT_TRUE(isnan(s.const_value));
    MetricBinding b = mapOne(
        R"({"type":"single-value","bindings":{"value":{"kind":"const","value":true}}})", "b");
    TEST_ASSERT_EQUAL(ui::layouts::BindKind::ConstBind, b.value_kind);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, b.const_value);
}

void test_local_binding() {
    MetricBinding m = mapOne(
        R"({"type":"single-value","name":"RSSI","format":{"unit":"","decimals":0},
            "bindings":{"value":{"kind":"local","id":"rssi"}}})",
        "rssi");
    TEST_ASSERT_EQUAL(ui::layouts::BindKind::LocalBind, m.value_kind);
    TEST_ASSERT_NOT_NULL(m.local_id);
    TEST_ASSERT_EQUAL_STRING("rssi", m.local_id);
    // Local subscribes NOTHING (no source, no retained SignalK path).
    TEST_ASSERT_EQUAL(MetricSource::None, m.source);
    TEST_ASSERT_NULL(m.path);
    TEST_ASSERT_TRUE(m.zoomable);
}

void test_local_without_pathbuf_degrades() {
    MetricBinding m = mapOneNoPathBufs(
        R"({"type":"single-value","bindings":{"value":{"kind":"local","id":"rssi"}}})", "x");
    TEST_ASSERT_EQUAL(ui::layouts::BindKind::PathBind, m.value_kind);
    TEST_ASSERT_NULL(m.local_id);
    TEST_ASSERT_EQUAL(MetricSource::None, m.source);  // -> "--", never dangles
}

void test_action_value_number_string_bool() {
    MetricBinding n = mapOne(
        R"({"type":"button","action":{"kind":"put","target":"steering.autopilot.target.headingTrue","value":1.5708}})",
        "b");
    TEST_ASSERT_NOT_NULL(n.action_value);
    TEST_ASSERT_EQUAL_STRING("1.5708", n.action_value);
    MetricBinding s = mapOne(
        R"({"type":"button","action":{"kind":"put","target":"steering.autopilot.state","value":"auto"}})",
        "b");
    TEST_ASSERT_NOT_NULL(s.action_value);
    TEST_ASSERT_EQUAL_STRING("\"auto\"", s.action_value);  // JSON string for putValue
    MetricBinding f = mapOne(
        R"({"type":"button","action":{"kind":"put","target":"steering.autopilot.tack","value":false}})",
        "b");
    TEST_ASSERT_NOT_NULL(f.action_value);
    TEST_ASSERT_EQUAL_STRING("false", f.action_value);
}

void test_action_value_absent_stays_null() {
    // No authored value -> NULL; button_action_cb then sends the legacy `true`.
    MetricBinding m = mapOne(
        R"({"type":"button","action":{"kind":"put","target":"steering.autopilot.tack"}})", "tk");
    TEST_ASSERT_NOT_NULL(m.command);
    TEST_ASSERT_NULL(m.action_value);
}

void setUp() {
}
void tearDown() {
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_token_to_kind);
    RUN_TEST(test_map_clinometer_binds_attitude_roll);
    RUN_TEST(test_map_rudder_center_zero_bar);
    RUN_TEST(test_style_range_fallback_and_format_range_wins);
    RUN_TEST(test_engine_paths_bridge_to_enum_sources);
    RUN_TEST(test_map_single_value);
    RUN_TEST(test_map_unknown_path_is_none);
    RUN_TEST(test_map_unknown_path_without_buf_degrades);
    RUN_TEST(test_map_known_path_does_not_retain);
    RUN_TEST(test_map_compass_label_fallback_to_id);
    RUN_TEST(test_accent_hex_string);
    RUN_TEST(test_accent_integer);
    RUN_TEST(test_format_range_and_precision);
    RUN_TEST(test_format_absent_defaults);
    RUN_TEST(test_format_decimals_alias);
    RUN_TEST(test_format_decimals_wins_over_precision);
    RUN_TEST(test_dir_binding_enum_bridge);
    RUN_TEST(test_dir_binding_dynamic_path);
    RUN_TEST(test_dir_absent_defaults);
    RUN_TEST(test_side_flag_bool_and_string);
    RUN_TEST(test_size_roles);
    RUN_TEST(test_center_flag);
    RUN_TEST(test_zones_parsed);
    RUN_TEST(test_zones_malformed_entries_skipped);
    RUN_TEST(test_zones_absent_defaults);
    RUN_TEST(test_zone_pick_helper);
    RUN_TEST(test_convert_si_display_helper);
    RUN_TEST(test_format_side_value_helper);
    RUN_TEST(test_unit_is_angle_helper);
    RUN_TEST(test_action_nav_sets_target_screen);
    RUN_TEST(test_action_command_sets_command);
    RUN_TEST(test_action_put_kind_rides_command_slot);
    RUN_TEST(test_action_command_dotted_target_retained_verbatim);
    RUN_TEST(test_action_absent_leaves_both_null);
    RUN_TEST(test_action_unknown_kind_ignored);
    RUN_TEST(test_zoom_default_value_tile_is_zoomable_self);
    RUN_TEST(test_zoom_button_not_zoomable_by_default);
    RUN_TEST(test_zoom_sourceless_tile_not_zoomable);
    RUN_TEST(test_zoom_false_disables);
    RUN_TEST(test_zoom_true_keeps_self);
    RUN_TEST(test_zoom_string_sets_target_screen);
    RUN_TEST(test_markers_parsed);
    RUN_TEST(test_markers_capped_at_max);
    RUN_TEST(test_markers_without_alloc_degrade);
    RUN_TEST(test_markers_ignored_on_non_dial);
    RUN_TEST(test_sectors_fixed);
    RUN_TEST(test_sectors_bound_edges);
    RUN_TEST(test_sectors_malformed_skipped_and_capped);
    RUN_TEST(test_hull_and_shape);
    RUN_TEST(test_const_numeric_binding);
    RUN_TEST(test_const_string_and_bool);
    RUN_TEST(test_local_binding);
    RUN_TEST(test_local_without_pathbuf_degrades);
    RUN_TEST(test_action_value_number_string_bool);
    RUN_TEST(test_action_value_absent_stays_null);
    return UNITY_END();
}
