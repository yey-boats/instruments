#pragma once

// BLE HID host: a persistent NimBLE central that bonds with a consumer-control
// remote (media buttons) and routes its keys to UI actions via app::post.
//
//   VOL+ / VOL-        -> brightness up / down (SetBrightness)
//   Next / Prev track  -> ShowScreen "next" / "prev"
//   Play-Pause / Enter -> ShowScreen "dashboard" (no dedicated select verb yet)
//
// Compiled to no-op stubs unless YEYBOATS_BLE_HID_HOST is defined (touch
// display board envs) and BLE is compiled in. All scan/connect work runs on a
// dedicated FreeRTOS task — never on the LVGL task or a GATT callback. The
// bonded peer address persists in NVS namespace "hidhost" and is reconnected
// on boot / disconnect with 5 s -> 60 s backoff.
//
// Console commands (reachable from serial AND BLE NUS via net::dispatchCommand):
//   hid-pair    scan ~15 s for a 0x1812 HID peripheral, bond with the
//               strongest match, persist it
//   hid-forget  drop the stored peer + its bond
//   hid-status  print state (peer, connection, backoff, keys handled)

#include <Arduino.h>

namespace ble_hid {

// Start the reconnect/pair worker task. Call once from bleSetup() after
// NimBLEDevice::init. No-op when the feature is compiled out.
void setup();

// hid-pair / hid-forget / hid-status. Returns true if the line was consumed.
// Only sets flags + wakes the worker; safe from any task.
bool handleSerialCommand(const String &line);

// True while an HID remote link is up (diagnostics).
bool connected();

}  // namespace ble_hid
