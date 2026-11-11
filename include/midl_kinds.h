#pragma once

// instruments#23: the MIDL source / action kinds THIS firmware implements.
//
// The MIDL schema is the language and is a superset (it also allows
// `computed` sources); per the MIDL design the per-target capability manifest
// states what a target supports. These arrays are the single source of truth:
//   - tools/embed_manifest.py parses them and writes them into the served
//     manifest's "sources" / "actionKinds" (so /api/midl/manifest is truthful
//     even where midl's generic catalog differs);
//   - check_doc() rejects documents that use any other kind.
// Keep them in step with midl_render.cpp::map_element. Format matters: the
// embed tool reads the brace lists below with a regex.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <ArduinoJson.h>
#include <stddef.h>

namespace midl::kinds {

// clang-format off
inline constexpr const char *SOURCES[] = {"signalk", "local", "const", nullptr};
inline constexpr const char *ACTION_KINDS[] = {"nav", "command", "put", nullptr};
// clang-format on

bool source_supported(const char *kind);
bool action_supported(const char *kind);

// Longest action target the renderer stores (map_element copies it into a
// midl::FirmwareLimits::path_len buffer, 96 incl. NUL — static_assert'd in
// midl_render.cpp). Longer put/command targets are REJECTED whole, never
// truncated: truncation could turn a path into a console command or PUT to a
// different path.
inline constexpr size_t ACTION_TARGET_MAX = 95;

// A `put` target must be a SignalK path so it can never fall through to the
// console-command funnel, and is safe to place in the PUT URL (which carries
// the device's bearer token): 2+ segments of [A-Za-z0-9_-], separated by
// single dots, no leading/trailing dot, at most ACTION_TARGET_MAX chars.
bool put_target_ok(const char *target);

// Percent-encode `path` for the SignalK PUT URL path: keeps unreserved chars
// [A-Za-z0-9-._~], encodes everything else as %XX — including '/', which a
// SignalK path never contains (segments are dot-separated), so a path cannot
// climb out of /signalk/v1/api/vessels/self/ (instruments#31). Returns false
// (out = "") if path is null or the result does not fit in n.
bool encode_put_path(const char *path, char *out, size_t n);

// Where a MIDL button's command slot goes on tap (instruments#31). The slot
// carries both `command` and `put` action targets; only a target that passes
// put_target_ok is a SignalK PUT (the bearer-token URL). Anything else,
// including dotted, space-free strings such as "../../x.y", stays on the
// console-command funnel (net::dispatchCommand), which `command` already
// reaches. None for a null/empty target.
enum class ButtonRoute { None, SignalKPut, Console };
ButtonRoute button_route(const char *target);

// Walk a MIDL document (element bindings, marker dirs, element actions, alarm
// sources). Returns false on the first unsupported kind and writes a clear,
// JSON-pointer-located message into err, e.g.
//   unsupported source kind 'computed' at /screens/0/elements/vmg/bindings/value
//   (this device supports: signalk, local, const)
// A binding with no `kind` but a `path` is the legacy SignalK form: accepted.
bool check_doc(JsonVariantConst doc, char *err, size_t err_len);

}  // namespace midl::kinds
