#pragma once

// SignalK -> boat::Snapshot ingest + boat::Snapshot -> View projection
// per docs/specs/12 Feature 1.
//
// Each time the SignalK WS task parses a delta, it calls
// ingest_signalk(view, millis(), touched) with the parser's per-delta
// FieldMask, which publishes ONLY the fields that delta actually carried
// into the global boat::Snapshot with SourceKind::SignalK, honoring the
// configured priority. This is the sole SignalK ingest path (there is no
// persistent sk::Data source-of-truth anymore).
//
// Pure declaration here; implementation in src/source_signalk.cpp.

#include "boat_data.h"

namespace boat {

// Publish the metric fields of `v` selected by `touched` (a boat::field_bit
// OR-mask produced by sk::applyDelta for the current delta) into the global
// Snapshot at time `now_ms`. Publishing only the touched fields keeps
// per-field staleness honest: a field whose sensor died is NOT re-stamped
// fresh just because other deltas keep arriving, so it times out in
// compose() and lower-priority sources can take it over. Returns the number
// of fields actually accepted (i.e. SignalK was the winning source for them
// at this tick). Link-state fields on `v` are ignored here.
int ingest_signalk(const View &v, uint32_t now_ms, FieldMask touched);

// Projection: fill the metric fields of `out` from the current fused
// boat::Snapshot so renderers see the value chosen by the source-priority
// resolver. Stale fields collapse to NaN so renderers fall back to "no
// data". The WS link-state fields (connected/lastUpdateMs/...) are NOT
// touched here - the SignalK layer fills those (see boat::current_view).
void compose(View &out, uint32_t now_ms);

}  // namespace boat
