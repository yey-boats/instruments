// Headless ESP32-S3-DevKitC-1 control-protocol harness: a bare controller that
// loops discover -> attach -> switch every view -> heartbeat -> detach against a
// target display over IP (or, in the BLE variant, scan -> switch_on_peer). The
// on-hardware verification vehicle for the protocol.
//
// This build now sits on the SAME base infrastructure as the main firmware:
// net::setup() / net::loop() bring up the WiFi manager (NVS-backed),
// ArduinoOTA (<deviceId>.local:3232), BLE NUS + CONNECTION, mDNS, and the
// dispatchCommand console funnel. That makes the harness OTA-updatable and
// reconfigurable over BLE/serial without a reflash. The protocol cycle runs
// only when WiFi is up and no OTA is in flight, so OTA always wins.
//
// Gated by YEYBOATS_HARNESS so this translation unit contributes an empty body
// (no second setup()/loop()) on the display/knob firmware builds, where the
// default build_src_filter would otherwise pull it in alongside main.cpp.
#if defined(YEYBOATS_HARNESS)

#include <Arduino.h>
#include <Preferences.h>

#include "net.h"
#include "proto/proto.h"
#include "secrets.h"
#include "wifi_store.h"

#if defined(HARNESS_BLE)
#include "proto_ble.h"
#else
#include "proto_ip.h"
#endif

// Target base URL: build-flag override (-D HARNESS_TARGET=...) else mDNS
// default. The NVS value (namespace "harness", key "target") overrides both
// when set, so the target can be repointed at runtime over BLE/serial/OTA
// without reflashing.
#ifndef HARNESS_TARGET
#define HARNESS_TARGET "http://espdisp.local"
#endif

static String s_target = HARNESS_TARGET;

// Load the persisted target override (if any) into s_target.
static void load_target() {
    Preferences p;
    if (p.begin("harness", /*readOnly=*/true)) {
        String t = p.getString("target", "");
        p.end();
        if (t.length()) s_target = t;
    }
}

static void save_target(const String &t) {
    Preferences p;
    if (p.begin("harness", /*readOnly=*/false)) {
        p.putString("target", t);
        p.end();
    }
    s_target = t;
}

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
        net::logf("[harness] BLE peer=%s id=%s views=%d", peer.addr, dev.deviceId, dev.views_count);
        if (dev.views_count > 0) view_id = dev.views[0].id;
    } else {
        net::logf("[harness] BLE peer=%s id=%s (get_device failed)", peer.addr, peer.device_id);
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
    net::logf("[harness] BLE switch %s -> %s", view_id, ok ? "PASS" : "FAIL");
}

static void run_cycle() {
    s_peer_count = 0;
    int found = proto_ble::scan(3000, on_peer, nullptr);
    if (found <= 0) net::logf("[harness] BLE scan found no Control-service peers (%d)", found);
    net::logf("[harness] cycle done");
}

#else

static void run_cycle() {
    // DeviceRecord (~1.5 KB) and AttachAck (~1.5 KB, embeds a DeviceRecord) are
    // too large for the 8 KB Arduino loopTask stack once HTTPClient + JSON also
    // run; static keeps them off the stack (loop() is the only caller, serial).
    static proto::DeviceRecord dev;
    memset(&dev, 0, sizeof(dev));
    if (!proto_ip::get_device(s_target, dev)) {
        net::logf("[harness] FAIL get_device");
        return;
    }
    net::logf("[harness] target=%s views=%d", dev.deviceId, dev.views_count);

    proto::Attach a{};
    strcpy(a.v, "1.0");
    strcpy(a.controllerId, "harness-1");
    strcpy(a.name, "Harness");
    strcpy(a.color, "#e91e63");
    a.ttlMs = 10000;
    static proto::AttachAck ack;
    memset(&ack, 0, sizeof(ack));
    if (!proto_ip::attach(s_target, a, ack) || !ack.accepted) {
        net::logf("[harness] FAIL attach");
        return;
    }
    net::logf("[harness] attached sid=%s", ack.sessionId);

    for (int i = 0; i < dev.views_count; ++i) {
        proto::Switch sw{};
        strcpy(sw.v, "1.0");
        strncpy(sw.sessionId, ack.sessionId, sizeof(sw.sessionId) - 1);
        strncpy(sw.viewId, dev.views[i].id, sizeof(sw.viewId) - 1);
        proto::SwitchAck sa{};
        bool ok = proto_ip::do_switch(s_target, sw, sa) && sa.ok &&
                  strcmp(sa.currentView, dev.views[i].id) == 0;
        net::logf("[harness] switch %s -> %s", dev.views[i].id, ok ? "PASS" : "FAIL");
        proto_ip::heartbeat(s_target, ack.sessionId);
        delay(1500);
    }
    proto_ip::detach(s_target, ack.sessionId);
    net::logf("[harness] cycle done");
}

#endif  // HARNESS_BLE

// Extra console handler: lines that net / sk / layout didn't consume land here.
// `harness target <url>` repoints the target (persisted to NVS), `harness run`
// fires a single protocol cycle on demand, `harness status` prints state.
static bool harness_cmd(const String &line) {
    if (!line.startsWith("harness")) return false;
    String rest = line.substring(7);
    rest.trim();

    if (rest.startsWith("target")) {
        String url = rest.substring(6);
        url.trim();
        if (url.length()) {
            save_target(url);
            net::logf("[harness] target set -> %s", s_target.c_str());
        } else {
            net::logf("[harness] target = %s", s_target.c_str());
        }
        return true;
    }
    if (rest == "run") {
        net::logf("[harness] manual run requested");
        run_cycle();
        return true;
    }
    if (rest == "status" || rest.length() == 0) {
        net::logf("[harness] target=%s wifi=%s(%s) ota=%s id=%s", s_target.c_str(),
                  net::wifiUp() ? "up" : "down", net::wifiStateName(),
                  net::otaInProgress() ? "yes" : "no", net::deviceId().c_str());
        return true;
    }
    net::logf("[harness] commands: harness target <url> | harness run | harness status");
    return true;
}

void setup() {
    Serial.begin(115200);

    // Seed WiFi credentials from the build-time secrets ONLY when nothing is
    // already saved, so the harness associates headless on a fresh device
    // without manual provisioning. saveWifi() persists + reboots, so guard it
    // behind the empty-store check to avoid a reboot loop. wifi_store is loaded
    // and legacy-migrated inside net::setup(); load it here first so the count
    // reflects any previously-saved networks.
    wifi_store::load();
    wifi_store::migrate_legacy_if_any();
    if (wifi_store::count() == 0) {
        Serial.println("[harness] no saved WiFi - seeding from secrets.h");
        // saveWifi reboots; on the next boot wifi_store has the entry and we
        // skip this branch, so there is no loop.
        net::saveWifi(WIFI_SSID, WIFI_PASS);
        return;  // unreachable past the reboot, but explicit.
    }

    load_target();

    net::setup();
    net::setExtraCommandHandler(harness_cmd);

    net::logf("[harness] base infra up (OTA via %s.local:3232); target %s", net::deviceId().c_str(),
              s_target.c_str());
}

void loop() {
    // Base infra housekeeping: ArduinoOTA poll, WiFi manager, mDNS refresh,
    // console drain. Must run every loop so an OTA push is serviced promptly.
    net::loop();

    // Run the (blocking) protocol cycle only when the network is up AND no OTA
    // is in flight, so an inbound update always pre-empts the soak loop.
#if defined(HARNESS_BLE)
    // BLE central works off-grid; gate only on OTA so a WiFi-side OTA wins.
    if (!net::otaInProgress()) {
        run_cycle();
    }
#else
    if (net::wifiUp() && !net::otaInProgress()) {
        run_cycle();
    }
#endif

    delay(1000);
}

#endif  // YEYBOATS_HARNESS
