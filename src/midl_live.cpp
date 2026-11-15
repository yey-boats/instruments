// GET /api/midl/live, pure half. See include/midl_live.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_live.h"

#include <math.h>
#include <string.h>

namespace midl::live {

namespace {

bool key_char(char c, bool allow_colon) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '-' || (allow_colon && c == ':');
}

bool valid_key(const char *p, size_t max_len, bool allow_colon) {
    if (!p || !p[0]) return false;
    size_t n = strlen(p);
    if (n >= max_len) return false;
    for (size_t i = 0; i < n; ++i)
        if (!key_char(p[i], allow_colon)) return false;
    return true;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

}  // namespace

bool valid_path(const char *p) {
    if (!valid_key(p, KEY_LEN, true)) return false;
    size_t n = strlen(p);
    if (p[0] == '.' || p[n - 1] == '.') return false;
    return strstr(p, "..") == nullptr;
}

bool valid_local(const char *id) {
    return valid_key(id, LOCAL_LEN, false);
}

void parse_list(const char *csv, KeyList &out, bool local_ids) {
    out.n = 0;
    out.truncated = false;
    out.invalid = 0;
    if (!csv) return;
    size_t total = strnlen(csv, MAX_PARAM + 1);
    if (total > MAX_PARAM) {
        total = MAX_PARAM;
        out.truncated = true;
    }
    size_t i = 0;
    while (i <= total) {
        size_t j = i;
        while (j < total && csv[j] != ',')
            ++j;
        size_t a = i, b = j;
        while (a < b && is_space(csv[a]))
            ++a;
        while (b > a && is_space(csv[b - 1]))
            --b;
        if (b > a) {
            char item[KEY_LEN];
            bool fits = b - a < sizeof(item);
            if (fits) {
                memcpy(item, csv + a, b - a);
                item[b - a] = '\0';
            }
            bool ok = fits && (local_ids ? valid_local(item) : valid_path(item));
            if (!ok) {
                ++out.invalid;
            } else {
                bool dup = false;
                for (size_t k = 0; k < out.n && !dup; ++k)
                    dup = strcmp(out.key[k], item) == 0;
                if (!dup) {
                    if (out.n < MAX_KEYS)
                        memcpy(out.key[out.n++], item, strlen(item) + 1);
                    else
                        out.truncated = true;
                }
            }
        }
        i = j + 1;
    }
}

static void put_value(JsonObject obj, const char *key, const Value &v) {
    if (v.kind == Value::Number && isfinite(v.num))
        obj[key] = v.num;
    else if (v.kind == Value::Text)
        obj[key] = (const char *)v.text;  // copied: v is reused per key
    else
        obj[key] = nullptr;
}

void build_json(JsonObject out, uint32_t t_ms, const KeyList &paths, const KeyList &locals,
                Resolver &r) {
    out["t"] = t_ms;
    JsonObject vals = out["values"].to<JsonObject>();
    JsonObject locs = out["locals"].to<JsonObject>();
    Value v;
    for (size_t i = 0; i < paths.n; ++i) {
        memset(&v, 0, sizeof(v));
        r.path(paths.key[i], v);
        v.text[TEXT_LEN - 1] = '\0';
        put_value(vals, paths.key[i], v);
    }
    for (size_t i = 0; i < locals.n; ++i) {
        memset(&v, 0, sizeof(v));
        r.local(locals.key[i], v);
        v.text[TEXT_LEN - 1] = '\0';
        put_value(locs, locals.key[i], v);
    }
    if (paths.truncated || locals.truncated) out["truncated"] = true;
}

void WatchSet::clear() {
    memset(path_, 0, sizeof(path_));
    memset(last_, 0, sizeof(last_));
    n_ = 0;
}

bool WatchSet::has(const char *path) const {
    for (size_t i = 0; i < n_; ++i)
        if (strcmp(path_[i], path) == 0) return true;
    return false;
}

void WatchSet::remove_(size_t i) {
    for (size_t k = i + 1; k < n_; ++k) {
        memcpy(path_[k - 1], path_[k], KEY_LEN);
        last_[k - 1] = last_[k];
    }
    --n_;
    memset(path_[n_], 0, KEY_LEN);
    last_[n_] = 0;
}

bool WatchSet::touch(const char *path, uint32_t now_ms) {
    if (!valid_path(path)) return false;
    for (size_t i = 0; i < n_; ++i) {
        if (strcmp(path_[i], path) == 0) {
            last_[i] = now_ms;
            return false;
        }
    }
    if (n_ >= CAP) {
        size_t oldest = 0;
        for (size_t i = 1; i < n_; ++i)
            if ((uint32_t)(now_ms - last_[i]) > (uint32_t)(now_ms - last_[oldest])) oldest = i;
        remove_(oldest);
    }
    memcpy(path_[n_], path, strlen(path) + 1);
    last_[n_] = now_ms;
    ++n_;
    return true;
}

bool WatchSet::expire(uint32_t now_ms, uint32_t ttl_ms) {
    bool changed = false;
    for (size_t i = n_; i-- > 0;) {
        if ((uint32_t)(now_ms - last_[i]) > ttl_ms) {
            remove_(i);
            changed = true;
        }
    }
    return changed;
}

}  // namespace midl::live
