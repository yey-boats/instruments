#pragma once

// Trend history backfill from the SignalK History API (device only).
//
// On screen show the UI task opens a new generation (begin) and queues one job
// per series (enqueue). A worker task runs the jobs one at a time — never more
// than one request in flight — against the SignalK server the display is
// connected to (same host/port and token as the WS client):
//   GET /signalk/v2/api/history/values?paths=<p>:<method>&duration=PT<w>S&resolution=<step>
// 5 s timeout, one retry on a transport error / 5xx; 404 / 501 / empty data =
// no backfill. Results are handed back through a single mailbox the UI task
// polls; a result for an older generation (screen no longer shown) is dropped.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <stdint.h>

#include "trend_history.h"

namespace trend::backfill {

struct Result {
    uint32_t gen;
    uint8_t series;    // series index in the UI's trend::Store
    int n;             // rows in age_s/vals
    double t_resp_s;   // monotonic time (now_s()) when the response arrived
    const float *age;  // seconds before t_resp_s
    const float *val;  // SI values
};

// Monotonic seconds (esp_timer), the clock the UI records history against.
double now_s();

// Start a new generation (cancels every queued job; an in-flight result of the
// old generation is ignored). Returns the generation id.
uint32_t begin();

// Queue one series' backfill. False when the queue is full or the path cannot
// be queried (const/local bindings never call this).
bool enqueue(uint32_t gen, uint8_t series, const char *path, Agg agg, uint32_t window_s,
             double step_s);

// UI task: true when a result for `gen` is ready; `out` borrows the mailbox
// buffers until done(). Stale-generation results are discarded here.
bool poll(uint32_t gen, Result &out);
void done();

}  // namespace trend::backfill
