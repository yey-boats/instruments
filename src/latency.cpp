#include "latency.h"

namespace latency {

static Stats s_stats[(int)Channel::COUNT];

void record(Channel c, uint32_t dt_us) {
    int i = (int)c;
    if (i < 0 || i >= (int)Channel::COUNT) return;
    Stats &s = s_stats[i];
    s.count++;
    s.sum_us += dt_us;
    if (dt_us < s.min_us) s.min_us = dt_us;
    if (dt_us > s.max_us) s.max_us = dt_us;
}

Stats snapshot(Channel c) {
    int i = (int)c;
    if (i < 0 || i >= (int)Channel::COUNT) return Stats{};
    return s_stats[i];
}

void reset_all() {
    for (int i = 0; i < (int)Channel::COUNT; ++i) {
        s_stats[i] = Stats{};
    }
}

const char *channel_name(Channel c) {
    switch (c) {
    case Channel::FrameInterval:
        return "frame_interval";
    case Channel::RenderLatency:
        return "render_latency";
    case Channel::CommandRtt:
        return "command_rtt";
    case Channel::SkAge:
        return "sk_age";
    default:
        return "?";
    }
}

}  // namespace latency
