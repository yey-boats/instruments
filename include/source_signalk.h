#pragma once

// SignalK -> boat::Snapshot bridge per docs/specs/12 Feature 1.
//
// Each time the SignalK WS task ingests a delta, it calls
// bridge_signalk_into_boat(sk::data, millis()). The bridge publishes
// every present (non-NaN) sk field into the global boat::Snapshot with
// SourceKind::SignalK, honoring the configured priority.
//
// Pure declaration here; implementation in src/source_signalk.cpp keeps
// the dependency on sk::Data and boat::Snapshot localized.

#include "boat_data.h"
#include "signalk_parser.h"

namespace boat {

// Republish all fields from `sk` into the global Snapshot at time
// `now_ms`. Returns the number of fields actually accepted (i.e.
// SignalK was the winning source for them at this tick).
int bridge_signalk_into_boat(const sk::Data &sk, uint32_t now_ms);

// Reverse projection: fill an sk::Data from the current boat::Snapshot
// so legacy screens reading sk::data still see the fused value chosen
// by the source-priority resolver. Stale fields collapse to NaN so
// renderers can fall back to "no data" rendering.
//
// `connected` and `lastUpdateMs` come from the sk WS state and are not
// overwritten here - callers preserve them.
void compose_from_boat(sk::Data &out, uint32_t now_ms);

}  // namespace boat
