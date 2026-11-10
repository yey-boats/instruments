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

// A `put` target must be a SignalK path (dotted, no spaces) so it can never
// fall through to the console-command funnel.
bool put_target_ok(const char *target);

// Walk a MIDL document (element bindings, marker dirs, element actions, alarm
// sources). Returns false on the first unsupported kind and writes a clear,
// JSON-pointer-located message into err, e.g.
//   unsupported source kind 'computed' at /screens/0/elements/vmg/bindings/value
//   (this device supports: signalk, local, const)
// A binding with no `kind` but a `path` is the legacy SignalK form: accepted.
bool check_doc(JsonVariantConst doc, char *err, size_t err_len);

}  // namespace midl::kinds
