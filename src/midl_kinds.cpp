// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "midl_kinds.h"

#include <stdio.h>
#include <string.h>

namespace midl::kinds {

static bool in_list(const char *const *list, const char *kind) {
    if (!kind || !kind[0]) return false;
    for (size_t i = 0; list[i]; ++i) {
        if (strcmp(list[i], kind) == 0) return true;
    }
    return false;
}

bool source_supported(const char *kind) {
    return in_list(SOURCES, kind);
}

bool action_supported(const char *kind) {
    return in_list(ACTION_KINDS, kind);
}

static bool seg_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

bool put_target_ok(const char *target) {
    if (!target) return false;
    size_t len = strnlen(target, ACTION_TARGET_MAX + 1);
    if (len == 0 || len > ACTION_TARGET_MAX) return false;
    size_t segs = 0, seg_len = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = target[i];
        if (c == '.') {
            if (seg_len == 0) return false;  // leading or doubled dot
            ++segs;
            seg_len = 0;
        } else if (seg_char(c)) {
            ++seg_len;
        } else {
            return false;
        }
    }
    if (seg_len == 0) return false;  // trailing dot
    return segs + 1 >= 2;
}

bool encode_put_path(const char *path, char *out, size_t n) {
    if (!out || n == 0) return false;
    out[0] = 0;
    if (!path) return false;
    static const char kHexDigits[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        unsigned char c = *p;
        bool keep = seg_char((char)c) || c == '.' || c == '~';
        size_t need = keep ? 1 : 3;
        if (o + need >= n) {
            out[0] = 0;
            return false;
        }
        if (keep) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = kHexDigits[c >> 4];
            out[o++] = kHexDigits[c & 0xF];
        }
    }
    out[o] = 0;
    return true;
}

ButtonRoute button_route(const char *target) {
    if (!target || !target[0]) return ButtonRoute::None;
    return put_target_ok(target) ? ButtonRoute::SignalKPut : ButtonRoute::Console;
}

static void join(const char *const *list, char *out, size_t n) {
    out[0] = 0;
    size_t used = 0;
    for (size_t i = 0; list[i] && used < n; ++i) {
        int w = snprintf(out + used, n - used, "%s%s", i ? ", " : "", list[i]);
        if (w < 0) break;
        used += (size_t)w;
    }
}

static bool check_source(JsonVariantConst src, const char *path, char *err, size_t n) {
    if (!src.is<JsonObjectConst>()) return true;  // structure is the schema's job
    JsonVariantConst k = src["kind"];
    if (k.isNull() && src["path"].is<const char *>()) return true;  // legacy signalk
    const char *kind = k | "";
    if (source_supported(kind)) return true;
    char supported[64];
    join(SOURCES, supported, sizeof(supported));
    snprintf(err, n, "unsupported source kind '%s' at %s (this device supports: %s)", kind, path,
             supported);
    return false;
}

static bool check_action(JsonVariantConst act, const char *path, char *err, size_t n) {
    if (!act.is<JsonObjectConst>()) return true;
    const char *kind = act["kind"] | "";
    if (!action_supported(kind)) {
        char supported[64];
        join(ACTION_KINDS, supported, sizeof(supported));
        snprintf(err, n, "unsupported action kind '%s' at %s (this device supports: %s)", kind,
                 path, supported);
        return false;
    }
    const char *target = act["target"] | "";
    // Length first, on the FULL string: the renderer stores at most
    // ACTION_TARGET_MAX chars and must never truncate a put/command target.
    if ((strcmp(kind, "put") == 0 || strcmp(kind, "command") == 0) &&
        strlen(target) > ACTION_TARGET_MAX) {
        snprintf(err, n, "action target at %s is longer than %u chars (got %u)", path,
                 (unsigned)ACTION_TARGET_MAX, (unsigned)strlen(target));
        return false;
    }
    if (strcmp(kind, "put") == 0 && !put_target_ok(target)) {
        snprintf(err, n,
                 "action kind 'put' at %s needs a SignalK path target "
                 "(dot-separated [A-Za-z0-9_-] segments, got '%.40s')",
                 path, target);
        return false;
    }
    return true;
}

bool check_doc(JsonVariantConst doc, char *err, size_t n) {
    if (n) err[0] = 0;
    char path[128];
    size_t si = 0;
    for (JsonVariantConst screen : doc["screens"].as<JsonArrayConst>()) {
        for (JsonPairConst kv : screen["elements"].as<JsonObjectConst>()) {
            const char *id = kv.key().c_str();
            JsonVariantConst el = kv.value();
            for (JsonPairConst b : el["bindings"].as<JsonObjectConst>()) {
                snprintf(path, sizeof(path), "/screens/%u/elements/%s/bindings/%s", (unsigned)si,
                         id, b.key().c_str());
                if (!check_source(b.value(), path, err, n)) return false;
            }
            size_t mi = 0;
            for (JsonVariantConst mk : el["markers"].as<JsonArrayConst>()) {
                if (!mk["dir"].isNull()) {
                    snprintf(path, sizeof(path), "/screens/%u/elements/%s/markers/%u/dir",
                             (unsigned)si, id, (unsigned)mi);
                    if (!check_source(mk["dir"], path, err, n)) return false;
                }
                ++mi;
            }
            if (!el["action"].isNull()) {
                snprintf(path, sizeof(path), "/screens/%u/elements/%s/action", (unsigned)si, id);
                if (!check_action(el["action"], path, err, n)) return false;
            }
        }
        ++si;
    }
    size_t ai = 0;
    for (JsonVariantConst alarm : doc["alarms"].as<JsonArrayConst>()) {
        if (!alarm["source"].isNull()) {
            snprintf(path, sizeof(path), "/alarms/%u/source", (unsigned)ai);
            if (!check_source(alarm["source"], path, err, n)) return false;
        }
        ++ai;
    }
    return true;
}

}  // namespace midl::kinds
