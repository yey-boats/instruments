#pragma once
// Controller-side, on-demand BLE central for the espdisp control protocol.
// This is the FALLBACK transport, used only when a peer has no reachable IP
// (design §4.2 / §4.3). The model is strictly on-demand:
//
//     scan -> connect ONE peer -> attach/switch/detach -> disconnect -> delete
//
// An idle outbound connection is NEVER held. NimBLE on the ESP32-S3 keeps the
// controller's connection state in internal SRAM; holding connections open (or
// running many at once) is the documented starvation risk. By scanning briefly,
// connecting to exactly one peer, doing the control exchange, and tearing the
// client down (disconnect + NimBLEDevice::deleteClient) on EVERY exit path, the
// footprint stays bounded.
//
// All entry points BLOCK (scan window, connect, GATT round-trips) and so MUST
// run on the worker/network task, never on the LVGL UI task. The UI task only
// sets a pending request (see knob_remote::switch_view), which the worker drains.
#include <Arduino.h>

#include "proto/proto.h"

// The central role is only compiled for controller boards (the knob) and the
// headless harness. Display-only boards stay peripheral-only: proto_ble.cpp is
// still in their build_src_filter but the body is #ifdef'd out so nothing here
// can call NimBLE's central API on a display.
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8) || defined(YEYBOATS_HARNESS)
#define YEYBOATS_BLE_CENTRAL 1
#endif

namespace proto_ble {

// One BLE peer advertising the Control service. `addr` is the 6-byte BLE
// address string ("aa:bb:cc:dd:ee:ff") used to reconnect; `device_id` comes
// from the advertised name; `pv` (protocol version) is parsed from the scan
// response manufacturer data when present (empty otherwise).
struct Peer {
    char device_id[40] = {0};
    char addr[24] = {0};  // NimBLEAddress::toString()
    char pv[8] = {0};
};

// Callback invoked once per discovered Control-service peer, on the worker task.
typedef void (*PeerCallback)(const Peer &peer, void *ctx);

// Synchronously scan for `timeout_ms` (active scan) and invoke `cb` for every
// advertiser carrying the Control service UUID. Results are cleared afterwards
// so NimBLE's scan cache does not accumulate. Returns the number of Control-
// service peers reported, or -1 if scanning is unavailable on this build.
// BLOCKING — worker task only.
int scan(uint32_t timeout_ms, PeerCallback cb, void *ctx);

// On-demand single-peer view switch. Connects to `addr`, writes the Attach,
// reads the AttachAck back (for the sessionId), writes the Switch, reads the
// SwitchAck, writes a Detach, then ALWAYS disconnects + deletes the client.
// `sw_template` carries the target viewId (+ v); sessionId is filled from the
// ack. Returns true iff the SwitchAck reported ok. One connection at a time
// (serialized by a worker-side busy guard). BLOCKING — worker task only.
bool switch_on_peer(const char *addr, const proto::Attach &a, const proto::Switch &sw_template);

// Connect to `addr`, read the DEVICE characteristic into `out` (the peer's
// identity + view list), then ALWAYS disconnect + delete the client. Used to
// enumerate a BLE-only peer's views on drill-in. Returns true on a parseable
// DeviceRecord. BLOCKING — worker task only.
bool get_device_on_peer(const char *addr, proto::DeviceRecord &out);

}  // namespace proto_ble
