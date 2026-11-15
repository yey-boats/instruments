// Device half of the MIDL local ids. See include/midl_local_device.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_local_device.h"

#include <Arduino.h>  // temperatureRead / millis
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "device_identity.h"
#include "esp_heap_caps.h"
#include "midl_local_ids.h"
#include "net.h"
#include "signalk.h"
#include "trip_log.h"

namespace midl::local_dev {

double numeric(const char *id) {
    if (!id || !id[0]) return NAN;
    if (strncmp(id, "device.", 7) == 0) id += 7;
    if (!strcmp(id, "temperature") || !strcmp(id, "temp"))
        return (double)temperatureRead();                                       // SoC temp, degC
    if (!strcmp(id, "heap")) return (double)esp_get_free_heap_size() / 1024.0;  // free internal, KB
    if (!strcmp(id, "psram"))
        return (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0;  // KB
    if (!strcmp(id, "rssi")) return (double)net::rssi();                     // dBm
    if (!strcmp(id, "uptime")) return (double)(millis() / 1000UL);           // s
    // Trip log (built-in trip record), display units.
    if (!strcmp(id, "trip.distance")) return trip_log::distance_nm(trip_log::current());  // nm
    if (!strcmp(id, "trip.avgSpeed")) return trip_log::avg_kn(trip_log::current());       // kn
    if (!strcmp(id, "trip.maxSpeed")) return trip_log::max_kn(trip_log::current());       // kn
    return NAN;
}

bool text(const char *id, char *out, size_t cap, TextCache &c) {
    if (!midl::local_ids::is_text_id(id)) return false;
    uint32_t now = millis();
    if (!c.primed || now - c.last_ms >= 1000) {
        c.primed = true;
        c.last_ms = now;
        snprintf(c.ip, sizeof(c.ip), "%s", net::wifiUp() ? net::ipString().c_str() : "");
        snprintf(c.ssid, sizeof(c.ssid), "%s", net::ssidString().c_str());
        snprintf(c.sk_state, sizeof(c.sk_state), "%s", sk::connectionStatus().c_str());
        String h = sk::targetHost();
        if (h.length())
            snprintf(c.sk_host, sizeof(c.sk_host), "%s:%u", h.c_str(), (unsigned)sk::targetPort());
        else
            c.sk_host[0] = 0;
    }
    const device_identity::Identity &di = device_identity::get();
    midl::local_ids::Snapshot snap = {};
    snap.ip = c.ip;
    snap.ssid = c.ssid;
    snap.rssi = net::wifiUp() ? net::rssi() : 0;
    snap.sk_state = c.sk_state;
    snap.sk_host = c.sk_host;
    snap.build = di.build_time;
    snap.version = di.firmware_version;
    snap.uptime_s = millis() / 1000UL;
    snap.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snap.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snap.device_id = di.device_id;
    snap.underway_s = trip_log::current().underway_s;
    midl::local_ids::format(id, snap, out, cap);
    return true;
}

}  // namespace midl::local_dev
