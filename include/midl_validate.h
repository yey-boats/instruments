#pragma once

// instruments#20: on-device MIDL validation.
//
// POST /api/midl/config used to accept any document with a `midl` marker and
// one screen. admit() is the full gate: it runs the SAME pipeline as midl's
// TypeScript/Python validateDocument (midl/ts/src/index.ts), against the
// device's embedded capability manifest, then this firmware's storage limits:
//
//   1. structural  - the yb-midl-config JSON Schema (shapes, types, enums,
//                    required fields, layout-node oneOf, numeric minimums)
//   2. semantic    - element refs resolve, unique screen/alarm ids, weights /
//                    grid arithmetic, known presets, required bindings,
//                    non-empty sources, put targets, action target length,
//                    style.range ordering
//   3. version     - same major, doc minor <= device minor
//   4. capability  - class tiles-per-screen / depth, element types, binding
//                    fields, source and action kinds advertised by the manifest
//   5. firmware    - what the renderer stores in fixed buffers: screen count,
//                    id/name/unit lengths, path lengths, PUT payload length,
//                    grid slots. Over-long values are rejected, never truncated.
//   6. kinds       - midl::kinds::check_doc (instruments#23)
//
// Stages 1-4 must agree with midl's validators on every case of the
// conformance corpus (test/test_midl_validate runs it); stage 5 is
// firmware-only and must not reject any document the corpus calls valid.
//
// Pure C++ / ArduinoJson: host-testable, no allocation, small stack (one
// shared path buffer; recursion bounded by ArduinoJson's nesting limit).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <ArduinoJson.h>
#include <stddef.h>

namespace midl::validate {

// Firmware storage limits enforced by stage 5. Mirrors include/midl_limits.h /
// include/layout.h / src/midl_render*.cpp buffer sizes (static_assert'd there).
inline constexpr size_t MAX_SCREENS = 16;     // ui::MAX_SCREENS (MIDL screen slots)
inline constexpr size_t STR_MAX = 31;         // layout::STR_LEN - 1: ids, names, units, zoom
inline constexpr size_t PATH_MAX_LEN = 95;    // layout::PATH_LEN - 1: paths, local ids, targets
inline constexpr size_t PUT_STRING_MAX = 29;  // "\"%s\"" in a 32-byte action value buffer
inline constexpr int GRID_SLOTS_MAX = 64;     // midl_solve.cpp kMaxSlots

struct Issue {
    char path[128];     // JSON pointer, e.g. /screens/0/elements/sog/bindings/value
    char message[160];  // human-readable reason
};

// Full admission check of `doc` for display class `class_id` against
// `manifest` (the device's embedded capability manifest on target). Returns
// true when the document is admissible; otherwise fills `out` with the first
// error (its JSON pointer and message) and returns false.
bool admit(JsonVariantConst doc, JsonVariantConst manifest, const char *class_id, Issue &out);

// Stages 1-4 only: midl's validateDocument verdict.
bool validate_document(JsonVariantConst doc, JsonVariantConst manifest, const char *class_id,
                       Issue &out);

// Stage 5 only: this firmware's storage limits.
bool check_firmware_limits(JsonVariantConst doc, Issue &out);

}  // namespace midl::validate
