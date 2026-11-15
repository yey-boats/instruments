// Trip log, device half (NVS + boat data). See include/trip_log.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "trip_log.h"

#include <Arduino.h>

#include "net.h"
#include "signalk.h"
#include "storage.h"

namespace trip_log {

static Accum s_acc = {};
static uint32_t s_started_at_ms = 0;

static void save() {
    storage::Namespace p("trip", false);
    p.put_double("dist", s_acc.dist_m);
    p.put_u32("under", s_acc.underway_s);
    p.put_double("maxsog", s_acc.max_sog);
    p.put_u32("start", s_started_at_ms);
}

void setup() {
    storage::Namespace p("trip", true);
    s_acc.dist_m = p.get_double("dist", 0);
    s_acc.underway_s = p.get_u32("under", 0);
    s_acc.max_sog = p.get_double("maxsog", 0);
    s_acc.last_ms = 0;
    s_started_at_ms = p.get_u32("start", 0);
    if (s_started_at_ms == 0) s_started_at_ms = millis();
}

void tick() {
    // Cheap: boat::current_view composes every field, so read SOG only once a
    // second (sample() ignores faster calls anyway).
    uint32_t now = millis();
    if (s_acc.last_ms != 0 && now - s_acc.last_ms < 1000) return;
    boat::View d;
    boat::current_view(d);
    if (sample(s_acc, d.sog, now)) save();
}

void reset() {
    s_acc = Accum{};
    s_started_at_ms = millis();
    save();
    net::logf("[trip] reset");
}

const Accum &current() {
    return s_acc;
}

uint32_t started_at_ms() {
    return s_started_at_ms;
}

}  // namespace trip_log
