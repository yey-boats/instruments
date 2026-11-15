// Time-windowed trend history store. See include/trend_history.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "trend_history.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace trend {

bool parse_window(const char *s, uint32_t &out_s) {
    if (!s || !isdigit((unsigned char)s[0])) return false;
    uint64_t n = 0;
    const char *p = s;
    while (isdigit((unsigned char)*p)) {
        n = n * 10 + (uint64_t)(*p - '0');
        if (n > 0xFFFFFFFFull) return false;
        ++p;
    }
    uint64_t mul = 0;
    if (p[0] == 's')
        mul = 1;
    else if (p[0] == 'm')
        mul = 60;
    else if (p[0] == 'h')
        mul = 3600;
    if (!mul || p[1] != 0) return false;
    uint64_t v = n * mul;
    if (v == 0 || v > 0xFFFFFFFFull) return false;
    out_s = (uint32_t)v;
    return true;
}

bool parse_window_json(JsonVariantConst v, uint32_t &out_s) {
    if (v.is<const char *>()) return parse_window(v.as<const char *>(), out_s);
    if (v.is<double>() && !v.is<bool>()) {
        double d = v.as<double>();
        if (!(d >= 1.0) || d > 4294967295.0) return false;
        out_s = (uint32_t)(d + 0.5);
        return true;
    }
    return false;
}

bool parse_agg(const char *s, Agg &out) {
    if (!s) return false;
    if (!strcmp(s, "avg")) {
        out = Agg::Avg;
    } else if (!strcmp(s, "min")) {
        out = Agg::Min;
    } else if (!strcmp(s, "max")) {
        out = Agg::Max;
    } else if (!strcmp(s, "last")) {
        out = Agg::Last;
    } else {
        return false;
    }
    return true;
}

const char *agg_token(Agg a) {
    switch (a) {
    case Agg::Min:
        return "min";
    case Agg::Max:
        return "max";
    case Agg::Last:
        return "last";
    case Agg::Avg:
    default:
        return "avg";
    }
}

const char *agg_sk_method(Agg a) {
    switch (a) {
    case Agg::Min:
        return "min";
    case Agg::Max:
        return "max";
    case Agg::Last:
        return "last";
    case Agg::Avg:
    default:
        return "average";
    }
}

uint32_t effective_window(uint32_t window_s, uint32_t max_window_s) {
    if (window_s == 0) window_s = DEFAULT_WINDOW_S;
    if (window_s < MIN_WINDOW_S) window_s = MIN_WINDOW_S;
    if (max_window_s >= MIN_WINDOW_S && window_s > max_window_s) window_s = max_window_s;
    return window_s;
}

bool auto_range(const float *v, int n, double &lo, double &hi) {
    bool any = false;
    double mn = 0, mx = 0;
    for (int i = 0; i < n; ++i) {
        if (!isfinite(v[i])) continue;
        if (!any) {
            mn = mx = v[i];
            any = true;
        } else {
            if (v[i] < mn) mn = v[i];
            if (v[i] > mx) mx = v[i];
        }
    }
    if (!any) return false;
    double span = mx - mn;
    if (span <= 0) {
        lo = mn - 1.0;
        hi = mx + 1.0;
    } else {
        lo = mn - span * 0.1;
        hi = mx + span * 0.1;
    }
    return true;
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's
// days_from_civil).
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool read_int(const char *&p, int digits, int &out) {
    out = 0;
    for (int i = 0; i < digits; ++i) {
        if (!isdigit((unsigned char)p[i])) return false;
        out = out * 10 + (p[i] - '0');
    }
    p += digits;
    return true;
}

bool parse_iso8601_ms(const char *s, int64_t &epoch_ms) {
    if (!s) return false;
    const char *p = s;
    int Y, M, D, h, mi, sec;
    if (!read_int(p, 4, Y) || *p++ != '-' || !read_int(p, 2, M) || *p++ != '-' ||
        !read_int(p, 2, D))
        return false;
    if (*p != 'T' && *p != 't' && *p != ' ') return false;
    ++p;
    if (!read_int(p, 2, h) || *p++ != ':' || !read_int(p, 2, mi)) return false;
    sec = 0;
    if (*p == ':') {
        ++p;
        if (!read_int(p, 2, sec)) return false;
    }
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || sec > 60) return false;
    int ms = 0;
    if (*p == '.' || *p == ',') {
        ++p;
        if (!isdigit((unsigned char)*p)) return false;
        int scale = 100;
        while (isdigit((unsigned char)*p)) {
            ms += (*p - '0') * scale;
            scale /= 10;
            ++p;
        }
    }
    int64_t off_s = 0;
    if (*p == 'Z' || *p == 'z') {
        ++p;
    } else if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1;
        ++p;
        int oh, om = 0;
        if (!read_int(p, 2, oh)) return false;
        if (*p == ':') ++p;
        if (isdigit((unsigned char)*p) && !read_int(p, 2, om)) return false;
        off_s = sign * (oh * 3600 + om * 60);
    } else {
        return false;
    }
    if (*p != 0) return false;
    int64_t days = days_from_civil(Y, (unsigned)M, (unsigned)D);
    int64_t secs = days * 86400 + h * 3600 + mi * 60 + sec - off_s;
    epoch_ms = secs * 1000 + ms;
    return true;
}

bool path_ok(const char *path) {
    if (!path || !path[0] || strlen(path) >= KEY_MAX) return false;
    bool seg_empty = true;
    for (const char *p = path; *p; ++p) {
        char c = *p;
        if (c == '.') {
            if (seg_empty) return false;
            seg_empty = true;
            continue;
        }
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
        seg_empty = false;
    }
    return !seg_empty;
}

bool build_history_query(char *out, size_t cap, const char *path, Agg agg, uint32_t window_s,
                         double step_s) {
    if (!out || cap == 0 || !path_ok(path) || window_s == 0 || !(step_s > 0)) return false;
    // %.3f then trim trailing zeros: "5", "6.667", "720".
    char res[24];
    snprintf(res, sizeof(res), "%.3f", step_s);
    char *dot = strchr(res, '.');
    if (dot) {
        char *e = res + strlen(res) - 1;
        while (e > dot && *e == '0')
            *e-- = 0;
        if (e == dot) *e = 0;
    }
    int n = snprintf(out, cap,
                     "/signalk/v2/api/history/values?paths=%s:%s&duration=PT%luS&resolution=%s",
                     path, agg_sk_method(agg), (unsigned long)window_s, res);
    return n > 0 && (size_t)n < cap;
}

int parse_history_rows(JsonVariantConst doc, float *age_s, float *vals, int cap) {
    JsonArrayConst data = doc["data"].as<JsonArrayConst>();
    if (data.isNull()) return -1;
    int64_t anchor = 0;
    bool have_anchor = parse_iso8601_ms(doc["range"]["to"] | "", anchor);
    if (!have_anchor) {
        // Fall back to the newest row time.
        for (JsonVariantConst row : data) {
            int64_t t;
            if (parse_iso8601_ms(row[0] | "", t) && (!have_anchor || t > anchor)) {
                anchor = t;
                have_anchor = true;
            }
        }
        if (!have_anchor) return 0;
    }
    int n = 0;
    for (JsonVariantConst row : data) {
        if (n >= cap) break;
        int64_t t;
        if (!parse_iso8601_ms(row[0] | "", t)) continue;
        JsonVariantConst v = row[1];
        if (v.isNull() || v.is<bool>() || !v.is<double>()) continue;
        double d = v.as<double>();
        if (!isfinite(d)) continue;
        age_s[n] = (float)((double)(anchor - t) / 1000.0);
        vals[n] = (float)d;
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Store

bool Store::init(uint16_t points, uint8_t max_series, AllocFn alloc, FreeFn free_fn) {
    if (points < 2 || points > MAX_POINTS || max_series == 0 || max_series > MAX_SERIES_CAP ||
        !alloc || !free_fn)
        return false;
    if (slots_ && points == points_ && max_series == max_series_) return true;
    shutdown();
    slots_ = (Series *)alloc(sizeof(Series) * max_series);
    if (!slots_) return false;
    memset(slots_, 0, sizeof(Series) * max_series);
    points_ = points;
    max_series_ = max_series;
    alloc_ = alloc;
    free_ = free_fn;
    return true;
}

void Store::shutdown() {
    if (!slots_) return;
    release_all();
    free_(slots_);
    slots_ = nullptr;
    points_ = 0;
    max_series_ = 0;
}

int Store::acquire(const char *key, Agg agg, uint32_t window_s, bool *created) {
    if (created) *created = false;
    if (!slots_ || !key || !key[0] || window_s == 0) return -1;
    int free_slot = -1;
    for (int i = 0; i < max_series_; ++i) {
        Series &se = slots_[i];
        if (!se.used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (se.agg == agg && se.window_s == window_s && !strncmp(se.key, key, KEY_MAX)) return i;
    }
    if (free_slot < 0) return -1;
    Series &se = slots_[free_slot];
    memset(&se, 0, sizeof(se));
    se.val = (float *)alloc_(sizeof(float) * points_);
    se.cnt = (uint16_t *)alloc_(sizeof(uint16_t) * points_);
    if (!se.val || !se.cnt) {
        if (se.val) free_(se.val);
        if (se.cnt) free_(se.cnt);
        memset(&se, 0, sizeof(se));
        return -1;
    }
    for (int i = 0; i < points_; ++i)
        clear_slot(se, i);
    se.used = true;
    se.agg = agg;
    se.window_s = window_s;
    se.step_s = (double)window_s / (double)points_;
    se.has_head = false;
    strncpy(se.key, key, KEY_MAX - 1);
    se.key[KEY_MAX - 1] = 0;
    if (created) *created = true;
    return free_slot;
}

void Store::release_all() {
    if (!slots_) return;
    for (int i = 0; i < max_series_; ++i) {
        Series &se = slots_[i];
        if (se.val) free_(se.val);
        if (se.cnt) free_(se.cnt);
        memset(&se, 0, sizeof(se));
    }
}

const Series *Store::series(int s) const {
    if (!slots_ || s < 0 || s >= max_series_ || !slots_[s].used) return nullptr;
    return &slots_[s];
}

int Store::series_count() const {
    int n = 0;
    for (int i = 0; slots_ && i < max_series_; ++i)
        if (slots_[i].used) ++n;
    return n;
}

int Store::slot_of(int64_t bucket) const {
    int64_t m = bucket % (int64_t)points_;
    if (m < 0) m += points_;
    return (int)m;
}

void Store::clear_slot(Series &se, int slot) {
    se.val[slot] = NAN;
    se.cnt[slot] = 0;
}

bool Store::advance(int s, double now_s) {
    if (!series(s)) return false;
    Series &se = slots_[s];
    int64_t b = (int64_t)floor(now_s / se.step_s);
    if (!se.has_head) {
        se.head = b;
        se.has_head = true;
        ++se.version;
        return true;
    }
    if (b <= se.head) return false;
    int64_t gap = b - se.head;
    if (gap >= points_) {
        for (int i = 0; i < points_; ++i)
            clear_slot(se, i);
    } else {
        for (int64_t k = se.head + 1; k <= b; ++k)
            clear_slot(se, slot_of(k));
    }
    se.head = b;
    ++se.version;
    return true;
}

void Store::sample(int s, double now_s, float si) {
    if (!series(s) || !isfinite(si)) return;
    advance(s, now_s);
    Series &se = slots_[s];
    int slot = slot_of(se.head);
    uint16_t c = se.cnt[slot];
    float &v = se.val[slot];
    float before = v;
    if (c == 0 || (c & BACKFILL_BIT)) {
        // First live sample (a live sample replaces a backfilled value).
        v = si;
        se.cnt[slot] = 1;
        ++se.version;
        return;
    }
    switch (se.agg) {
    case Agg::Min:
        if (si < v) v = si;
        break;
    case Agg::Max:
        if (si > v) v = si;
        break;
    case Agg::Last:
        v = si;
        break;
    case Agg::Avg:
    default:
        v += (si - v) / (float)(c + 1);
        break;
    }
    if (c < COUNT_MASK) se.cnt[slot] = c + 1;
    if (v != before) ++se.version;
}

float Store::value_at(int s, int i) const {
    const Series *se = series(s);
    if (!se || !se->has_head || i < 0 || i >= points_) return NAN;
    int64_t b = se->head - (points_ - 1) + i;
    int slot = slot_of(b);
    return se->cnt[slot] ? se->val[slot] : NAN;
}

bool Store::is_backfilled(int s, int i) const {
    const Series *se = series(s);
    if (!se || !se->has_head || i < 0 || i >= points_) return false;
    int64_t b = se->head - (points_ - 1) + i;
    return (se->cnt[slot_of(b)] & BACKFILL_BIT) != 0;
}

int Store::merge_backfill(int s, double t_resp_s, const float *age_s, const float *vals, int n) {
    if (!series(s) || !age_s || !vals || n <= 0) return 0;
    advance(s, t_resp_s);
    Series &se = slots_[s];
    int written = 0;
    for (int r = 0; r < n; ++r) {
        if (!isfinite(vals[r]) || !isfinite(age_s[r])) continue;
        double t = t_resp_s - (double)age_s[r];
        int64_t b = (int64_t)floor(t / se.step_s);
        if (b > se.head || b <= se.head - points_) continue;  // outside the window
        int slot = slot_of(b);
        uint16_t c = se.cnt[slot];
        if (c != 0 && !(c & BACKFILL_BIT)) continue;  // live data wins
        float x = vals[r];
        if (c == 0) {
            se.val[slot] = x;
            se.cnt[slot] = BACKFILL_BIT | 1;
            ++written;
            continue;
        }
        // Two server rows in one bucket (server buckets need not align with
        // ours): combine with the series' own aggregate.
        uint16_t k = c & COUNT_MASK;
        float &v = se.val[slot];
        switch (se.agg) {
        case Agg::Min:
            if (x < v) v = x;
            break;
        case Agg::Max:
            if (x > v) v = x;
            break;
        case Agg::Last:
            v = x;  // rows arrive oldest -> newest
            break;
        case Agg::Avg:
        default:
            v += (x - v) / (float)(k + 1);
            break;
        }
        if (k < COUNT_MASK) se.cnt[slot] = BACKFILL_BIT | (uint16_t)(k + 1);
    }
    ++se.version;
    return written;
}

}  // namespace trend
