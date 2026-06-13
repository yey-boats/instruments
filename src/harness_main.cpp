// Headless ESP32-S3-DevKitC-1 control-protocol harness: a bare controller that
// loops discover -> attach -> switch every view -> heartbeat -> detach against a
// target display over IP. The on-hardware verification vehicle for the protocol.
//
// Gated by ESPDISP_HARNESS so this translation unit contributes an empty body
// (no second setup()/loop()) on the display/knob firmware builds, where the
// default build_src_filter would otherwise pull it in alongside main.cpp.
#if defined(ESPDISP_HARNESS)

#include <Arduino.h>
#include <WiFi.h>

#include "proto/proto.h"
#include "proto_ip.h"
#include "secrets.h"

// Target base URL: build-flag override (-D HARNESS_TARGET=...) else mDNS default.
#ifndef HARNESS_TARGET
#define HARNESS_TARGET "http://espdisp.local"
#endif

static String s_target = HARNESS_TARGET;

static void run_cycle() {
    proto::DeviceRecord dev;
    if (!proto_ip::get_device(s_target, dev)) {
        Serial.println("[harness] FAIL get_device");
        return;
    }
    Serial.printf("[harness] target=%s views=%d\n", dev.deviceId, dev.views_count);

    proto::Attach a{};
    strcpy(a.v, "1.0");
    strcpy(a.controllerId, "harness-1");
    strcpy(a.name, "Harness");
    strcpy(a.color, "#e91e63");
    a.ttlMs = 10000;
    proto::AttachAck ack{};
    if (!proto_ip::attach(s_target, a, ack) || !ack.accepted) {
        Serial.println("[harness] FAIL attach");
        return;
    }
    Serial.printf("[harness] attached sid=%s\n", ack.sessionId);

    for (int i = 0; i < dev.views_count; ++i) {
        proto::Switch sw{};
        strcpy(sw.v, "1.0");
        strncpy(sw.sessionId, ack.sessionId, sizeof(sw.sessionId) - 1);
        strncpy(sw.viewId, dev.views[i].id, sizeof(sw.viewId) - 1);
        proto::SwitchAck sa{};
        bool ok = proto_ip::do_switch(s_target, sw, sa) && sa.ok &&
                  strcmp(sa.currentView, dev.views[i].id) == 0;
        Serial.printf("[harness] switch %s -> %s\n", dev.views[i].id, ok ? "PASS" : "FAIL");
        proto_ip::heartbeat(s_target, ack.sessionId);
        delay(1500);
    }
    proto_ip::detach(s_target, ack.sessionId);
    Serial.println("[harness] cycle done");
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED)
        delay(200);
    Serial.printf("[harness] wifi up %s, target %s\n", WiFi.localIP().toString().c_str(),
                  s_target.c_str());
}

void loop() {
    run_cycle();
    delay(5000);
}

#endif  // ESPDISP_HARNESS
