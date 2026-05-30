#pragma once

// Latency benchmarks for the rendering and event pipelines.
//
// Tracked channels (per docs/specs/09 profiling plan):
//
//   FrameInterval : disp_flush_cb -> disp_flush_cb time in us
//                   (effective frame pacing; jitter shows render stalls)
//   RenderLatency : last invalidate -> first flush after it, in us
//                   (how long a dirty-mark takes to reach the panel)
//   CommandRtt    : app::post -> pump drain, in us
//                   (touch / web / BLE event -> UI task handles it)
//   SkAge         : sk.lastUpdateMs -> next ui_refresh observed it
//                   (incoming SK delta -> screen reads it)
//
// Each channel keeps count, sum, min, max. record() is lock-free and
// ISR-safe (32-bit atomic-ish writes on ESP32; we don't need perfect
// consistency for diagnostics).

#include <stdint.h>

namespace latency {

enum class Channel : uint8_t { FrameInterval, RenderLatency, CommandRtt, SkAge, COUNT };

struct Stats {
    uint32_t count = 0;
    uint64_t sum_us = 0;
    uint32_t min_us = 0xFFFFFFFF;
    uint32_t max_us = 0;
};

void record(Channel c, uint32_t dt_us);
Stats snapshot(Channel c);
void reset_all();

const char *channel_name(Channel c);

}  // namespace latency
