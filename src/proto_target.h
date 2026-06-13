#pragma once
// Target-side espdisp control-protocol state: the single, mutex-guarded
// session table + shared key + current view. Drives the /api/p2p endpoints
// (web.cpp) and, later, the BLE Control GATT (Phase 3.2/4). View switches are
// applied by posting app::Command{ShowScreen} to the UI task - this module
// NEVER touches LVGL directly.
#include <Arduino.h>
#include "proto/proto.h"

namespace proto_target {

void setup();  // load the shared key from NVS (namespace "proto", key "key")

// Update the cached shared key live (after a `ctl key` console command
// rewrites NVS "proto"/"key"), so new attaches are gated by the new key
// without a reboot. Pass "" to make control open again.
void set_key(const char *key);

// Fill a DeviceRecord from the live UI/board/identity state (id, role, views,
// currentView, transports, authRequired).
void fill_device_record(proto::DeviceRecord &r);

// auth + version gate + create a session. Always fills `ack`. Returns
// ack.accepted.
bool handle_attach(const proto::Attach &a, proto::AttachAck &ack);

// Validate the session (refreshes it), record currentView, and post a
// ShowScreen command to the UI task. Always fills `ack`. Returns ack.ok.
bool handle_switch(const proto::Switch &s, proto::SwitchAck &ack);

// Refresh / end a session. Return whether the sessionId was known.
bool handle_heartbeat(const char *sid);
bool handle_detach(const char *sid);

// Fill a ControlState (active sessions + currentView) for serialization.
void fill_state(proto::ControlState &cs);

// Reap stale sessions (call periodically with millis()).
void tick(long now_ms);

// Copy the active sessions out (under the lock) for the UI-task frame overlay
// (Phase 3.2). Returns the count copied (<= cap).
int active_session_snapshot(proto::Session *out, int cap);

}  // namespace proto_target
