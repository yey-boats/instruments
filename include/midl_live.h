#pragma once

// GET /api/midl/live: live values for the device-hosted MIDL editor's preview
// (docs/midl/midl-only.md, "Device-hosted editor"). Pure half, host-tested:
// query-list parsing + caps, the JSON response shape and the editor watch set
// (paths the editor asked for, kept subscribed ~10 s after the last request).
// The device glue (resolvers, SignalK subscription) is in web.cpp/signalk.cpp.
//
//   GET /api/midl/live?paths=<p1,p2,...>&locals=<id1,id2,...>
//   -> {"t":<ms>,"values":{"<path>":number|string|null},
//       "locals":{"<id>":number|string|null}}
//
// Both KeyList and WatchSet are a few KB: never on a task stack (function-static
// in PSRAM on the device; see CLAUDE.md memory traps).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace midl::live {

constexpr size_t MAX_KEYS = 32;     // paths (and, separately, locals) per request
constexpr size_t KEY_LEN = 80;      // = sk::PathStore::PATH_LEN (longest path + NUL)
constexpr size_t LOCAL_LEN = 32;    // longest local id + NUL
constexpr size_t MAX_PARAM = 4096;  // longest accepted `paths` / `locals` value
constexpr uint32_t WATCH_TTL_MS = 10000;
constexpr size_t TEXT_LEN = 64;  // longest string value reported

// A SignalK path the editor may ask for: 1..79 chars of [A-Za-z0-9_.:-], no
// leading/trailing '.', no "..". Wildcards ('*') are refused: the watch must
// not subscribe a whole subtree.
bool valid_path(const char *p);
// A local id: 1..31 chars of [A-Za-z0-9_.-].
bool valid_local(const char *id);

struct KeyList {
    char key[MAX_KEYS][KEY_LEN];
    size_t n;
    bool truncated;    // more valid keys than MAX_KEYS (the rest are dropped)
    uint16_t invalid;  // keys that failed validation (dropped)
};

// Split a comma-separated list (already URL-decoded) into `out`: trims spaces,
// skips empty items, drops invalid ones, de-duplicates, keeps the first
// MAX_KEYS. `csv` longer than MAX_PARAM is treated as truncated at MAX_PARAM.
// `local_ids` selects valid_local + LOCAL_LEN instead of valid_path.
void parse_list(const char *csv, KeyList &out, bool local_ids);

struct Value {
    enum Kind : uint8_t { Null, Number, Text } kind;
    double num;
    char text[TEXT_LEN];
};

// Value sources. The device resolves paths through widget_data (raw SignalK
// SI, PathStore fallback) and locals through the MIDL painter's resolvers.
class Resolver {
  public:
    virtual ~Resolver() = default;
    virtual void path(const char *path, Value &v) = 0;
    virtual void local(const char *id, Value &v) = 0;
};

// Fill `out` with {t, values, locals}. A non-finite number is reported as null.
void build_json(JsonObject out, uint32_t t_ms, const KeyList &paths, const KeyList &locals,
                Resolver &r);

// Bounded set of paths the editor is watching. touch() refreshes (or adds) a
// path; when full the least recently touched path is evicted. expire() drops
// paths not touched for `ttl_ms`. Both return true when the SET changed (a
// refresh of a present path is not a change).
class WatchSet {
  public:
    static constexpr size_t CAP = MAX_KEYS;

    void clear();
    bool touch(const char *path, uint32_t now_ms);
    bool expire(uint32_t now_ms, uint32_t ttl_ms = WATCH_TTL_MS);
    size_t size() const { return n_; }
    const char *at(size_t i) const { return i < n_ ? path_[i] : nullptr; }
    bool has(const char *path) const;

  private:
    char path_[CAP][KEY_LEN] = {};
    uint32_t last_[CAP] = {};
    size_t n_ = 0;
    void remove_(size_t i);
};

}  // namespace midl::live
