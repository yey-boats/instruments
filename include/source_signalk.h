#pragma once

// SignalK -> boat::Snapshot ingest + boat::Snapshot -> View projection
// per docs/specs/12 Feature 1.
//
// Each time the SignalK WS task parses a delta into a transient boat::View,
// it calls ingest_signalk(view, millis()), which publishes every present
// (non-NaN) field into the global boat::Snapshot with SourceKind::SignalK,
// honoring the configured priority. This is the sole SignalK ingest path
// (there is no persistent sk::Data source-of-truth anymore).
//
// Pure declaration here; implementation in src/source_signalk.cpp.

#include "boat_data.h"

namespace boat {

// Republish all metric fields from `v` into the global Snapshot at time
// `now_ms`. Returns the number of fields actually accepted (i.e. SignalK
// was the winning source for them at this tick). Link-state fields on `v`
// are ignored here.
int ingest_signalk(const View &v, uint32_t now_ms);

// Projection: fill the metric fields of `out` from the current fused
// boat::Snapshot so renderers see the value chosen by the source-priority
// resolver. Stale fields collapse to NaN so renderers fall back to "no
// data". The WS link-state fields (connected/lastUpdateMs/...) are NOT
// touched here - the SignalK layer fills those (see boat::current_view).
void compose(View &out, uint32_t now_ms);

}  // namespace boat
