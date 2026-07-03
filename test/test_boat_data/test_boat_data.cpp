#include <unity.h>
#include <cmath>
#include <cstdio>

#include "boat_data.h"
// source_signalk.cpp (ingest_signalk + compose) is pure C++ but not part of
// the native env's build_src_filter; include the TU directly so this suite
// can exercise the mask-gated SignalK ingest end to end.
#include "../../src/source_signalk.cpp"

using namespace boat;

void setUp(void) {
    reset_all();
}
void tearDown(void) {
}

static void test_defaults() {
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_TRUE(std::isnan(s.sog_mps.value));
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::None), static_cast<int>(s.sog_mps.source));
    TEST_ASSERT_EQUAL_UINT32(0, s.sog_mps.updated_ms);
}

static void test_rank_of_default_priority() {
    Priority p;
    TEST_ASSERT_EQUAL_UINT8(0, rank_of(p, SourceKind::Nmea2000));
    TEST_ASSERT_EQUAL_UINT8(1, rank_of(p, SourceKind::NmeaWifi));
    TEST_ASSERT_EQUAL_UINT8(2, rank_of(p, SourceKind::SignalK));
    TEST_ASSERT_EQUAL_UINT8(3, rank_of(p, SourceKind::Demo));
    TEST_ASSERT_EQUAL_UINT8(255, rank_of(p, SourceKind::None));
}

static void test_fresh_window() {
    Field f;
    f.value = 1.0;
    f.source = SourceKind::SignalK;
    f.updated_ms = 100;
    TEST_ASSERT_TRUE(fresh(f, 200, 1000));
    TEST_ASSERT_FALSE(fresh(f, 2000, 1000));
    TEST_ASSERT_FALSE(fresh(f, 100, 0));
    Field empty;
    TEST_ASSERT_FALSE(fresh(empty, 100, 1000));
}

static void test_value_or_nan() {
    Field f;
    f.value = 4.13;
    f.source = SourceKind::NmeaWifi;
    f.updated_ms = 100;
    TEST_ASSERT_EQUAL_DOUBLE(4.13, value_or_nan(f, 200, 1000));
    TEST_ASSERT_TRUE(std::isnan(value_or_nan(f, 5000, 1000)));
}

static void test_signalk_only_publish() {
    publish(&Snapshot::sog_mps, SourceKind::SignalK, 1000, 3.5);
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(3.5, s.sog_mps.value);
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::SignalK), static_cast<int>(s.sog_mps.source));
}

static void test_priority_higher_wins_when_both_fresh() {
    publish(&Snapshot::sog_mps, SourceKind::SignalK, 1000, 3.5);
    bool accepted = publish(&Snapshot::sog_mps, SourceKind::NmeaWifi, 1100, 4.0);
    TEST_ASSERT_TRUE(accepted);
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(4.0, s.sog_mps.value);
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::NmeaWifi), static_cast<int>(s.sog_mps.source));
}

static void test_lower_priority_rejected_when_higher_fresh() {
    publish(&Snapshot::sog_mps, SourceKind::Nmea2000, 1000, 4.0);
    bool accepted = publish(&Snapshot::sog_mps, SourceKind::SignalK, 1100, 3.5);
    TEST_ASSERT_FALSE(accepted);
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(4.0, s.sog_mps.value);
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::Nmea2000), static_cast<int>(s.sog_mps.source));
}

static void test_lower_priority_accepted_when_higher_stale() {
    // Default n2k timeout = 2000ms.
    publish(&Snapshot::sog_mps, SourceKind::Nmea2000, 1000, 4.0);
    bool accepted = publish(&Snapshot::sog_mps, SourceKind::SignalK, 5000, 3.5);
    TEST_ASSERT_TRUE(accepted);
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(3.5, s.sog_mps.value);
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::SignalK), static_cast<int>(s.sog_mps.source));
}

static void test_custom_priority_reorders() {
    Priority p;
    p.order[0] = SourceKind::SignalK;
    p.order[1] = SourceKind::NmeaWifi;
    p.order[2] = SourceKind::Nmea2000;
    p.order[3] = SourceKind::Demo;
    p.order[4] = SourceKind::None;
    set_priority(p);
    publish(&Snapshot::sog_mps, SourceKind::SignalK, 1000, 3.5);
    bool accepted = publish(&Snapshot::sog_mps, SourceKind::Nmea2000, 1100, 4.0);
    TEST_ASSERT_FALSE(accepted);
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(3.5, s.sog_mps.value);
}

static void test_should_accept_pure() {
    Priority p;
    Timeouts t;
    // Same source overwriting itself is fine.
    TEST_ASSERT_TRUE(should_accept(SourceKind::SignalK, SourceKind::SignalK, 100, 200, p, t));
    // Higher priority always wins when fresh.
    TEST_ASSERT_TRUE(should_accept(SourceKind::Nmea2000, SourceKind::SignalK, 100, 200, p, t));
    // None never accepted.
    TEST_ASSERT_FALSE(should_accept(SourceKind::None, SourceKind::SignalK, 100, 200, p, t));
    // Lower priority rejected when higher fresh.
    TEST_ASSERT_FALSE(should_accept(SourceKind::SignalK, SourceKind::Nmea2000, 100, 200, p, t));
    // Lower priority accepted when higher stale.
    TEST_ASSERT_TRUE(should_accept(SourceKind::SignalK, SourceKind::Nmea2000, 100, 5000, p, t));
}

static void test_autopilot_state_publish() {
    publish_autopilot_state(SourceKind::SignalK, 1000, "auto");
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_STRING("auto", s.autopilot_state);
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::SignalK),
                      static_cast<int>(s.autopilot_state_source));

    // Higher priority overrides.
    publish_autopilot_state(SourceKind::Nmea2000, 1100, "wind");
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_STRING("wind", s.autopilot_state);

    // Lower priority rejected.
    publish_autopilot_state(SourceKind::SignalK, 1200, "standby");
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_STRING("wind", s.autopilot_state);
}

static void test_independent_fields() {
    publish(&Snapshot::sog_mps, SourceKind::Nmea2000, 1000, 4.0);
    publish(&Snapshot::depth_m, SourceKind::SignalK, 1000, 12.0);
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(4.0, s.sog_mps.value);
    TEST_ASSERT_EQUAL_DOUBLE(12.0, s.depth_m.value);
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::Nmea2000), static_cast<int>(s.sog_mps.source));
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::SignalK), static_cast<int>(s.depth_m.source));
}

// --- mask-gated SignalK ingest (BUG: per-field staleness must survive a
// chatty link that keeps re-delivering OTHER fields) ---

static void test_ingest_publishes_only_touched_fields() {
    // The accumulator View carries a finite depth from an earlier delta, but
    // THIS delta only touched sog. Only sog may be (re)stamped.
    View v;
    v.sog = 4.2;
    v.depth = 7.5;  // stale accumulator leftover
    int n = ingest_signalk(v, 1000, field_bit(FieldId::Sog));
    TEST_ASSERT_EQUAL(1, n);
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(4.2, s.sog_mps.value);
    TEST_ASSERT_EQUAL_UINT32(1000, s.sog_mps.updated_ms);
    TEST_ASSERT_TRUE(std::isnan(s.depth_m.value));  // never published
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::None), static_cast<int>(s.depth_m.source));
}

static void test_ingest_untouched_field_keeps_old_timestamp() {
    View v;
    v.sog = 4.2;
    v.depth = 7.5;
    // First delta carries both fields.
    ingest_signalk(v, 1000, field_bit(FieldId::Sog) | field_bit(FieldId::Depth));
    // Depth sensor dies; sog keeps arriving. The accumulator still holds the
    // last depth value, but the mask no longer includes it.
    v.sog = 4.4;
    ingest_signalk(v, 6000, field_bit(FieldId::Sog));
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_UINT32(6000, s.sog_mps.updated_ms);
    TEST_ASSERT_EQUAL_UINT32(1000, s.depth_m.updated_ms);  // NOT re-stamped
}

static void test_ingest_stale_field_goes_nan_while_others_update() {
    // Depth arrives once; sog keeps updating every second. After the SignalK
    // freshness window (default 10 s) depth must compose to NaN while sog
    // stays live.
    View v;
    v.depth = 7.5;
    v.sog = 4.0;
    ingest_signalk(v, 1000, field_bit(FieldId::Sog) | field_bit(FieldId::Depth));
    for (uint32_t t = 2000; t <= 14000; t += 1000) {
        v.sog = 4.0 + t * 1e-4;
        ingest_signalk(v, t, field_bit(FieldId::Sog));
    }
    View out;
    compose(out, 14500);
    TEST_ASSERT_FALSE(std::isnan(out.sog));   // still fresh (last at 14000)
    TEST_ASSERT_TRUE(std::isnan(out.depth));  // stale: last touch was 1000
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_UINT32(1000, s.depth_m.updated_ms);
}

static void test_ingest_stale_signalk_yields_to_lower_priority() {
    // A dead SignalK field must not permanently outrank a lower-priority
    // source: once the SK stamp ages past its timeout, Demo may take over.
    View v;
    v.sog = 4.0;
    ingest_signalk(v, 1000, field_bit(FieldId::Sog));
    // Still fresh at 5000 -> Demo rejected.
    TEST_ASSERT_FALSE(publish(&Snapshot::sog_mps, SourceKind::Demo, 5000, 9.9));
    // SignalK keeps sending OTHER fields; sog stays untouched.
    v.depth = 7.0;
    ingest_signalk(v, 10500, field_bit(FieldId::Depth));
    // sog's SignalK stamp is now 10.5 s old (> 10 s timeout) -> Demo accepted.
    TEST_ASSERT_TRUE(publish(&Snapshot::sog_mps, SourceKind::Demo, 11500, 9.9));
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(9.9, s.sog_mps.value);
    TEST_ASSERT_EQUAL(static_cast<int>(SourceKind::Demo), static_cast<int>(s.sog_mps.source));
}

static void test_ingest_apstate_gated_by_mask() {
    View v;
    std::snprintf(v.apState, sizeof(v.apState), "auto");
    // Accumulator carries the string but this delta didn't touch it.
    ingest_signalk(v, 1000, field_bit(FieldId::Sog));
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_STRING("", s.autopilot_state);
    // Now the delta actually carries it.
    int n = ingest_signalk(v, 2000, field_bit(FieldId::ApState));
    TEST_ASSERT_EQUAL(1, n);
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_STRING("auto", s.autopilot_state);
    TEST_ASSERT_EQUAL_UINT32(2000, s.autopilot_state_updated_ms);
}

static void test_ingest_new_fields_mask_gated() {
    // Coverage-wave fields ride the same mask-gated ingest: only the bits the
    // delta carried are published.
    View v;
    v.engineRevs = 30.0;
    v.roll = -0.0872;
    v.outsidePressure = 101300.0;  // in the accumulator but NOT in this mask
    int n = ingest_signalk(v, 1000, field_bit(FieldId::EngineRpm) | field_bit(FieldId::Roll));
    TEST_ASSERT_EQUAL(2, n);
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(30.0, s.engine_rev_hz.value);
    TEST_ASSERT_EQUAL_DOUBLE(-0.0872, s.roll_rad.value);
    TEST_ASSERT_TRUE(std::isnan(s.outside_pressure_pa.value));  // never published
}

static void test_ingest_heading_mag_and_variation_compose() {
    View v;
    v.headingMag = 1.0;
    v.variation = 0.05;
    ingest_signalk(v, 1000, field_bit(FieldId::HeadingMag) | field_bit(FieldId::Variation));
    View out;
    compose(out, 1500);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, out.headingMag);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.05, out.variation);
    TEST_ASSERT_TRUE(std::isnan(out.headingTrue));  // BUG-2: not conflated
}

static void test_ingest_engine_hours_mask_gated_and_composes() {
    // Phase 5: propulsion.*.runTime (seconds) rides the same mask-gated
    // ingest and composes onto View.engineHours.
    View v;
    v.engineHours = 5400.0;  // 1.5 h in seconds
    // In the accumulator but NOT in the mask -> never published.
    ingest_signalk(v, 1000, field_bit(FieldId::Sog));
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_TRUE(std::isnan(s.engine_hours_s.value));
    // Mask carries it -> published + composes.
    int n = ingest_signalk(v, 2000, field_bit(FieldId::EngineHours));
    TEST_ASSERT_EQUAL(1, n);
    copy_snapshot(s);
    TEST_ASSERT_EQUAL_DOUBLE(5400.0, s.engine_hours_s.value);
    TEST_ASSERT_EQUAL_UINT32(2000, s.engine_hours_s.updated_ms);
    View out;
    compose(out, 2500);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 5400.0, out.engineHours);
    // And it goes stale like any SignalK field (default 10 s window).
    compose(out, 13000);
    TEST_ASSERT_TRUE(std::isnan(out.engineHours));
}

static void test_ingest_empty_mask_publishes_nothing() {
    View v;
    v.sog = 4.0;
    v.depth = 7.5;
    TEST_ASSERT_EQUAL(0, ingest_signalk(v, 1000, 0));
    Snapshot s;
    copy_snapshot(s);
    TEST_ASSERT_TRUE(std::isnan(s.sog_mps.value));
    TEST_ASSERT_TRUE(std::isnan(s.depth_m.value));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_rank_of_default_priority);
    RUN_TEST(test_fresh_window);
    RUN_TEST(test_value_or_nan);
    RUN_TEST(test_signalk_only_publish);
    RUN_TEST(test_priority_higher_wins_when_both_fresh);
    RUN_TEST(test_lower_priority_rejected_when_higher_fresh);
    RUN_TEST(test_lower_priority_accepted_when_higher_stale);
    RUN_TEST(test_custom_priority_reorders);
    RUN_TEST(test_should_accept_pure);
    RUN_TEST(test_autopilot_state_publish);
    RUN_TEST(test_independent_fields);
    RUN_TEST(test_ingest_publishes_only_touched_fields);
    RUN_TEST(test_ingest_untouched_field_keeps_old_timestamp);
    RUN_TEST(test_ingest_stale_field_goes_nan_while_others_update);
    RUN_TEST(test_ingest_stale_signalk_yields_to_lower_priority);
    RUN_TEST(test_ingest_apstate_gated_by_mask);
    RUN_TEST(test_ingest_new_fields_mask_gated);
    RUN_TEST(test_ingest_heading_mag_and_variation_compose);
    RUN_TEST(test_ingest_engine_hours_mask_gated_and_composes);
    RUN_TEST(test_ingest_empty_mask_publishes_nothing);
    return UNITY_END();
}
