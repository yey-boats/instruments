#pragma once

// Cross-task command queue.
//
// Only the LVGL / UI owner task is allowed to mutate UI state. Web, BLE,
// and other I/O callbacks post Commands here; pump() drains them on the
// UI task. SignalK PUTs and other slow network work are routed to a
// dedicated network worker task instead (post_net()).
//
// Payloads are fixed-size for safety. The single exception is
// ApplyLayout, which carries a heap-owned blob - producer mallocs into
// PSRAM, consumer frees after handling.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace app {

enum class CommandType : uint8_t {
    None = 0,
    ShowScreen,      // a = id ("wind", "next", "prev") or numeric
    ApplyLayout,     // blob = JSON bytes; blob_len = length; consumer frees
    SetTheme,        // a = "day" or "night"
    SetBrightness,   // i = 0..255
    Reboot,
    RunCommand,      // a = console command line (legacy dispatchCommand)
    SignalKPut,      // a = SK dotted path, b = JSON value (sent to net queue)
    SaveWifi,        // a = ssid, b = password (sent to net queue, reboots)
};

struct Command {
    CommandType type = CommandType::None;
    char a[96] = {0};
    char b[256] = {0};
    int32_t i = 0;
    // Heap-owned payload for ApplyLayout. The consumer (pump or net
    // worker) is responsible for heap_caps_free(blob).
    void *blob = nullptr;
    size_t blob_len = 0;
};

void setup();

// Post to the UI queue (drained on the LVGL task via pump()).
// Returns false if the queue is full or setup() hasn't run.
bool post(const Command &cmd, uint32_t timeout_ms = 0);

// Post to the net worker queue (HTTP PUTs, WiFi save+reboot, ...).
bool post_net(const Command &cmd, uint32_t timeout_ms = 0);

// Drain the UI queue. Must only be called from the LVGL task.
void pump();

// Diagnostics for the metrics endpoint.
size_t ui_queue_depth();
size_t net_queue_depth();
uint32_t ui_high_water();
uint32_t net_high_water();

}  // namespace app
