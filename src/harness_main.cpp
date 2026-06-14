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
#include "secrets.h"

#if defined(HARNESS_BLE)
#include <NimBLEDevice.h>

#include "proto_ble.h"
#else
#include "proto_ip.h"
#endif

// Target base URL: build-flag override (-D HARNESS_TARGET=...) else mDNS default.
#ifndef HARNESS_TARGET
#define HARNESS_TARGET "http://espdisp.local"
#endif

static String s_target = HARNESS_TARGET;

#if defined(HARNESS_BLE)

// BLE-central soak: scan for espdisp Control-service peers, then for each one
// run the on-demand connect -> attach -> switch -> detach -> disconnect path
// (proto_ble::switch_on_peer does the full RAII-cleaned sequence). This loop is
// the soak vehicle for the documented internal-SRAM-starvation risk of holding
// the NimBLE central role on the ESP32-S3.
static int s_peer_count = 0;

static void on_peer(const proto_ble::Peer &peer, void *) {
    ++s_peer_count;

    // Optionally enumerate the peer's views to pick a real viewId; fall back to
    // a stub if the device read fails so we still exercise the switch path.
    // DeviceRecord (~1.5 KB) is static to keep it off the task stack (the scan
    // callback runs serially under proto_ble::scan on the one loopTask).
    static proto::DeviceRecord dev;
    memset(&dev, 0, sizeof(dev));
    const char *view_id = "0";
    if (proto_ble::get_device_on_peer(peer.addr, dev)) {
        Serial.printf("[harness] BLE peer=%s id=%s views=%d\n", peer.addr, dev.deviceId,
                      dev.views_count);
        if (dev.views_count > 0) view_id = dev.views[0].id;
    } else {
        Serial.printf("[harness] BLE peer=%s id=%s (get_device failed)\n", peer.addr,
                      peer.device_id);
    }

    proto::Attach a{};
    strcpy(a.v, "1.0");
    strcpy(a.controllerId, "harness-ble");
    strcpy(a.name, "Harness BLE");
    strcpy(a.color, "#e91e63");
    a.ttlMs = 10000;

    proto::Switch sw{};
    strcpy(sw.v, "1.0");
    strncpy(sw.viewId, view_id, sizeof(sw.viewId) - 1);

    bool ok = proto_ble::switch_on_peer(peer.addr, a, sw);
    Serial.printf("[harness] BLE switch %s -> %s\n", view_id, ok ? "PASS" : "FAIL");
}

static void run_cycle() {
    s_peer_count = 0;
    int found = proto_ble::scan(3000, on_peer, nullptr);
    if (found <= 0)
        Serial.printf("[harness] BLE scan found no Control-service peers (%d)\n", found);
    Serial.println("[harness] cycle done");
}

#else

static void run_cycle() {
    // DeviceRecord (~1.5 KB) and AttachAck (~1.5 KB, embeds a DeviceRecord) are
    // too large for the 8 KB Arduino loopTask stack once HTTPClient + JSON also
    // run; static keeps them off the stack (loop() is the only caller, serial).
    static proto::DeviceRecord dev;
    memset(&dev, 0, sizeof(dev));
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
    static proto::AttachAck ack;
    memset(&ack, 0, sizeof(ack));
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

#endif  // HARNESS_BLE

void setup() {
    Serial.begin(115200);

#if defined(HARNESS_BLE)
    // BLE central works off-grid; skip the WiFi-connect wait entirely. Bring up
    // the NimBLE stack so proto_ble's scan/connect calls have a controller. The
    // harness is central-only, so no server/advertising — just init + MTU.
    NimBLEDevice::init("harness");
    NimBLEDevice::setMTU(247);
    Serial.println("[harness] BLE central up (scan->switch_on_peer soak)");
#else
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    // Bounded connect with retry so a transient AP/RSSI hiccup doesn't wedge the
    // harness in a silent infinite wait; re-issue begin() every ~20 s until up.
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        if (millis() - started > 20000) {
            Serial.println("[harness] wifi still connecting, retrying begin()...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            started = millis();
        }
    }
    Serial.printf("[harness] wifi up %s, target %s\n", WiFi.localIP().toString().c_str(),
                  s_target.c_str());
#endif
}

void loop() {
    run_cycle();
    delay(5000);
}

#endif  // ESPDISP_HARNESS
