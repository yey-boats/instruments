#pragma once
#include <Arduino.h>
#include "proto/proto.h"

namespace proto_ip {
// Controller side: HTTP calls against a target base URL ("http://host[:port]").
// Each returns true on a 2xx + parsed ack. Uses HTTPClient; call off the UI task.
bool get_device(const String &base, proto::DeviceRecord &out);
bool attach(const String &base, const proto::Attach &a, proto::AttachAck &ack);
bool do_switch(const String &base, const proto::Switch &s, proto::SwitchAck &ack);
bool heartbeat(const String &base, const char *sessionId);
bool detach(const String &base, const char *sessionId);
bool get_state(const String &base, proto::ControlState &out);
}  // namespace proto_ip
