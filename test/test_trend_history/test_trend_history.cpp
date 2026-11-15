// Trend history store (midl docs/specs/2026-11-15-trend-history.md): duration
// parsing, time buckets, gaps, scroll on a steady value, aggregates, auto
// range, series sharing / maxSeries, backfill merge (never over live
// buckets), the History API query and response parsing.
#include <unity.h>
#include <ArduinoJson.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "generated_midl_limits.h"
#include "metric_value.h"
#include "trend_history.h"

using trend::Agg;
using trend::Store;

static int s_allocs = 0;
static void *t_alloc(size_t n) {
    ++s_allocs;
    return malloc(n);
}
static void t_free(void *p) {
    if (p) --s_allocs;
    free(p);
}

static Store s;

void setUp() {
    s_allocs = 0;
    TEST_ASSERT_TRUE(s.init(120, 6, t_alloc, t_free));
}
void tearDown() {
    s.shutdown();
    TEST_ASSERT_EQUAL_INT(0, s_allocs);  // every buffer returned
}

// ---- parsing ----

void test_parse_window() {
    uint32_t w = 0;
    TEST_ASSERT_TRUE(trend::parse_window("10m", w));
    TEST_ASSERT_EQUAL_UINT32(600, w);
    TEST_ASSERT_TRUE(trend::parse_window("90s", w));
    TEST_ASSERT_EQUAL_UINT32(90, w);
    TEST_ASSERT_TRUE(trend::parse_window("24h", w));
    TEST_ASSERT_EQUAL_UINT32(86400, w);
    TEST_ASSERT_FALSE(trend::parse_window("0m", w));
    TEST_ASSERT_FALSE(trend::parse_window("10", w));
    TEST_ASSERT_FALSE(trend::parse_window("10d", w));
    TEST_ASSERT_FALSE(trend::parse_window("m", w));
    TEST_ASSERT_FALSE(trend::parse_window("10mm", w));
    TEST_ASSERT_FALSE(trend::parse_window("-5m", w));
    TEST_ASSERT_FALSE(trend::parse_window(nullptr, w));

    JsonDocument d;
    deserializeJson(d, R"({"a":"1h","b":600,"c":true,"d":0,"e":"x"})");
    TEST_ASSERT_TRUE(trend::parse_window_json(d["a"], w));
    TEST_ASSERT_EQUAL_UINT32(3600, w);
    TEST_ASSERT_TRUE(trend::parse_window_json(d["b"], w));
    TEST_ASSERT_EQUAL_UINT32(600, w);
    TEST_ASSERT_FALSE(trend::parse_window_json(d["c"], w));
    TEST_ASSERT_FALSE(trend::parse_window_json(d["d"], w));
    TEST_ASSERT_FALSE(trend::parse_window_json(d["e"], w));
    TEST_ASSERT_FALSE(trend::parse_window_json(d["missing"], w));
}

void test_parse_agg_and_methods() {
    Agg a = Agg::Avg;
    TEST_ASSERT_TRUE(trend::parse_agg("max", a));
    TEST_ASSERT_EQUAL(Agg::Max, a);
    TEST_ASSERT_TRUE(trend::parse_agg("min", a));
    TEST_ASSERT_EQUAL(Agg::Min, a);
    TEST_ASSERT_TRUE(trend::parse_agg("last", a));
    TEST_ASSERT_EQUAL(Agg::Last, a);
    TEST_ASSERT_TRUE(trend::parse_agg("avg", a));
    TEST_ASSERT_EQUAL(Agg::Avg, a);
    TEST_ASSERT_FALSE(trend::parse_agg("average", a));
    TEST_ASSERT_EQUAL_STRING("average", trend::agg_sk_method(Agg::Avg));
    TEST_ASSERT_EQUAL_STRING("min", trend::agg_sk_method(Agg::Min));
    TEST_ASSERT_EQUAL_STRING("max", trend::agg_sk_method(Agg::Max));
    TEST_ASSERT_EQUAL_STRING("last", trend::agg_sk_method(Agg::Last));
}

void test_effective_window() {
    TEST_ASSERT_EQUAL_UINT32(600, trend::effective_window(0, 86400));
    TEST_ASSERT_EQUAL_UINT32(60, trend::effective_window(10, 86400));
    TEST_ASSERT_EQUAL_UINT32(86400, trend::effective_window(200000, 86400));
    TEST_ASSERT_EQUAL_UINT32(1800, trend::effective_window(1800, 86400));
}

void test_class_history_limits() {
    // Host builds use square-480: 120 points, 6 series, 24 h.
    TEST_ASSERT_EQUAL_INT(120, midl_gen::HISTORY_POINTS);
    TEST_ASSERT_EQUAL_INT(6, midl_gen::HISTORY_MAX_SERIES);
    TEST_ASSERT_EQUAL_INT(86400, (int)midl_gen::HISTORY_MAX_WINDOW_S);
}

// ---- buckets ----

void test_step_and_empty() {
    int i = s.acquire("environment.wind.speedTrue", Agg::Avg, 600);
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_EQUAL_DOUBLE(5.0, s.series(i)->step_s);  // 10m / 120
    for (int k = 0; k < 120; ++k)
        TEST_ASSERT_TRUE(isnan(s.value_at(i, k)));  // before any data: all gaps
}

void test_bucketing_and_alignment() {
    int i = s.acquire("p", Agg::Avg, 600);
    // Buckets align to floor(t / 5): 1000..1004.999 is one bucket.
    s.sample(i, 1000.0, 2.0f);
    s.sample(i, 1004.9, 4.0f);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, s.value_at(i, 119));
    s.sample(i, 1005.0, 10.0f);  // next bucket: the chart scrolls one point
    TEST_ASSERT_EQUAL_FLOAT(10.0f, s.value_at(i, 119));
    TEST_ASSERT_EQUAL_FLOAT(3.0f, s.value_at(i, 118));
}

void test_gaps_for_empty_buckets() {
    int i = s.acquire("p", Agg::Avg, 600);
    s.sample(i, 1000.0, 1.0f);
    s.sample(i, 1020.0, 2.0f);  // 4 buckets later; 3 empty in between
    TEST_ASSERT_EQUAL_FLOAT(2.0f, s.value_at(i, 119));
    TEST_ASSERT_TRUE(isnan(s.value_at(i, 118)));
    TEST_ASSERT_TRUE(isnan(s.value_at(i, 117)));
    TEST_ASSERT_TRUE(isnan(s.value_at(i, 116)));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s.value_at(i, 115));
}

void test_scrolls_on_steady_value_and_without_samples() {
    int i = s.acquire("p", Agg::Avg, 600);
    // A steady value sampled every refresh still scrolls one bucket per step.
    uint32_t v0 = s.series(i)->version;
    for (int k = 0; k < 10; ++k)
        s.sample(i, 1000.0 + k * 5.0, 7.0f);
    for (int k = 0; k < 10; ++k)
        TEST_ASSERT_EQUAL_FLOAT(7.0f, s.value_at(i, 119 - k));
    TEST_ASSERT_TRUE(s.series(i)->version > v0);
    // No samples at all: advance alone moves "now" and the old points shift left.
    int64_t head = s.series(i)->head;
    TEST_ASSERT_TRUE(s.advance(i, 1000.0 + 12 * 5.0));
    TEST_ASSERT_EQUAL_INT64(head + 3, s.series(i)->head);
    TEST_ASSERT_TRUE(isnan(s.value_at(i, 119)));
    TEST_ASSERT_EQUAL_FLOAT(7.0f, s.value_at(i, 119 - 3));
    TEST_ASSERT_FALSE(s.advance(i, 1000.0 + 12 * 5.0 + 1.0));  // same bucket
}

void test_old_data_leaves_window() {
    int i = s.acquire("p", Agg::Avg, 600);
    s.sample(i, 1000.0, 1.0f);
    s.advance(i, 1000.0 + 599.0);  // still inside the 10 min window
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s.value_at(i, 0));
    s.advance(i, 1000.0 + 605.0);  // one bucket past: gone
    for (int k = 0; k < 120; ++k)
        TEST_ASSERT_TRUE(isnan(s.value_at(i, k)));
    s.sample(i, 1000.0 + 5000.0, 3.0f);  // a jump far ahead clears everything
    TEST_ASSERT_EQUAL_FLOAT(3.0f, s.value_at(i, 119));
    TEST_ASSERT_TRUE(isnan(s.value_at(i, 0)));
}

void test_aggregates() {
    const float xs[] = {4.0f, 1.0f, 7.0f, 2.0f};
    const Agg aggs[] = {Agg::Avg, Agg::Min, Agg::Max, Agg::Last};
    const float want[] = {3.5f, 1.0f, 7.0f, 2.0f};
    for (int a = 0; a < 4; ++a) {
        int i = s.acquire("p", aggs[a], 600);
        for (float x : xs)
            s.sample(i, 2000.0, x);
        TEST_ASSERT_EQUAL_FLOAT(want[a], s.value_at(i, 119));
    }
    TEST_ASSERT_EQUAL_INT(4, s.series_count());  // agg is part of the key
}

void test_nan_sample_ignored() {
    int i = s.acquire("p", Agg::Avg, 600);
    s.sample(i, 1000.0, NAN);
    TEST_ASSERT_TRUE(isnan(s.value_at(i, 119)));
    s.sample(i, 1000.0, 5.0f);
    s.sample(i, 1001.0, NAN);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, s.value_at(i, 119));
}

// ---- sharing / limits / lifetime ----

void test_sharing_and_max_series() {
    bool created = false;
    int a = s.acquire("environment.wind.speedTrue", Agg::Avg, 600, &created);
    TEST_ASSERT_TRUE(created);
    int b = s.acquire("environment.wind.speedTrue", Agg::Avg, 600, &created);
    TEST_ASSERT_FALSE(created);
    TEST_ASSERT_EQUAL_INT(a, b);  // same path + agg + window = one series
    TEST_ASSERT_NOT_EQUAL(a, s.acquire("environment.wind.speedTrue", Agg::Avg, 1800));
    TEST_ASSERT_NOT_EQUAL(a, s.acquire("environment.wind.speedTrue", Agg::Max, 600));
    for (int k = 0; k < 3; ++k) {
        char key[16];
        snprintf(key, sizeof(key), "p%d", k);
        TEST_ASSERT_TRUE(s.acquire(key, Agg::Avg, 600) >= 0);
    }
    TEST_ASSERT_EQUAL_INT(6, s.series_count());
    TEST_ASSERT_EQUAL_INT(-1, s.acquire("one.too.many", Agg::Avg, 600));  // over maxSeries
    s.release_all();  // screen left: everything freed
    TEST_ASSERT_EQUAL_INT(0, s.series_count());
    TEST_ASSERT_EQUAL_INT(1, s_allocs);  // only the slot table remains
    TEST_ASSERT_EQUAL_INT(0, s.acquire("p", Agg::Avg, 600));
}

void test_round_class_geometry() {
    s.shutdown();
    TEST_ASSERT_TRUE(s.init(90, 2, t_alloc, t_free));
    int i = s.acquire("p", Agg::Avg, 600);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 600.0 / 90.0, s.series(i)->step_s);
    TEST_ASSERT_TRUE(s.acquire("q", Agg::Avg, 600) >= 0);
    TEST_ASSERT_EQUAL_INT(-1, s.acquire("r", Agg::Avg, 600));
}

// ---- auto range ----

void test_auto_range() {
    double lo = 0, hi = 0;
    float none[3] = {NAN, NAN, NAN};
    TEST_ASSERT_FALSE(trend::auto_range(none, 3, lo, hi));
    float v[4] = {NAN, 10.0f, 20.0f, NAN};
    TEST_ASSERT_TRUE(trend::auto_range(v, 4, lo, hi));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 9.0, lo);  // padded 10 % of the span
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 21.0, hi);
    float flat[3] = {5.0f, 5.0f, NAN};
    TEST_ASSERT_TRUE(trend::auto_range(flat, 3, lo, hi));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 4.0, lo);  // flat -> +-1 display unit
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 6.0, hi);
}

void test_display_conversion_matches_hero() {
    // The chart converts the stored SI value with the hero number's own path.
    boat::View d;
    d.tws = 5.0;  // m/s
    using ui::layouts::MetricSource;
    double si = ui::layouts::metric_si(MetricSource::TWS_kn, d);
    TEST_ASSERT_EQUAL_DOUBLE(5.0, si);
    TEST_ASSERT_EQUAL_DOUBLE(ui::layouts::metric_value(MetricSource::TWS_kn, d),
                             ui::layouts::metric_display_from_si(MetricSource::TWS_kn, si));
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 9.719, ui::layouts::metric_value(MetricSource::TWS_kn, d));
}

// ---- backfill ----

void test_backfill_fills_only_empty_buckets() {
    int i = s.acquire("p", Agg::Avg, 600);
    const double now = 10000.0;  // bucket 2000
    s.sample(i, now, 50.0f);     // live sample in the current bucket
    // Rows: 0 s ago (current bucket, live -> keep), 5 s ago, 10 s ago, and
    // one outside the window.
    const float age[] = {0.0f, 5.0f, 10.0f, 700.0f};
    const float val[] = {1.0f, 2.0f, 3.0f, 4.0f};
    int n = s.merge_backfill(i, now, age, val, 4);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_FLOAT(50.0f, s.value_at(i, 119));  // live wins
    TEST_ASSERT_EQUAL_FLOAT(2.0f, s.value_at(i, 118));
    TEST_ASSERT_EQUAL_FLOAT(3.0f, s.value_at(i, 117));
    TEST_ASSERT_TRUE(s.is_backfilled(i, 118));
    TEST_ASSERT_FALSE(s.is_backfilled(i, 119));
    TEST_ASSERT_TRUE(isnan(s.value_at(i, 116)));
}

void test_backfill_then_live_replaces_and_rows_combine() {
    int i = s.acquire("p", Agg::Avg, 600);
    const double now = 10002.0;  // bucket 2000 spans 10000..10005
    // Two rows land in the current bucket: averaged with each other.
    const float age[] = {1.0f, 2.0f};
    const float val[] = {4.0f, 6.0f};
    TEST_ASSERT_EQUAL_INT(1, s.merge_backfill(i, now, age, val, 2));
    TEST_ASSERT_EQUAL_FLOAT(5.0f, s.value_at(i, 119));
    TEST_ASSERT_TRUE(s.is_backfilled(i, 119));
    // A live sample in that bucket replaces the server value (live wins).
    s.sample(i, now + 1.0, 9.0f);
    TEST_ASSERT_EQUAL_FLOAT(9.0f, s.value_at(i, 119));
    TEST_ASSERT_FALSE(s.is_backfilled(i, 119));
    s.sample(i, now + 2.0, 11.0f);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, s.value_at(i, 119));
}

void test_backfill_late_result_aligns_by_age() {
    int i = s.acquire("p", Agg::Max, 600);
    s.sample(i, 10000.0, 1.0f);
    s.advance(i, 10030.0);  // the response arrives 30 s later
    const float age[] = {0.0f, 30.0f, 600.0f};
    const float val[] = {8.0f, 9.0f, 7.0f};
    s.merge_backfill(i, 10030.0, age, val, 3);
    TEST_ASSERT_EQUAL_FLOAT(8.0f, s.value_at(i, 119));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s.value_at(i, 113));  // live bucket from 10000 kept
}

void test_iso8601() {
    int64_t ms = 0;
    TEST_ASSERT_TRUE(trend::parse_iso8601_ms("1970-01-01T00:00:00Z", ms));
    TEST_ASSERT_EQUAL_INT64(0, ms);
    TEST_ASSERT_TRUE(trend::parse_iso8601_ms("2026-11-15T20:11:50.339Z", ms));
    TEST_ASSERT_EQUAL_INT64(1794773510339LL, ms);
    TEST_ASSERT_TRUE(trend::parse_iso8601_ms("2026-11-15T22:11:50.339+02:00", ms));
    TEST_ASSERT_EQUAL_INT64(1794773510339LL, ms);
    TEST_ASSERT_TRUE(trend::parse_iso8601_ms("2000-02-29T12:00:00.5Z", ms));
    TEST_ASSERT_EQUAL_INT64(951825600500LL, ms);
    TEST_ASSERT_FALSE(trend::parse_iso8601_ms("2026-11-15", ms));
    TEST_ASSERT_FALSE(trend::parse_iso8601_ms("2026-11-15T20:11:50", ms));  // no zone
    TEST_ASSERT_FALSE(trend::parse_iso8601_ms("garbage", ms));
}

void test_history_query() {
    char q[256];
    TEST_ASSERT_TRUE(
        trend::build_history_query(q, sizeof(q), "environment.wind.speedTrue", Agg::Avg, 600, 5.0));
    TEST_ASSERT_EQUAL_STRING("/signalk/v2/api/history/values?paths=environment.wind.speedTrue:"
                             "average&duration=PT600S&resolution=5",
                             q);
    TEST_ASSERT_TRUE(trend::build_history_query(q, sizeof(q), "a.b", Agg::Max, 600, 600.0 / 90));
    TEST_ASSERT_EQUAL_STRING(
        "/signalk/v2/api/history/values?paths=a.b:max&duration=PT600S&resolution=6.667", q);
    TEST_ASSERT_FALSE(trend::build_history_query(q, sizeof(q), "a b", Agg::Avg, 600, 5.0));
    TEST_ASSERT_FALSE(trend::build_history_query(q, sizeof(q), "a..b", Agg::Avg, 600, 5.0));
    TEST_ASSERT_FALSE(trend::build_history_query(q, 20, "a.b", Agg::Avg, 600, 5.0));
}

void test_parse_response_rows() {
    // Shape of signalk-server 2.x /signalk/v2/api/history/values.
    JsonDocument d;
    deserializeJson(d, R"({"context":"vessels.self",
        "range":{"from":"2026-11-15T20:11:50.000Z","to":"2026-11-15T20:21:50.000Z"},
        "values":[{"path":"environment.wind.speedTrue","method":"average"}],
        "data":[["2026-11-15T20:21:40.000Z",5.5],["2026-11-15T20:21:45.000Z",null],
                ["2026-11-15T20:21:50.000Z",6.0],["bad",1]]})");
    float age[8], val[8];
    int n = trend::parse_history_rows(d.as<JsonVariantConst>(), age, val, 8);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, age[0]);
    TEST_ASSERT_EQUAL_FLOAT(5.5f, val[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, age[1]);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, val[1]);
    // Empty data (bench server without a provider) = no backfill.
    deserializeJson(d, R"({"range":{"to":"2026-11-15T20:21:50.339Z"},"data":[]})");
    TEST_ASSERT_EQUAL_INT(0, trend::parse_history_rows(d.as<JsonVariantConst>(), age, val, 8));
    // Error body (400/501) = not a values response.
    deserializeJson(d, R"({"error":"No history api provider configured"})");
    TEST_ASSERT_EQUAL_INT(-1, trend::parse_history_rows(d.as<JsonVariantConst>(), age, val, 8));
    // No range: anchored on the newest row.
    deserializeJson(d, R"({"data":[["2026-11-15T20:00:00Z",1],["2026-11-15T20:00:30Z",2]]})");
    n = trend::parse_history_rows(d.as<JsonVariantConst>(), age, val, 8);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, age[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, age[1]);
    // Cap respected.
    TEST_ASSERT_EQUAL_INT(1, trend::parse_history_rows(d.as<JsonVariantConst>(), age, val, 1));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_window);
    RUN_TEST(test_parse_agg_and_methods);
    RUN_TEST(test_effective_window);
    RUN_TEST(test_class_history_limits);
    RUN_TEST(test_step_and_empty);
    RUN_TEST(test_bucketing_and_alignment);
    RUN_TEST(test_gaps_for_empty_buckets);
    RUN_TEST(test_scrolls_on_steady_value_and_without_samples);
    RUN_TEST(test_old_data_leaves_window);
    RUN_TEST(test_aggregates);
    RUN_TEST(test_nan_sample_ignored);
    RUN_TEST(test_sharing_and_max_series);
    RUN_TEST(test_round_class_geometry);
    RUN_TEST(test_auto_range);
    RUN_TEST(test_display_conversion_matches_hero);
    RUN_TEST(test_backfill_fills_only_empty_buckets);
    RUN_TEST(test_backfill_then_live_replaces_and_rows_combine);
    RUN_TEST(test_backfill_late_result_aligns_by_age);
    RUN_TEST(test_iso8601);
    RUN_TEST(test_history_query);
    RUN_TEST(test_parse_response_rows);
    return UNITY_END();
}
