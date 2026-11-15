#pragma once

// Time-windowed trend history (contract: midl docs/specs/2026-11-15-trend-history.md).
//
// A trend element records its value into fixed time buckets aligned to
// floor(t / step), step = window / points. The chart spans exactly `window`
// with its right edge at "now"; a bucket with no samples is a gap. Values are
// stored in SI (as received) and converted to display units at draw time.
//
// Pure C++ (no LVGL, no Arduino): host-tested by test/test_trend_history. The
// device wiring lives in src/ui/ui_layouts.cpp (recording + chart) and
// src/trend_backfill.cpp (SignalK History API fetch on a worker task).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <stddef.h>
#include <stdint.h>

#include <ArduinoJson.h>

namespace trend {

// history.agg: how the samples in one bucket combine.
enum class Agg : uint8_t {
    Avg = 0,
    Min,
    Max,
    Last,
};

constexpr uint32_t DEFAULT_WINDOW_S = 600;  // history absent -> 10m
constexpr uint32_t MIN_WINDOW_S = 60;       // spec: window >= 60 s
constexpr size_t KEY_MAX = 96;              // path / key buffer (layout::PATH_LEN)
constexpr uint16_t MAX_POINTS = 240;        // hard cap on points per series
constexpr uint8_t MAX_SERIES_CAP = 8;       // hard cap on series slots

// "<n>s" | "<n>m" | "<n>h" -> seconds. False on anything else (incl. 0).
bool parse_window(const char *s, uint32_t &out_s);
// history.window as authored: a duration string or a number of seconds.
// False when absent/invalid (caller keeps the default).
bool parse_window_json(JsonVariantConst v, uint32_t &out_s);
// "avg" | "min" | "max" | "last". False on anything else.
bool parse_agg(const char *s, Agg &out);
const char *agg_token(Agg a);      // "avg" ...
const char *agg_sk_method(Agg a);  // SignalK History API method: "average" | "min" | "max" | "last"

// Clamp an authored window into [MIN_WINDOW_S, max_window_s]; 0 -> default.
uint32_t effective_window(uint32_t window_s, uint32_t max_window_s);

// Auto y-range over display values (NaN = gap): min/max of the finite values,
// padded 10 % of the span on each side; a flat line gets +-1 display unit.
// False (lo/hi untouched) when there is no finite value.
bool auto_range(const float *v, int n, double &lo, double &hi);

// ISO-8601 UTC timestamp ("2026-11-15T20:11:50.339Z", fraction optional, 'Z'
// or +hh:mm offset) -> epoch milliseconds.
bool parse_iso8601_ms(const char *s, int64_t &epoch_ms);

// A path usable in the History API query (dotted [A-Za-z0-9_-] segments).
bool path_ok(const char *path);

// Build "/signalk/v2/api/history/values?paths=<path>:<method>&duration=PT<w>S&resolution=<step>".
// `resolution` is seconds (SignalK parseResolution: a number of seconds; a
// fraction is fine, e.g. 6.667 for 10m over 90 points). False on overflow / bad path.
bool build_history_query(char *out, size_t cap, const char *path, Agg agg, uint32_t window_s,
                         double step_s);

// Parse a History API values response into rows of (age_s, value) where
// age_s = anchor - row time and anchor = range.to (the server's "now" for the
// query; falls back to the newest row's time). Rows with a null/non-numeric
// value are skipped. Returns the row count written (0 = empty data), or -1
// when the document is not a values response.
int parse_history_rows(JsonVariantConst doc, float *age_s, float *vals, int cap);

// Per-bucket count encoding: 0 = empty (gap); live samples count 1..0x7FFF;
// BACKFILL_BIT marks a bucket filled from the server (a live sample replaces it).
constexpr uint16_t BACKFILL_BIT = 0x8000;
constexpr uint16_t COUNT_MASK = 0x7FFF;

struct Series {
    bool used;
    Agg agg;
    uint32_t window_s;
    double step_s;
    int64_t head;      // absolute bucket index (floor(t/step)) of the newest bucket
    bool has_head;     // false until the first advance
    uint32_t version;  // bumps on every visible change (sample, scroll, backfill)
    float *val;        // [points] aggregated SI value per ring slot
    uint16_t *cnt;     // [points] count per ring slot (see BACKFILL_BIT)
    char key[KEY_MAX];
};

using AllocFn = void *(*)(size_t);
using FreeFn = void (*)(void *);

// Fixed-capacity store of series for the screen on display. Single-threaded
// (UI task). Buffers come from the injected allocator (device: PSRAM first).
class Store {
  public:
    // Allocates the slot table. Safe to call again with the same geometry.
    bool init(uint16_t points, uint8_t max_series, AllocFn alloc, FreeFn free_fn);
    void shutdown();  // release_all + free the slot table

    // Find or create the series for key+agg+window. Tiles with the same key
    // share one series. Returns its index, or -1 when max_series is reached
    // or the buffer allocation failed. `created` reports a new series.
    int acquire(const char *key, Agg agg, uint32_t window_s, bool *created = nullptr);
    void release_all();  // free every series buffer (screen left)

    // Record one SI sample at monotonic time now_s into the current bucket.
    void sample(int s, double now_s, float si);
    // Scroll the series to now_s (clears buckets that left the window).
    // True when the head bucket moved.
    bool advance(int s, double now_s);
    // Display point i: 0 = oldest .. points()-1 = newest (the bucket of
    // "now"). NaN for a gap.
    float value_at(int s, int i) const;
    // True when bucket i was filled by backfill (not live).
    bool is_backfilled(int s, int i) const;
    // Merge server rows (age_s before t_resp_s, value) into buckets that no
    // live sample has filled. Returns the number of buckets written.
    int merge_backfill(int s, double t_resp_s, const float *age_s, const float *vals, int n);

    const Series *series(int s) const;
    int series_count() const;
    uint16_t points() const { return points_; }
    uint8_t max_series() const { return max_series_; }

  private:
    int slot_of(int64_t bucket) const;
    void clear_slot(Series &se, int slot);

    Series *slots_ = nullptr;
    uint16_t points_ = 0;
    uint8_t max_series_ = 0;
    AllocFn alloc_ = nullptr;
    FreeFn free_ = nullptr;
};

}  // namespace trend
