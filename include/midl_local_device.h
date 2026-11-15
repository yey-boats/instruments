#pragma once

// Device half of the MIDL local ids ({kind:"local", id:"..."} bindings): reads
// the live device status the pure formatter (midl_local_ids.h) renders. Shared
// by the MIDL painters (UI task, midl_render_apply.cpp) and the editor's
// GET /api/midl/live (web task, web.cpp) so both show the same values. No LVGL.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <stddef.h>
#include <stdint.h>

namespace midl::local_dev {

// Numeric local id in DISPLAY units (temperature degC, heap/psram KB, rssi
// dBm, uptime s, trip.distance nm, trip.avgSpeed / trip.maxSpeed kn),
// accepted with or without a "device." prefix. Unknown ids -> NaN.
double numeric(const char *id);

// Device strings for the text ids, refreshed at most once a second (the
// String-returning status APIs would churn the internal heap at the painters'
// 5 Hz). One cache per calling task: it is not locked.
struct TextCache {
    char ip[16];
    char ssid[33];
    char sk_state[20];
    char sk_host[72];
    uint32_t last_ms;
    bool primed;
};

// Text id (midl::local_ids::is_text_id) formatted into `out`; false (and `out`
// untouched) for a non-text id.
bool text(const char *id, char *out, size_t cap, TextCache &cache);

}  // namespace midl::local_dev
