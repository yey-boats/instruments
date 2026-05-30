#include "source_nmea_wifi.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "storage.h"

#include "boat_data.h"
#include "net.h"
#include "nmea0183.h"

namespace nmea_wifi {

namespace {

constexpr const char *NS = "n0183w";
constexpr uint16_t DEFAULT_PORT = 10110;  // NMEA0183 over TCP/UDP convention

bool s_enabled = false;
Protocol s_proto = Protocol::Tcp;
String s_host = "";
uint16_t s_port = DEFAULT_PORT;

volatile bool s_connected = false;
volatile uint32_t s_bytes_in = 0;
volatile uint32_t s_sentences_ok = 0;
volatile uint32_t s_sentences_bad = 0;
volatile uint32_t s_last_rx_ms = 0;

TaskHandle_t s_task = nullptr;
nmea0183::Stream s_stream;

// Map an NMEA0183 FieldKind to a publish into boat::Snapshot.
void on_sentence(const nmea0183::ParseResult &r, void * /*user*/) {
    if (!r.ok) {
        s_sentences_bad++;
        return;
    }
    s_sentences_ok++;
    uint32_t now = millis();
    s_last_rx_ms = now;
    using FK = nmea0183::FieldKind;
    using boat::Snapshot;
    using boat::SourceKind;
    for (int i = 0; i < r.count; ++i) {
        const auto &f = r.fields[i];
        double v = f.value;
        switch (f.kind) {
        case FK::LatDeg:
            boat::publish(&Snapshot::lat_deg, SourceKind::NmeaWifi, now, v);
            break;
        case FK::LonDeg:
            boat::publish(&Snapshot::lon_deg, SourceKind::NmeaWifi, now, v);
            break;
        case FK::SogKn:
            boat::publish(&Snapshot::sog_mps, SourceKind::NmeaWifi, now, v * 0.514444);
            break;
        case FK::StwKn:
            boat::publish(&Snapshot::stw_mps, SourceKind::NmeaWifi, now, v * 0.514444);
            break;
        case FK::CogTrueDeg:
            boat::publish(&Snapshot::cog_true_rad, SourceKind::NmeaWifi, now, v * (M_PI / 180.0));
            break;
        case FK::HeadingTrueDeg:
            boat::publish(&Snapshot::heading_true_rad, SourceKind::NmeaWifi, now,
                          v * (M_PI / 180.0));
            break;
        case FK::HeadingMagDeg:
            // No mag field in Snapshot yet; treat as heading_true fallback when nothing better.
            boat::publish(&Snapshot::heading_true_rad, SourceKind::NmeaWifi, now,
                          v * (M_PI / 180.0));
            break;
        case FK::AwaDeg:
            boat::publish(&Snapshot::awa_rad, SourceKind::NmeaWifi, now, v * (M_PI / 180.0));
            break;
        case FK::AwsKn:
            boat::publish(&Snapshot::aws_mps, SourceKind::NmeaWifi, now, v * 0.514444);
            break;
        case FK::TwaDeg:
            boat::publish(&Snapshot::twa_rad, SourceKind::NmeaWifi, now, v * (M_PI / 180.0));
            break;
        case FK::TwsKn:
            boat::publish(&Snapshot::tws_mps, SourceKind::NmeaWifi, now, v * 0.514444);
            break;
        case FK::DepthM:
            boat::publish(&Snapshot::depth_m, SourceKind::NmeaWifi, now, v);
            break;
        case FK::WaterTempC:
            boat::publish(&Snapshot::water_temp_k, SourceKind::NmeaWifi, now, v + 273.15);
            break;
        case FK::XteNm:
            boat::publish(&Snapshot::xte_m, SourceKind::NmeaWifi, now, v * 1852.0);
            break;
        case FK::BtwTrueDeg:
            boat::publish(&Snapshot::btw_rad, SourceKind::NmeaWifi, now, v * (M_PI / 180.0));
            break;
        case FK::DtwNm:
            boat::publish(&Snapshot::dtw_m, SourceKind::NmeaWifi, now, v * 1852.0);
            break;
        default:
            break;
        }
    }
}

void load_prefs() {
    storage::Namespace p(NS, true);
    s_enabled = p.get_u8("enabled", 0) != 0;
    s_proto = static_cast<Protocol>(p.get_u8("proto", 0));
    s_host = String(p.get_string("host", "").c_str());
    s_port = p.get_u16("port", DEFAULT_PORT);
}

void save_prefs() {
    storage::Namespace p(NS, false);
    p.put_u8("enabled", s_enabled ? 1 : 0);
    p.put_u8("proto", static_cast<uint8_t>(s_proto));
    p.put_string("host", s_host.c_str());
    p.put_u16("port", s_port);
}

void run_tcp() {
    WiFiClient client;
    while (s_enabled && s_proto == Protocol::Tcp) {
        if (!net::wifiUp()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_host.length() == 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        net::logf("[n0183w] TCP connect %s:%u", s_host.c_str(), s_port);
        if (!client.connect(s_host.c_str(), s_port, 4000)) {
            net::logf("[n0183w] TCP connect failed");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        s_connected = true;
        net::logf("[n0183w] TCP up");
        uint8_t buf[256];
        while (s_enabled && client.connected()) {
            int n = client.available();
            if (n > 0) {
                if (n > (int)sizeof(buf)) n = sizeof(buf);
                int got = client.read(buf, n);
                if (got > 0) {
                    s_bytes_in += got;
                    nmea0183::stream_feed(s_stream, buf, got);
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        s_connected = false;
        client.stop();
        net::logf("[n0183w] TCP closed");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void run_udp() {
    WiFiUDP udp;
    bool listening = false;
    uint8_t buf[1024];
    while (s_enabled && s_proto == Protocol::Udp) {
        if (!net::wifiUp()) {
            if (listening) {
                udp.stop();
                listening = false;
                s_connected = false;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!listening) {
            if (udp.begin(s_port)) {
                listening = true;
                s_connected = true;
                net::logf("[n0183w] UDP listening on :%u", s_port);
            } else {
                net::logf("[n0183w] UDP bind failed");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }
        int sz = udp.parsePacket();
        if (sz > 0) {
            if (sz > (int)sizeof(buf)) sz = sizeof(buf);
            int got = udp.read(buf, sz);
            if (got > 0) {
                s_bytes_in += got;
                nmea0183::stream_feed(s_stream, buf, got);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (listening) {
        udp.stop();
        s_connected = false;
    }
}

void worker(void *) {
    nmea0183::stream_init(s_stream, on_sentence, nullptr);
    for (;;) {
        if (!s_enabled) {
            s_connected = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (s_proto == Protocol::Tcp)
            run_tcp();
        else
            run_udp();
    }
}

}  // namespace

void setup() {
    load_prefs();
    if (!s_task) {
        xTaskCreatePinnedToCore(worker, "n0183w", 4096, nullptr, 1, &s_task, 0);
    }
    net::logf("[n0183w] %s proto=%s host=%s port=%u", s_enabled ? "enabled" : "disabled",
              s_proto == Protocol::Tcp ? "tcp" : "udp", s_host.c_str(), s_port);
}

Status status() {
    Status s;
    s.enabled = s_enabled;
    s.proto = s_proto;
    s.host = s_host;
    s.port = s_port;
    s.connected = s_connected;
    s.bytes_in = s_bytes_in;
    s.sentences_ok = s_sentences_ok;
    s.sentences_bad = s_sentences_bad;
    s.last_rx_ms = s_last_rx_ms;
    return s;
}

bool handleSerialCommand(const String &line) {
    if (!line.startsWith("nmea-wifi")) return false;
    String rest = line.length() > 9 ? line.substring(9) : String("");
    rest.trim();
    if (rest.length() == 0 || rest == "status") {
        Status st = status();
        net::logf("[n0183w] enabled=%d proto=%s host=%s port=%u connected=%d "
                  "rx_bytes=%lu sentences ok=%lu bad=%lu last_rx_ago=%lums",
                  st.enabled, st.proto == Protocol::Tcp ? "tcp" : "udp", st.host.c_str(), st.port,
                  st.connected, (unsigned long)st.bytes_in, (unsigned long)st.sentences_ok,
                  (unsigned long)st.sentences_bad,
                  (unsigned long)(st.last_rx_ms ? (millis() - st.last_rx_ms) : 0));
        return true;
    }
    if (rest == "enable") {
        s_enabled = true;
        save_prefs();
        net::logf("[n0183w] enabled");
        return true;
    }
    if (rest == "disable") {
        s_enabled = false;
        save_prefs();
        net::logf("[n0183w] disabled");
        return true;
    }
    if (rest.startsWith("tcp ")) {
        String args = rest.substring(4);
        args.trim();
        int sp = args.indexOf(' ');
        String host = sp < 0 ? args : args.substring(0, sp);
        uint16_t port = sp < 0 ? DEFAULT_PORT : (uint16_t)args.substring(sp + 1).toInt();
        if (port == 0) port = DEFAULT_PORT;
        s_proto = Protocol::Tcp;
        s_host = host;
        s_port = port;
        s_enabled = true;
        save_prefs();
        net::logf("[n0183w] tcp %s:%u (enabled)", host.c_str(), port);
        return true;
    }
    if (rest.startsWith("udp")) {
        String args = rest.length() > 3 ? rest.substring(3) : String("");
        args.trim();
        uint16_t port = args.length() == 0 ? DEFAULT_PORT : (uint16_t)args.toInt();
        if (port == 0) port = DEFAULT_PORT;
        s_proto = Protocol::Udp;
        s_port = port;
        s_enabled = true;
        save_prefs();
        net::logf("[n0183w] udp :%u (enabled)", port);
        return true;
    }
    net::logf("[n0183w] usage: nmea-wifi [status|enable|disable|tcp <host> <port>|udp <port>]");
    return true;
}

}  // namespace nmea_wifi
