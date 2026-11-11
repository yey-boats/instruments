// instruments#20: on-device MIDL validation. See include/midl_validate.h.
//
// Stages 1-4 port midl's validateDocument (midl/ts/src/index.ts):
//   structural = midl/schemas/yb-midl-config.schema.json (validate.ts / ajv)
//   semantic   = midl/ts/src/semantic.ts
//   version    = midl/ts/src/version.ts
//   capability = midl/ts/src/satisfy.ts (+ presets.ts expand/countTiles/depth)
// Keep them in step with the pinned midl submodule; test/test_midl_validate
// runs midl's conformance corpus against this file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_validate.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "midl_kinds.h"
#include "midl_limits.h"

namespace midl::validate {

// Stage 5 limits must match the buffers the renderer copies into.
static_assert(STR_MAX + 1 == midl::FirmwareLimits::str_len, "STR_MAX tracks layout::STR_LEN");
static_assert(PATH_MAX_LEN + 1 == midl::FirmwareLimits::path_len, "PATH_MAX_LEN tracks PATH_LEN");
static_assert(PATH_MAX_LEN == midl::kinds::ACTION_TARGET_MAX, "targets share the path buffer");
static_assert(PUT_STRING_MAX + 3 == midl::FirmwareLimits::str_len,
              "\"%s\" + NUL must fit the 32-byte action value buffer");

namespace {

// One shared JSON-pointer buffer for the whole walk (keeps every recursion
// frame small: the web task stack is tiny). push() appends a segment and
// returns the old length; pop() restores it.
struct Ctx {
    char path[sizeof(Issue::path)];
    size_t len;
    Issue *out;
};

// JSON pointer segment escaping (RFC 6901): '~' -> "~0", '/' -> "~1".
size_t push(Ctx &c, const char *seg) {
    size_t saved = c.len;
    const size_t cap = sizeof(c.path) - 1;
    if (c.len < cap) c.path[c.len++] = '/';
    for (const char *p = seg; *p && c.len < cap; ++p) {
        if (*p == '~' || *p == '/') {
            if (c.len + 2 > cap) break;
            c.path[c.len++] = '~';
            c.path[c.len++] = *p == '~' ? '0' : '1';
        } else {
            c.path[c.len++] = *p;
        }
    }
    c.path[c.len] = 0;
    return saved;
}

size_t push(Ctx &c, size_t idx) {
    char seg[16];
    snprintf(seg, sizeof(seg), "%u", (unsigned)idx);
    return push(c, seg);
}

void pop(Ctx &c, size_t saved) {
    c.len = saved;
    c.path[saved] = 0;
}

// Record an error at the current path. Always returns false (for `return fail(...)`).
bool fail(Ctx &c, const char *fmt, ...) {
    snprintf(c.out->path, sizeof(c.out->path), "%s", c.len ? c.path : "/");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c.out->message, sizeof(c.out->message), fmt, ap);
    va_end(ap);
    return false;
}

// Record an error at current path + "/seg".
bool fail_at(Ctx &c, const char *seg, const char *fmt, ...) {
    size_t s = push(c, seg);
    snprintf(c.out->path, sizeof(c.out->path), "%s", c.path);
    pop(c, s);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c.out->message, sizeof(c.out->message), fmt, ap);
    va_end(ap);
    return false;
}

void reset(Ctx &c, Issue &out) {
    c.path[0] = 0;
    c.len = 0;
    c.out = &out;
}

// --- JSON value helpers ------------------------------------------------------

bool is_obj(JsonVariantConst v) {
    return v.is<JsonObjectConst>();
}
bool is_arr(JsonVariantConst v) {
    return v.is<JsonArrayConst>();
}
bool is_str(JsonVariantConst v) {
    return v.is<const char *>();
}
bool is_num(JsonVariantConst v) {
    return !v.is<bool>() && (v.is<double>() || v.is<long long>() || v.is<unsigned long long>());
}
// JSON Schema "integer": any number with a zero fractional part (2.0 counts).
bool is_int(JsonVariantConst v) {
    if (!is_num(v)) return false;
    if (v.is<long long>() || v.is<unsigned long long>()) return true;
    double d = v.as<double>();
    return isfinite(d) && floor(d) == d;
}
bool has(JsonObjectConst o, const char *key) {
    for (JsonPairConst kv : o)
        if (strcmp(kv.key().c_str(), key) == 0) return true;
    return false;
}
const char *str(JsonVariantConst v) {
    return v.is<const char *>() ? v.as<const char *>() : "";
}
bool non_empty_trimmed(JsonVariantConst v) {
    if (!is_str(v)) return false;
    for (const char *p = v.as<const char *>(); *p; ++p)
        if (!(*p == ' ' || (*p >= '\t' && *p <= '\r'))) return true;
    return false;
}
bool in_list(JsonArrayConst list, const char *s) {
    for (JsonVariantConst v : list)
        if (is_str(v) && strcmp(v.as<const char *>(), s) == 0) return true;
    return false;
}
bool one_of(const char *s, const char *const *opts) {
    for (; *opts; ++opts)
        if (strcmp(s, *opts) == 0) return true;
    return false;
}

// ^\d+\.\d+\.\d+$ ; parsed components saturate (only == / <= comparisons).
bool parse_version(const char *s, unsigned long long v[3]) {
    for (int i = 0; i < 3; ++i) {
        if (*s < '0' || *s > '9') return false;
        unsigned long long n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n > 1000000000000ULL ? n : n * 10 + (unsigned)(*s - '0');
            ++s;
        }
        v[i] = n;
        if (i < 2) {
            if (*s != '.') return false;
            ++s;
        }
    }
    return *s == 0;
}

// =============================================================================
// Stage 1: structural (yb-midl-config.schema.json)
// =============================================================================

const char *const SOURCE_KINDS[] = {"signalk", "local", "const", "computed", nullptr};
const char *const ACTION_KINDS_SCHEMA[] = {"put", "nav", "command", nullptr};

bool s_string(Ctx &c, JsonObjectConst o, const char *key, bool non_empty = false) {
    if (!has(o, key)) return true;
    JsonVariantConst v = o[key];
    if (!is_str(v)) return fail_at(c, key, "%s must be a string", key);
    if (non_empty && !v.as<const char *>()[0])
        return fail_at(c, key, "%s must NOT have fewer than 1 characters", key);
    return true;
}
bool s_number(Ctx &c, JsonObjectConst o, const char *key) {
    if (has(o, key) && !is_num(o[key])) return fail_at(c, key, "%s must be number", key);
    return true;
}
bool s_int_min(Ctx &c, JsonObjectConst o, const char *key, double min) {
    if (!has(o, key)) return true;
    if (!is_int(o[key])) return fail_at(c, key, "%s must be integer", key);
    if (o[key].as<double>() < min) return fail_at(c, key, "%s must be >= %g", key, min);
    return true;
}
bool s_enum(Ctx &c, JsonObjectConst o, const char *key, const char *const *opts) {
    if (!has(o, key)) return true;
    if (!is_str(o[key]) || !one_of(o[key].as<const char *>(), opts))
        return fail_at(c, key, "%s must be equal to one of the allowed values", key);
    return true;
}
bool s_object(Ctx &c, JsonObjectConst o, const char *key) {
    if (has(o, key) && !is_obj(o[key])) return fail_at(c, key, "%s must be object", key);
    return true;
}
bool s_required(Ctx &c, JsonObjectConst o, const char *key) {
    if (!has(o, key)) return fail(c, "must have required property '%s'", key);
    return true;
}

bool s_meta(Ctx &c, JsonObjectConst parent) {
    if (!has(parent, "meta")) return true;
    size_t s = push(c, "meta");
    JsonVariantConst v = parent["meta"];
    bool ok = is_obj(v) ? true : fail(c, "meta must be object");
    if (ok) {
        JsonObjectConst m = v.as<JsonObjectConst>();
        ok = s_string(c, m, "useCase") && s_string(c, m, "agentNotes");
        if (ok && has(m, "tags")) {
            size_t t = push(c, "tags");
            if (!is_arr(m["tags"])) ok = fail(c, "tags must be array");
            size_t i = 0;
            for (JsonVariantConst tag : m["tags"].as<JsonArrayConst>()) {
                if (!ok) break;
                if (!is_str(tag)) {
                    size_t u = push(c, i);
                    ok = fail(c, "tag must be string");
                    pop(c, u);
                }
                ++i;
            }
            pop(c, t);
        }
    }
    pop(c, s);
    return ok;
}

// $defs/source, at the current path.
bool s_source(Ctx &c, JsonVariantConst v) {
    if (!is_obj(v)) return fail(c, "source must be object");
    JsonObjectConst o = v.as<JsonObjectConst>();
    if (!s_required(c, o, "kind")) return false;
    if (!s_enum(c, o, "kind", SOURCE_KINDS)) return false;
    if (!s_string(c, o, "path") || !s_string(c, o, "id") || !s_string(c, o, "expr")) return false;
    const char *k = str(o["kind"]);
    if (!strcmp(k, "signalk")) return s_required(c, o, "path");
    if (!strcmp(k, "local")) return s_required(c, o, "id");
    if (!strcmp(k, "const")) return s_required(c, o, "value");
    if (!strcmp(k, "computed")) return s_required(c, o, "expr");
    return true;
}

bool s_action(Ctx &c, JsonVariantConst v) {
    if (!is_obj(v)) return fail(c, "action must be object");
    JsonObjectConst o = v.as<JsonObjectConst>();
    if (!s_required(c, o, "kind") || !s_enum(c, o, "kind", ACTION_KINDS_SCHEMA) ||
        !s_string(c, o, "target"))
        return false;
    const char *k = str(o["kind"]);
    if (!strcmp(k, "put")) return s_required(c, o, "target") && s_required(c, o, "value");
    if (!strcmp(k, "nav") || !strcmp(k, "command")) return s_required(c, o, "target");
    return true;
}

// Array of objects whose members are checked by `each` (at /key/i).
template <typename F> bool s_array_of(Ctx &c, JsonObjectConst o, const char *key, F each) {
    if (!has(o, key)) return true;
    size_t s = push(c, key);
    bool ok = is_arr(o[key]) ? true : fail(c, "%s must be array", key);
    size_t i = 0;
    for (JsonVariantConst item : o[key].as<JsonArrayConst>()) {
        if (!ok) break;
        size_t t = push(c, i++);
        ok = each(item);
        pop(c, t);
    }
    pop(c, s);
    return ok;
}

bool s_style(Ctx &c, JsonObjectConst el) {
    if (!has(el, "style")) return true;
    size_t s = push(c, "style");
    bool ok = is_obj(el["style"]) ? true : fail(c, "style must be object");
    if (ok) {
        JsonObjectConst st = el["style"].as<JsonObjectConst>();
        static const char *const SCALE[] = {"fixed", "metric", nullptr};
        static const char *const SHAPE[] = {"round", "band", nullptr};
        if (has(st, "range")) {
            size_t r = push(c, "range");
            JsonVariantConst rv = st["range"];
            if (!is_arr(rv))
                ok = fail(c, "range must be array");
            else if (rv.size() != 2)
                ok = fail(c, "range must have exactly 2 items");
            size_t i = 0;
            for (JsonVariantConst x : rv.as<JsonArrayConst>()) {
                if (!ok) break;
                if (!is_num(x)) {
                    size_t u = push(c, i);
                    ok = fail(c, "range item must be number");
                    pop(c, u);
                }
                ++i;
            }
            pop(c, r);
        }
        ok = ok && s_array_of(c, st, "zones", [&](JsonVariantConst z) {
                 if (!is_obj(z)) return fail(c, "zone must be object");
                 JsonObjectConst zo = z.as<JsonObjectConst>();
                 return s_required(c, zo, "lt") && s_required(c, zo, "color") &&
                        s_number(c, zo, "lt") && s_string(c, zo, "color");
             });
        if (ok && has(st, "size") && !is_str(st["size"]) && !is_num(st["size"]))
            ok = fail_at(c, "size", "size must be string,number");
        ok = ok && s_string(c, st, "color") && s_string(c, st, "colorRole") &&
             s_enum(c, st, "scale", SCALE) && s_string(c, st, "span");
        ok = ok && s_array_of(c, st, "sectors", [&](JsonVariantConst z) {
                 if (!is_obj(z)) return fail(c, "sector must be object");
                 JsonObjectConst zo = z.as<JsonObjectConst>();
                 return s_required(c, zo, "from") && s_required(c, zo, "to") &&
                        s_required(c, zo, "color") && s_number(c, zo, "from") &&
                        s_number(c, zo, "to") && s_string(c, zo, "color");
             });
        if (ok && has(st, "hull") && !st["hull"].is<bool>())
            ok = fail_at(c, "hull", "hull must be boolean");
        ok = ok && s_enum(c, st, "shape", SHAPE) && s_number(c, st, "center");
    }
    pop(c, s);
    return ok;
}

bool s_format(Ctx &c, JsonObjectConst el) {
    if (!has(el, "format")) return true;
    size_t s = push(c, "format");
    bool ok = is_obj(el["format"]) ? true : fail(c, "format must be object");
    if (ok) {
        JsonObjectConst f = el["format"].as<JsonObjectConst>();
        ok = s_string(c, f, "unit") && s_int_min(c, f, "decimals", 0) &&
             s_int_min(c, f, "precision", 0);
        if (ok && has(f, "side") && !f["side"].is<bool>() && !is_str(f["side"]))
            ok = fail_at(c, "side", "side must be boolean,string");
    }
    pop(c, s);
    return ok;
}

bool s_element(Ctx &c, JsonVariantConst v) {
    if (!is_obj(v)) return fail(c, "element must be object");
    JsonObjectConst el = v.as<JsonObjectConst>();
    if (!s_required(c, el, "type") || !s_string(c, el, "type", true) || !s_string(c, el, "name") ||
        !s_meta(c, el))
        return false;
    if (has(el, "bindings")) {
        size_t s = push(c, "bindings");
        bool ok = is_obj(el["bindings"]) ? true : fail(c, "bindings must be object");
        for (JsonPairConst kv : el["bindings"].as<JsonObjectConst>()) {
            if (!ok) break;
            size_t t = push(c, kv.key().c_str());
            ok = s_source(c, kv.value());
            pop(c, t);
        }
        pop(c, s);
        if (!ok) return false;
    }
    if (!s_style(c, el) || !s_format(c, el)) return false;
    static const char *const MARKER_KINDS[] = {"rim", "vector", nullptr};
    if (!s_array_of(c, el, "markers", [&](JsonVariantConst m) {
            if (!is_obj(m)) return fail(c, "marker must be object");
            JsonObjectConst mo = m.as<JsonObjectConst>();
            if (!s_string(c, mo, "glyph") || !s_string(c, mo, "color")) return false;
            if (has(mo, "dir")) {
                size_t d = push(c, "dir");
                bool ok = s_source(c, mo["dir"]);
                pop(c, d);
                if (!ok) return false;
            }
            return s_enum(c, mo, "kind", MARKER_KINDS);
        }))
        return false;
    if (has(el, "action")) {
        size_t s = push(c, "action");
        bool ok = s_action(c, el["action"]);
        pop(c, s);
        if (!ok) return false;
    }
    return s_string(c, el, "zoom");
}

// $defs/node: oneOf {element}, {flow,children}, {rows,cols,cells}, {preset},
// spacer {colSpan?,rowSpan?}. The branch is chosen by the node's keys; a node
// that fits none (or mixes shapes) fails at its own path, like ajv's oneOf.
bool keys_subset(JsonObjectConst o, const char *const *allowed) {
    for (JsonPairConst kv : o)
        if (!one_of(kv.key().c_str(), allowed)) return false;
    return true;
}

bool s_node(Ctx &c, JsonVariantConst v);

bool s_node_list(Ctx &c, JsonObjectConst o, const char *key) {
    size_t s = push(c, key);
    JsonVariantConst a = o[key];
    bool ok = true;
    if (!is_arr(a))
        ok = fail(c, "%s must be array", key);
    else if (a.size() == 0)
        ok = fail(c, "%s must NOT have fewer than 1 items", key);
    size_t i = 0;
    for (JsonVariantConst child : a.as<JsonArrayConst>()) {
        if (!ok) break;
        size_t t = push(c, i++);
        ok = s_node(c, child);
        pop(c, t);
    }
    pop(c, s);
    return ok;
}

bool s_node(Ctx &c, JsonVariantConst v) {
    static const char *const LEAF[] = {"element", "colSpan", "rowSpan", nullptr};
    static const char *const SPLIT[] = {"flow", "children", "weights", nullptr};
    static const char *const GRID[] = {"rows", "cols", "cells", nullptr};
    static const char *const PRESET[] = {"preset", "slots", nullptr};
    static const char *const SPACER[] = {"colSpan", "rowSpan", nullptr};
    static const char *const FLOWS[] = {"row", "col", nullptr};
    if (!is_obj(v))
        return fail(c, "layout node must be object (must match exactly one schema in oneOf)");
    JsonObjectConst o = v.as<JsonObjectConst>();
    const char *shape = "must match exactly one schema in oneOf";
    if (has(o, "element")) {
        if (!keys_subset(o, LEAF)) return fail(c, "%s", shape);
        return s_string(c, o, "element", true) && s_int_min(c, o, "colSpan", 1) &&
               s_int_min(c, o, "rowSpan", 1);
    }
    if (has(o, "flow") || has(o, "children") || has(o, "weights")) {
        if (!keys_subset(o, SPLIT) || !has(o, "flow") || !has(o, "children"))
            return fail(c, "%s", shape);
        if (!s_enum(c, o, "flow", FLOWS)) return false;
        if (has(o, "weights")) {
            size_t s = push(c, "weights");
            bool ok = is_arr(o["weights"]) ? true : fail(c, "weights must be array");
            size_t i = 0;
            for (JsonVariantConst w : o["weights"].as<JsonArrayConst>()) {
                if (!ok) break;
                if (!is_num(w) || w.as<double>() <= 0) {
                    size_t t = push(c, i);
                    ok = fail(c, "weight must be a number > 0");
                    pop(c, t);
                }
                ++i;
            }
            pop(c, s);
            if (!ok) return false;
        }
        return s_node_list(c, o, "children");
    }
    if (has(o, "rows") || has(o, "cols") || has(o, "cells")) {
        if (!keys_subset(o, GRID) || !has(o, "rows") || !has(o, "cols") || !has(o, "cells"))
            return fail(c, "%s", shape);
        return s_int_min(c, o, "rows", 1) && s_int_min(c, o, "cols", 1) &&
               s_node_list(c, o, "cells");
    }
    if (has(o, "preset") || has(o, "slots")) {
        if (!keys_subset(o, PRESET) || !has(o, "preset")) return fail(c, "%s", shape);
        if (!s_string(c, o, "preset", true)) return false;
        if (has(o, "slots")) {
            size_t s = push(c, "slots");
            bool ok = is_arr(o["slots"]) ? true : fail(c, "slots must be array");
            size_t i = 0;
            for (JsonVariantConst sl : o["slots"].as<JsonArrayConst>()) {
                if (!ok) break;
                if (!is_str(sl)) {
                    size_t t = push(c, i);
                    ok = fail(c, "slot must be string");
                    pop(c, t);
                }
                ++i;
            }
            pop(c, s);
            if (!ok) return false;
        }
        return true;
    }
    if (!keys_subset(o, SPACER)) return fail(c, "%s", shape);
    return s_int_min(c, o, "colSpan", 1) && s_int_min(c, o, "rowSpan", 1);
}

bool s_screen(Ctx &c, JsonVariantConst v) {
    if (!is_obj(v)) return fail(c, "screen must be object");
    JsonObjectConst sc = v.as<JsonObjectConst>();
    if (!s_required(c, sc, "id") || !s_required(c, sc, "elements") ||
        !s_required(c, sc, "layout") || !s_string(c, sc, "id", true) || !s_meta(c, sc))
        return false;
    {
        size_t s = push(c, "elements");
        bool ok = is_obj(sc["elements"]) ? true : fail(c, "elements must be object");
        for (JsonPairConst kv : sc["elements"].as<JsonObjectConst>()) {
            if (!ok) break;
            size_t t = push(c, kv.key().c_str());
            ok = s_element(c, kv.value());
            pop(c, t);
        }
        pop(c, s);
        if (!ok) return false;
    }
    {
        size_t s = push(c, "layout");
        bool ok = s_node(c, sc["layout"]);
        pop(c, s);
        if (!ok) return false;
    }
    return s_array_of(c, sc, "variants", [&](JsonVariantConst vv) {
        if (!is_obj(vv)) return fail(c, "variant must be object");
        JsonObjectConst vo = vv.as<JsonObjectConst>();
        if (!s_required(c, vo, "class") || !s_required(c, vo, "layout") ||
            !s_string(c, vo, "class", true))
            return false;
        size_t s = push(c, "layout");
        bool ok = s_node(c, vo["layout"]);
        pop(c, s);
        return ok;
    });
}

bool s_alarm(Ctx &c, JsonVariantConst v) {
    static const char *const LEVELS[] = {"info", "warn", "alarm", "emergency", nullptr};
    if (!is_obj(v)) return fail(c, "alarm must be object");
    JsonObjectConst a = v.as<JsonObjectConst>();
    if (!s_required(c, a, "id") || !s_required(c, a, "source") || !s_required(c, a, "level") ||
        !s_required(c, a, "message"))
        return false;
    if (!has(a, "lt") && !has(a, "gt")) return fail(c, "must have required property 'lt' or 'gt'");
    if (!s_string(c, a, "id", true)) return false;
    {
        size_t s = push(c, "source");
        bool ok = s_source(c, a["source"]);
        pop(c, s);
        if (!ok) return false;
    }
    return s_enum(c, a, "level", LEVELS) && s_number(c, a, "lt") && s_number(c, a, "gt") &&
           s_string(c, a, "message", true);
}

bool structural(Ctx &c, JsonVariantConst doc) {
    if (!is_obj(doc)) return fail(c, "document must be object");
    JsonObjectConst d = doc.as<JsonObjectConst>();
    if (!s_required(c, d, "midl") || !s_required(c, d, "screens")) return false;
    unsigned long long ver[3];
    if (!is_str(d["midl"])) return fail_at(c, "midl", "midl must be string");
    if (!parse_version(d["midl"].as<const char *>(), ver))
        return fail_at(c, "midl", "midl must match pattern \"^\\d+\\.\\d+\\.\\d+$\"");
    if (!s_object(c, d, "settings") || !s_object(c, d, "defaults") || !s_object(c, d, "presets"))
        return false;
    {
        size_t s = push(c, "screens");
        JsonVariantConst sc = d["screens"];
        bool ok = true;
        if (!is_arr(sc))
            ok = fail(c, "screens must be array");
        else if (sc.size() == 0)
            ok = fail(c, "screens must NOT have fewer than 1 items");
        size_t i = 0;
        for (JsonVariantConst screen : sc.as<JsonArrayConst>()) {
            if (!ok) break;
            size_t t = push(c, i++);
            ok = s_screen(c, screen);
            pop(c, t);
        }
        pop(c, s);
        if (!ok) return false;
    }
    if (!s_array_of(c, d, "alarms", [&](JsonVariantConst a) { return s_alarm(c, a); }))
        return false;
    return s_meta(c, d);
}

// =============================================================================
// Stage 2: semantic (midl/ts/src/semantic.ts). Runs on a structurally valid doc.
// =============================================================================

// Required bindings per registered element type (ELEMENT_REQUIRED_BINDINGS).
// Unregistered types are only a warning there, so they pass here too.
bool requires_value(const char *type, bool &registered) {
    static const char *const VALUE_TYPES[] = {"single-value", "text",      "gauge",   "bar",
                                              "trend",        "autopilot", "compass", "windrose",
                                              "clinometer",   nullptr};
    registered = one_of(type, VALUE_TYPES) || !strcmp(type, "button");
    return one_of(type, VALUE_TYPES);
}

bool utf8_len_over(const char *s, size_t max) {
    return strnlen(s, max + 1) > max;  // strlen counts UTF-8 bytes, like the firmware
}

bool sem_source(Ctx &c, JsonVariantConst src) {
    if (!is_obj(src)) return true;
    const char *k = str(src["kind"]);
    if (!strcmp(k, "signalk") && !non_empty_trimmed(src["path"]))
        return fail_at(c, "path", "source.kind \"signalk\" requires a non-empty path");
    if (!strcmp(k, "local") && !non_empty_trimmed(src["id"]))
        return fail_at(c, "id", "source.kind \"local\" requires a non-empty id");
    if (!strcmp(k, "computed") && !non_empty_trimmed(src["expr"]))
        return fail_at(c, "expr", "source.kind \"computed\" requires a non-empty expr");
    if (!strcmp(k, "const") && !has(src.as<JsonObjectConst>(), "value"))
        return fail_at(c, "value", "source.kind \"const\" requires a value");
    return true;
}

bool sem_element(Ctx &c, const char *id, JsonObjectConst el) {
    bool registered = false;
    JsonObjectConst bindings = el["bindings"].as<JsonObjectConst>();
    if (requires_value(str(el["type"]), registered) && !has(bindings, "value")) {
        size_t s = push(c, "bindings");
        bool ok = fail_at(c, "value", "element \"%s\" of type \"%s\" requires a \"value\" binding",
                          id, str(el["type"]));
        pop(c, s);
        return ok;
    }
    {
        size_t s = push(c, "bindings");
        bool ok = true;
        for (JsonPairConst kv : bindings) {
            if (!ok) break;
            size_t t = push(c, kv.key().c_str());
            ok = sem_source(c, kv.value());
            pop(c, t);
        }
        pop(c, s);
        if (!ok) return false;
    }
    JsonVariantConst act = el["action"];
    if (is_obj(act) && is_str(act["target"])) {
        const char *kind = str(act["kind"]);
        const char *target = act["target"].as<const char *>();
        size_t s = push(c, "action");
        bool ok = true;
        if (!strcmp(kind, "put") && !midl::kinds::put_target_ok(target))
            ok = fail_at(c, "target",
                         "put target must be a SignalK path (dotted segments of [A-Za-z0-9_-], "
                         "at most %u chars)",
                         (unsigned)midl::kinds::ACTION_TARGET_MAX);
        else if (!strcmp(kind, "command") && utf8_len_over(target, midl::kinds::ACTION_TARGET_MAX))
            ok = fail_at(c, "target", "command target is longer than %u bytes",
                         (unsigned)midl::kinds::ACTION_TARGET_MAX);
        pop(c, s);
        if (!ok) return false;
    }
    JsonVariantConst range = el["style"]["range"];
    if (is_arr(range) && range.size() == 2 && is_num(range[0]) && is_num(range[1])) {
        double lo = range[0].as<double>(), hi = range[1].as<double>();
        if (hi <= lo) {
            size_t s = push(c, "style");
            bool ok = fail_at(c, "range",
                              "element \"%s\" range [%g, %g] is invalid: max must be greater "
                              "than min",
                              id, lo, hi);
            pop(c, s);
            return ok;
        }
    }
    return true;
}

bool known_preset(const char *p) {
    return !strcmp(p, "full") || !strcmp(p, "hero-split");
}

// checkNode: internal layout arithmetic (refs resolved by sem_refs afterwards).
bool sem_node(Ctx &c, JsonVariantConst n) {
    JsonObjectConst o = n.as<JsonObjectConst>();
    if (has(o, "element")) return true;
    if (has(o, "preset")) {
        if (!known_preset(str(o["preset"])))
            return fail_at(c, "preset",
                           "preset \"%s\" is not a known preset; expected one of: full, hero-split",
                           str(o["preset"]));
        return true;
    }
    if (has(o, "children")) {
        JsonArrayConst kids = o["children"].as<JsonArrayConst>();
        if (has(o, "weights") && o["weights"].size() != kids.size())
            return fail_at(c, "weights",
                           "weights length must match children length (%u weights vs %u "
                           "children)",
                           (unsigned)o["weights"].size(), (unsigned)kids.size());
        size_t s = push(c, "children");
        size_t i = 0;
        bool ok = true;
        for (JsonVariantConst k : kids) {
            if (!ok) break;
            size_t t = push(c, i++);
            ok = sem_node(c, k);
            pop(c, t);
        }
        pop(c, s);
        return ok;
    }
    if (has(o, "cells")) {
        long long rows = o["rows"].as<long long>(), cols = o["cols"].as<long long>();
        long long total = rows * cols;
        JsonArrayConst cells = o["cells"].as<JsonArrayConst>();
        bool spans = false;
        for (JsonVariantConst cell : cells) {
            JsonObjectConst co = cell.as<JsonObjectConst>();
            if ((has(co, "colSpan") && co["colSpan"].as<double>() != 1) ||
                (has(co, "rowSpan") && co["rowSpan"].as<double>() != 1))
                spans = true;
        }
        if (spans) {
            // Row-major occupancy, spans clamped to the grid (solve.ts). Grids
            // beyond the solver's 64 slots are left to the firmware stage.
            if (total <= GRID_SLOTS_MAX) {
                uint64_t occ = 0;
                long long slot = 0;
                bool overflow = false;
                for (JsonVariantConst cell : cells) {
                    while (slot < total && (occ >> slot & 1))
                        ++slot;
                    if (slot >= total) {
                        overflow = true;
                        break;
                    }
                    long long r = slot / cols, col = slot % cols;
                    long long cs = is_num(cell["colSpan"]) ? cell["colSpan"].as<long long>() : 1;
                    long long rs = is_num(cell["rowSpan"]) ? cell["rowSpan"].as<long long>() : 1;
                    if (cs > cols - col) cs = cols - col;
                    if (rs > rows - r) rs = rows - r;
                    for (long long dr = 0; dr < rs; ++dr)
                        for (long long dc = 0; dc < cs; ++dc)
                            occ |= 1ULL << ((r + dr) * cols + col + dc);
                }
                if (overflow)
                    return fail_at(c, "cells",
                                   "grid has more cells than can fit in %lld * %lld = %lld slots "
                                   "after accounting for spans",
                                   rows, cols, total);
            }
        } else if ((long long)cells.size() != total) {
            return fail_at(c, "cells",
                           "grid cells length must equal rows * cols (%lld * %lld = %lld); got %u "
                           "cells",
                           rows, cols, total, (unsigned)cells.size());
        }
        size_t s = push(c, "cells");
        size_t i = 0;
        bool ok = true;
        for (JsonVariantConst k : cells) {
            if (!ok) break;
            size_t t = push(c, i++);
            ok = sem_node(c, k);
            pop(c, t);
        }
        pop(c, s);
        return ok;
    }
    return true;  // spacer
}

// checkRefs: every leaf / preset slot names an element of this screen.
bool sem_refs(Ctx &c, JsonVariantConst n, JsonObjectConst elements) {
    JsonObjectConst o = n.as<JsonObjectConst>();
    if (has(o, "element")) {
        if (!has(elements, str(o["element"])))
            return fail_at(c, "element", "layout.element \"%s\" does not exist in screen.elements",
                           str(o["element"]));
        return true;
    }
    if (has(o, "preset")) {
        size_t s = push(c, "slots");
        size_t i = 0;
        bool ok = true;
        for (JsonVariantConst sl : o["slots"].as<JsonArrayConst>()) {
            if (!ok) break;
            size_t t = push(c, i++);
            if (!has(elements, str(sl)))
                ok = fail_at(c, "element",
                             "layout.element \"%s\" does not exist in screen.elements", str(sl));
            pop(c, t);
        }
        pop(c, s);
        return ok;
    }
    const char *key = has(o, "children") ? "children" : has(o, "cells") ? "cells" : nullptr;
    if (!key) return true;
    size_t s = push(c, key);
    size_t i = 0;
    bool ok = true;
    for (JsonVariantConst k : o[key].as<JsonArrayConst>()) {
        if (!ok) break;
        size_t t = push(c, i++);
        ok = sem_refs(c, k, elements);
        pop(c, t);
    }
    pop(c, s);
    return ok;
}

bool sem_layout(Ctx &c, JsonVariantConst layout, JsonObjectConst elements) {
    return sem_node(c, layout) && sem_refs(c, layout, elements);
}

bool semantic(Ctx &c, JsonVariantConst doc) {
    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    size_t si = 0;
    for (JsonVariantConst screen : screens) {
        size_t s = push(c, "screens");
        size_t t = push(c, si);
        bool ok = true;
        // Duplicate screen id (first occurrence wins).
        size_t pi = 0;
        for (JsonVariantConst prev : screens) {
            if (pi >= si) break;
            if (!strcmp(str(prev["id"]), str(screen["id"]))) {
                ok = fail_at(c, "id", "duplicate screen.id \"%s\" (already used by screens/%u)",
                             str(screen["id"]), (unsigned)pi);
                break;
            }
            ++pi;
        }
        JsonObjectConst elements = screen["elements"].as<JsonObjectConst>();
        if (ok) {
            size_t e = push(c, "elements");
            for (JsonPairConst kv : elements) {
                if (!ok) break;
                size_t f = push(c, kv.key().c_str());
                ok = sem_element(c, kv.key().c_str(), kv.value().as<JsonObjectConst>());
                pop(c, f);
            }
            pop(c, e);
        }
        if (ok) {
            size_t l = push(c, "layout");
            ok = sem_layout(c, screen["layout"], elements);
            pop(c, l);
        }
        if (ok) {
            size_t v = push(c, "variants");
            size_t vi = 0;
            for (JsonVariantConst variant : screen["variants"].as<JsonArrayConst>()) {
                if (!ok) break;
                size_t w = push(c, vi++);
                if (!non_empty_trimmed(variant["class"]))
                    ok = fail_at(c, "class", "variant must declare a non-empty class");
                if (ok) {
                    size_t l = push(c, "layout");
                    ok = sem_layout(c, variant["layout"], elements);
                    pop(c, l);
                }
                pop(c, w);
            }
            pop(c, v);
        }
        pop(c, t);
        pop(c, s);
        if (!ok) return false;
        ++si;
    }
    JsonArrayConst alarms = doc["alarms"].as<JsonArrayConst>();
    size_t ai = 0;
    for (JsonVariantConst alarm : alarms) {
        size_t pi = 0;
        for (JsonVariantConst prev : alarms) {
            if (pi >= ai) break;
            if (!strcmp(str(prev["id"]), str(alarm["id"]))) {
                size_t s = push(c, "alarms");
                size_t t = push(c, ai);
                fail_at(c, "id", "duplicate alarm.id \"%s\" (already used by alarms/%u)",
                        str(alarm["id"]), (unsigned)pi);
                pop(c, t);
                pop(c, s);
                return false;
            }
            ++pi;
        }
        ++ai;
    }
    return true;
}

// =============================================================================
// Stage 4: capability satisfaction (midl/ts/src/satisfy.ts + presets.ts)
// =============================================================================

// Walk the preset-EXPANDED tree without materialising it. Fails (false) if a
// preset is unknown or has the wrong slot count (expand() throws in TS).
struct Shape {
    int tiles;
    int depth;
};

bool expanded_shape(JsonVariantConst n, Shape &out, const char *&err) {
    JsonObjectConst o = n.as<JsonObjectConst>();
    if (has(o, "preset")) {
        const char *p = str(o["preset"]);
        size_t slots = o["slots"].size();
        size_t want = !strcmp(p, "full") ? 1 : !strcmp(p, "hero-split") ? 3 : 0;
        if (!want) {
            err = "unknown preset";
            return false;
        }
        if (slots != want) {
            err = want == 1 ? "preset expects 1 slots" : "preset expects 3 slots";
            return false;
        }
        out = want == 1 ? Shape{1, 1} : Shape{3, 3};
        return true;
    }
    if (has(o, "element")) {
        out = Shape{1, 1};
        return true;
    }
    const char *key = has(o, "children") ? "children" : has(o, "cells") ? "cells" : nullptr;
    out = Shape{0, 1};  // spacer
    if (!key) return true;
    int max_child = 0;
    for (JsonVariantConst k : o[key].as<JsonArrayConst>()) {
        Shape s;
        if (!expanded_shape(k, s, err)) return false;
        out.tiles += s.tiles;
        if (s.depth > max_child) max_child = s.depth;
    }
    if (o[key].size()) out.depth = 1 + max_child;
    return true;
}

struct Caps {
    JsonVariantConst manifest;
    JsonObjectConst cls;
    JsonArrayConst types;  // class elements (or null -> manifest.elements[].type)
    JsonArrayConst sources;
    bool sources_default;  // manifest.sources absent -> ["signalk"]
    JsonArrayConst actions;
};

bool type_allowed(const Caps &k, const char *type) {
    if (!k.types.isNull()) return in_list(k.types, type);
    for (JsonVariantConst e : k.manifest["elements"].as<JsonArrayConst>())
        if (!strcmp(str(e["type"]), type)) return true;
    return false;
}

JsonVariantConst cap_for(const Caps &k, const char *type) {
    for (JsonVariantConst e : k.manifest["elements"].as<JsonArrayConst>())
        if (!strcmp(str(e["type"]), type)) return e;
    return JsonVariantConst();
}

bool cap_element(Ctx &c, const Caps &k, JsonObjectConst el) {
    const char *type = str(el["type"]);
    if (!type_allowed(k, type)) return fail_at(c, "type", "element type not supported: %s", type);
    JsonVariantConst cap = cap_for(k, type);
    size_t s = push(c, "bindings");
    bool ok = true;
    for (JsonPairConst kv : el["bindings"].as<JsonObjectConst>()) {
        const char *field = kv.key().c_str();
        const char *kind = str(kv.value()["kind"]);
        if (is_arr(cap["bindings"]) && !in_list(cap["bindings"].as<JsonArrayConst>(), field))
            ok = fail_at(c, field, "binding field not supported by %s: %s", type, field);
        else if (k.sources_default ? strcmp(kind, "signalk") != 0 : !in_list(k.sources, kind))
            ok = fail_at(c, field, "source kind not supported: %s", kind);
        if (!ok) break;
    }
    pop(c, s);
    if (!ok) return false;
    if (is_obj(el["action"]) && k.actions.size() > 0 &&
        !in_list(k.actions, str(el["action"]["kind"])))
        return fail_at(c, "action", "action kind not supported: %s", str(el["action"]["kind"]));
    return true;
}

// Element ids in expanded-tree order, checked one by one.
bool cap_ids(Ctx &c, const Caps &k, JsonVariantConst n, JsonObjectConst elements) {
    JsonObjectConst o = n.as<JsonObjectConst>();
    auto check_id = [&](const char *id) {
        if (!has(elements, id)) return fail(c, "layout references unknown element: %s", id);
        size_t s = push(c, "elements");
        size_t t = push(c, id);
        bool ok = cap_element(c, k, elements[id].as<JsonObjectConst>());
        pop(c, t);
        pop(c, s);
        return ok;
    };
    if (has(o, "element")) return check_id(str(o["element"]));
    if (has(o, "preset")) {
        for (JsonVariantConst sl : o["slots"].as<JsonArrayConst>())
            if (!check_id(str(sl))) return false;
        return true;
    }
    const char *key = has(o, "children") ? "children" : has(o, "cells") ? "cells" : nullptr;
    if (!key) return true;
    for (JsonVariantConst kid : o[key].as<JsonArrayConst>())
        if (!cap_ids(c, k, kid, elements)) return false;
    return true;
}

bool capability(Ctx &c, JsonVariantConst doc, JsonVariantConst manifest, const char *class_id) {
    Caps k;
    k.manifest = manifest;
    for (JsonVariantConst cl : manifest["classes"].as<JsonArrayConst>())
        if (!strcmp(str(cl["id"]), class_id)) k.cls = cl.as<JsonObjectConst>();
    if (k.cls.isNull()) return fail(c, "class not supported: %s", class_id);
    k.types = k.cls["elements"].as<JsonArrayConst>();
    k.sources_default = manifest["sources"].isNull();
    k.sources = manifest["sources"].as<JsonArrayConst>();
    k.actions = manifest["actionKinds"].as<JsonArrayConst>();
    long long max_tiles = k.cls["maxTiles"] | 0LL;
    long long max_depth = k.cls["maxDepth"] | 0LL;

    size_t si = 0;
    for (JsonVariantConst screen : doc["screens"].as<JsonArrayConst>()) {
        size_t s = push(c, "screens");
        size_t t = push(c, si++);
        JsonVariantConst layout = screen["layout"];
        for (JsonVariantConst v : screen["variants"].as<JsonArrayConst>()) {
            if (!strcmp(str(v["class"]), class_id)) {
                layout = v["layout"];
                break;
            }
        }
        Shape shape;
        const char *err = "";
        bool ok = true;
        if (!expanded_shape(layout, shape, err)) {
            ok = fail_at(c, "layout", "%s", err);
        } else {
            if (shape.tiles > max_tiles)
                ok = fail_at(c, "layout", "too many tiles: %d > %lld", shape.tiles, max_tiles);
            else if (shape.depth > max_depth)
                ok = fail_at(c, "layout", "nesting too deep: %d > %lld", shape.depth, max_depth);
            else
                ok = cap_ids(c, k, layout, screen["elements"].as<JsonObjectConst>());
        }
        pop(c, t);
        pop(c, s);
        if (!ok) return false;
    }
    return true;
}

// =============================================================================
// Stage 5: firmware storage limits
// =============================================================================

bool len_ok(Ctx &c, JsonVariantConst v, const char *seg, size_t max) {
    if (is_str(v) && utf8_len_over(v.as<const char *>(), max))
        return fail_at(c, seg, "%s is longer than %u bytes (the device stores at most %u)", seg,
                       (unsigned)max, (unsigned)max);
    return true;
}

bool fw_source(Ctx &c, JsonVariantConst src) {
    if (!is_obj(src)) return true;
    const char *kind = str(src["kind"]);
    if (!midl::kinds::source_supported(kind))
        return fail(c, "unsupported source kind '%s' (this device does not implement it)", kind);
    return len_ok(c, src["path"], "path", PATH_MAX_LEN) &&
           len_ok(c, src["id"], "id", PATH_MAX_LEN) &&
           len_ok(c, src["value"], "value", PATH_MAX_LEN);
}

bool fw_grid(Ctx &c, JsonVariantConst n) {
    JsonObjectConst o = n.as<JsonObjectConst>();
    if (has(o, "cells") && o["rows"].as<double>() * o["cols"].as<double>() > GRID_SLOTS_MAX)
        return false;
    const char *key = has(o, "children") ? "children" : has(o, "cells") ? "cells" : nullptr;
    if (!key) return true;
    for (JsonVariantConst k : o[key].as<JsonArrayConst>())
        if (!fw_grid(c, k)) return false;
    return true;
}

bool fw_element(Ctx &c, const char *id, JsonObjectConst el) {
    if (utf8_len_over(id, STR_MAX))
        return fail(c, "element id is longer than %u bytes", (unsigned)STR_MAX);
    if (!len_ok(c, el["name"], "name", STR_MAX) || !len_ok(c, el["zoom"], "zoom", STR_MAX))
        return false;
    {
        size_t s = push(c, "format");
        bool ok = len_ok(c, el["format"]["unit"], "unit", STR_MAX);
        pop(c, s);
        if (!ok) return false;
    }
    {
        size_t s = push(c, "bindings");
        bool ok = true;
        for (JsonPairConst kv : el["bindings"].as<JsonObjectConst>()) {
            size_t t = push(c, kv.key().c_str());
            ok = fw_source(c, kv.value());
            pop(c, t);
            if (!ok) break;
        }
        pop(c, s);
        if (!ok) return false;
    }
    {
        size_t s = push(c, "markers");
        size_t i = 0;
        bool ok = true;
        for (JsonVariantConst m : el["markers"].as<JsonArrayConst>()) {
            size_t t = push(c, i++);
            size_t u = push(c, "dir");
            ok = fw_source(c, m["dir"]);
            pop(c, u);
            pop(c, t);
            if (!ok) break;
        }
        pop(c, s);
        if (!ok) return false;
    }
    JsonVariantConst act = el["action"];
    if (!is_obj(act)) return true;
    size_t s = push(c, "action");
    bool ok = true;
    const char *kind = str(act["kind"]);
    if (!midl::kinds::action_supported(kind))
        ok = fail(c, "unsupported action kind '%s' (this device does not implement it)", kind);
    ok = ok && len_ok(c, act["target"], "target", PATH_MAX_LEN);
    // A PUT string payload is sent as "\"%s\"" from a 32-byte buffer: longer
    // strings would be cut, quotes/backslashes/control chars would corrupt it.
    if (ok && !strcmp(kind, "put") && is_str(act["value"])) {
        const char *v = act["value"].as<const char *>();
        if (utf8_len_over(v, PUT_STRING_MAX))
            ok = fail_at(c, "value", "put string value is longer than %u bytes",
                         (unsigned)PUT_STRING_MAX);
        for (const char *p = v; ok && *p; ++p)
            if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20)
                ok = fail_at(c, "value",
                             "put string value must not contain quotes, backslashes or "
                             "control characters");
    }
    pop(c, s);
    return ok;
}

bool firmware(Ctx &c, JsonVariantConst doc) {
    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    if (screens.size() > MAX_SCREENS) {
        size_t s = push(c, "screens");
        size_t t = push(c, MAX_SCREENS);
        fail(c, "too many screens: %u > %u (this device holds at most %u)",
             (unsigned)screens.size(), (unsigned)MAX_SCREENS, (unsigned)MAX_SCREENS);
        pop(c, t);
        pop(c, s);
        return false;
    }
    size_t si = 0;
    for (JsonVariantConst screen : screens) {
        size_t s = push(c, "screens");
        size_t t = push(c, si++);
        bool ok = len_ok(c, screen["id"], "id", STR_MAX);
        if (ok && !fw_grid(c, screen["layout"]))
            ok = fail_at(c, "layout", "grid has more than %d slots (rows * cols)", GRID_SLOTS_MAX);
        size_t vi = 0;
        for (JsonVariantConst v : screen["variants"].as<JsonArrayConst>()) {
            if (!ok) break;
            if (!fw_grid(c, v["layout"])) {
                size_t u = push(c, "variants");
                size_t w = push(c, vi);
                ok = fail_at(c, "layout", "grid has more than %d slots (rows * cols)",
                             GRID_SLOTS_MAX);
                pop(c, w);
                pop(c, u);
            }
            ++vi;
        }
        if (ok) {
            size_t e = push(c, "elements");
            for (JsonPairConst kv : screen["elements"].as<JsonObjectConst>()) {
                size_t f = push(c, kv.key().c_str());
                ok = fw_element(c, kv.key().c_str(), kv.value().as<JsonObjectConst>());
                pop(c, f);
                if (!ok) break;
            }
            pop(c, e);
        }
        pop(c, t);
        pop(c, s);
        if (!ok) return false;
    }
    size_t ai = 0;
    for (JsonVariantConst alarm : doc["alarms"].as<JsonArrayConst>()) {
        size_t s = push(c, "alarms");
        size_t t = push(c, ai++);
        size_t u = push(c, "source");
        bool ok = fw_source(c, alarm["source"]);
        pop(c, u);
        pop(c, t);
        pop(c, s);
        if (!ok) return false;
    }
    return true;
}

}  // namespace

bool validate_document(JsonVariantConst doc, JsonVariantConst manifest, const char *class_id,
                       Issue &out) {
    Ctx c;
    reset(c, out);
    out.path[0] = out.message[0] = 0;
    if (!structural(c, doc)) return false;
    if (!semantic(c, doc)) return false;
    unsigned long long dv[3], mv[3];
    parse_version(str(doc["midl"]), dv);
    if (!parse_version(str(manifest["midl"]), mv))
        return fail_at(c, "manifest", "device manifest has no valid midl version");
    if (dv[0] != mv[0] || dv[1] > mv[1])
        return fail_at(c, "midl", "incompatible MIDL %s vs device %s", str(doc["midl"]),
                       str(manifest["midl"]));
    return capability(c, doc, manifest, class_id ? class_id : "");
}

bool check_firmware_limits(JsonVariantConst doc, Issue &out) {
    Ctx c;
    reset(c, out);
    out.path[0] = out.message[0] = 0;
    return firmware(c, doc);
}

bool admit(JsonVariantConst doc, JsonVariantConst manifest, const char *class_id, Issue &out) {
    return validate_document(doc, manifest, class_id, out) && check_firmware_limits(doc, out);
}

}  // namespace midl::validate
