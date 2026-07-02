#include "source_nmea2000.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

#include "boat_data.h"
#include "n2k_decode.h"
#include "net.h"
#include "storage.h"
#include "units.h"

#ifdef ENABLE_NMEA2000
#include "driver/twai.h"
#endif

namespace nmea2000 {

namespace {

constexpr const char *NS = "n2k";

bool s_enabled = false;
// Spec 12 §4 safety scaffold:
//   sniff       -> log every received frame's PGN + raw bytes (off by
//                  default to keep UDP/serial quiet).
//   tx_enabled  -> explicit gate that any future transmit path MUST
//                  consult before queuing a frame. Hardware is held in
//                  TWAI_MODE_LISTEN_ONLY when this is false, so we can
//                  guarantee no bus traffic regardless of code paths.
bool s_sniff = false;
bool s_tx_enabled = false;
int8_t s_rx_pin = -1;
int8_t s_tx_pin = -1;
volatile uint32_t s_frames_rx = 0;
volatile uint32_t s_pgns_decoded = 0;
volatile uint32_t s_pgns_unknown = 0;
volatile uint32_t s_last_rx_ms = 0;
// BUG-3 counted stat: PGN 130306 frames whose wind reference we do NOT map
// (ref 1 magnetic ground direction, reserved refs). Never folded into TWA.
volatile uint32_t s_wind_ref_dropped = 0;
TaskHandle_t s_task = nullptr;

void load_prefs() {
    storage::Namespace p(NS, true);
    s_enabled = p.get_u8("enabled", 0) != 0;
    s_sniff = p.get_u8("sniff", 0) != 0;
    s_tx_enabled = p.get_u8("tx_en", 0) != 0;
    s_rx_pin = p.get_i8("rx_pin", -1);
    s_tx_pin = p.get_i8("tx_pin", -1);
}

void save_prefs() {
    storage::Namespace p(NS, false);
    p.put_u8("enabled", s_enabled ? 1 : 0);
    p.put_u8("sniff", s_sniff ? 1 : 0);
    p.put_u8("tx_en", s_tx_enabled ? 1 : 0);
    p.put_i8("rx_pin", s_rx_pin);
    p.put_i8("tx_pin", s_tx_pin);
}

#ifdef ENABLE_NMEA2000

// J1939 29-bit CAN ID decode. NMEA2000 layers PGN/source/dest into the
// extended CAN identifier.
struct J1939Id {
    uint8_t priority;
    uint32_t pgn;  // 18 bits effective
    uint8_t source;
};

J1939Id decode_id(uint32_t id) {
    J1939Id r{};
    r.priority = (uint8_t)((id >> 26) & 0x7);
    uint8_t pf = (uint8_t)((id >> 16) & 0xFF);
    uint8_t ps = (uint8_t)((id >> 8) & 0xFF);
    uint8_t dp = (uint8_t)((id >> 24) & 0x1);
    r.source = (uint8_t)(id & 0xFF);
    if (pf < 240) {
        // PDU1 - destination-specific. PGN ignores PS in the ID.
        r.pgn = ((uint32_t)dp << 16) | ((uint32_t)pf << 8);
    } else {
        // PDU2 - broadcast.
        r.pgn = ((uint32_t)dp << 16) | ((uint32_t)pf << 8) | ps;
    }
    return r;
}

inline uint16_t u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
inline int16_t s16le(const uint8_t *p) {
    return (int16_t)u16le(p);
}
inline uint32_t u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void decode_pgn(uint32_t pgn, const uint8_t *d, uint8_t n) {
    using boat::Snapshot;
    using boat::SourceKind;
    const SourceKind src = SourceKind::Nmea2000;
    uint32_t now = millis();
    s_last_rx_ms = now;
    bool ok = false;

    switch (pgn) {
    case 127250: {  // Vessel heading (BUG-2: honor the reference byte)
        n2k::Heading127250 h = n2k::decode_127250(d, n);
        if (isfinite(h.variation_rad)) {
            boat::publish(&Snapshot::variation_rad, src, now, h.variation_rad);
            ok = true;
        }
        if (isfinite(h.heading_rad)) {
            if (h.reference == 0) {
                // True-referenced sensor heading.
                boat::publish(&Snapshot::heading_true_rad, src, now, h.heading_rad);
            } else if (h.reference == 1) {
                // Magnetic heading on its own field. When variation is also
                // available, derive true = magnetic + variation (+E) and
                // publish BOTH — deriving true from mag+variation is standard
                // practice, so heading_true consumers keep working on a
                // magnetic-only compass install.
                boat::publish(&Snapshot::heading_mag_rad, src, now, h.heading_rad);
                if (isfinite(h.variation_rad)) {
                    boat::publish(&Snapshot::heading_true_rad, src, now,
                                  units::wrap_2pi(h.heading_rad + h.variation_rad));
                }
            }
            // Unknown reference: heading dropped (variation may still have
            // published above).
            if (h.reference == 0 || h.reference == 1) ok = true;
        }
        break;
    }
    case 127251: {  // Rate of turn
        double rot = n2k::decode_127251_rot_radps(d, n);
        if (isfinite(rot)) {
            boat::publish(&Snapshot::rate_of_turn_radps, src, now, rot);
            ok = true;
        }
        break;
    }
    case 127257: {  // Attitude (roll/pitch; yaw duplicates heading)
        n2k::Attitude127257 a = n2k::decode_127257(d, n);
        if (isfinite(a.roll_rad)) {
            boat::publish(&Snapshot::roll_rad, src, now, a.roll_rad);
            ok = true;
        }
        if (isfinite(a.pitch_rad)) {
            boat::publish(&Snapshot::pitch_rad, src, now, a.pitch_rad);
            ok = true;
        }
        break;
    }
    case 127245: {  // Rudder — measured position onto the existing rudder field
        n2k::Rudder127245 r = n2k::decode_127245(d, n);
        if (isfinite(r.position_rad)) {
            boat::publish(&Snapshot::rudder_angle_rad, src, now, units::wrap_pi(r.position_rad));
            ok = true;
        }
        break;
    }
    case 127488: {  // Engine rapid — RPM (primary engine = instance 0)
        n2k::EngineRapid127488 e = n2k::decode_127488(d, n);
        if (e.instance == 0 && isfinite(e.rev_hz)) {
            boat::publish(&Snapshot::engine_rev_hz, src, now, e.rev_hz);
            ok = true;
        }
        break;
    }
    case 127489: {                  // Engine dynamic — FAST PACKET; reassemble then decode
        static n2k::FastPacket fp;  // single stream; worker task only
        if (!n2k::fastpacket_feed(fp, d, n)) break;
        n2k::EngineDynamic127489 e = n2k::decode_127489(fp.buf, fp.expect);
        fp.seq = 0xFF;               // consumed; wait for the next frame 0
        if (e.instance != 0) break;  // primary engine only (matches 127488)
        if (isfinite(e.oil_pressure_pa)) {
            boat::publish(&Snapshot::engine_oil_pressure_pa, src, now, e.oil_pressure_pa);
            ok = true;
        }
        if (isfinite(e.coolant_temp_k)) {
            boat::publish(&Snapshot::engine_coolant_temp_k, src, now, e.coolant_temp_k);
            ok = true;
        }
        if (isfinite(e.fuel_rate_m3s)) {
            boat::publish(&Snapshot::engine_fuel_rate_m3s, src, now, e.fuel_rate_m3s);
            ok = true;
        }
        // e.engine_hours_s: decoded but NOT published — boat::Snapshot has no
        // engine-hours field yet (noted follow-up in the coverage report).
        break;
    }
    case 127508: {  // Battery status
        if (n < 5) break;
        uint16_t v_raw = u16le(d + 1);
        if (v_raw != 0xFFFF) {
            double v = v_raw * 0.01;
            boat::publish(&Snapshot::battery_v, src, now, v);
            ok = true;
        }
        break;
    }
    case 128267: {  // Depth
        if (n < 6) break;
        uint32_t depth_raw = u32le(d + 1);
        if (depth_raw != 0xFFFFFFFF) {
            double depth = depth_raw * 0.01;
            boat::publish(&Snapshot::depth_m, src, now, depth);
            ok = true;
        }
        break;
    }
    case 129025: {  // Position rapid
        if (n < 8) break;
        int32_t lat_raw = (int32_t)u32le(d);
        int32_t lon_raw = (int32_t)u32le(d + 4);
        if (lat_raw != (int32_t)0x7FFFFFFF) {
            double lat = lat_raw * 1e-7;
            boat::publish(&Snapshot::lat_deg, src, now, lat);
            ok = true;
        }
        if (lon_raw != (int32_t)0x7FFFFFFF) {
            double lon = lon_raw * 1e-7;
            boat::publish(&Snapshot::lon_deg, src, now, lon);
            ok = true;
        }
        break;
    }
    case 129026: {  // COG/SOG rapid
        if (n < 6) break;
        uint16_t cog_raw = u16le(d + 2);
        uint16_t sog_raw = u16le(d + 4);
        if (cog_raw != 0xFFFF) {
            boat::publish(&Snapshot::cog_true_rad, src, now, cog_raw * 0.0001);
            ok = true;
        }
        if (sog_raw != 0xFFFF) {
            boat::publish(&Snapshot::sog_mps, src, now, sog_raw * 0.01);
            ok = true;
        }
        break;
    }
    case 130306: {  // Wind data (BUG-3: map the reference byte properly)
        n2k::Wind130306 w = n2k::decode_130306(d, n);
        if (isnan(w.speed_mps) || isnan(w.angle_rad)) break;
        double wa = units::wrap_pi(w.angle_rad);
        switch (w.target) {
        case n2k::WindTarget::Apparent:  // ref 2
            boat::publish(&Snapshot::aws_mps, src, now, w.speed_mps);
            boat::publish(&Snapshot::awa_rad, src, now, wa);
            ok = true;
            break;
        case n2k::WindTarget::True:  // ref 0 (ground) / ref 3 (boat-referenced)
            boat::publish(&Snapshot::tws_mps, src, now, w.speed_mps);
            boat::publish(&Snapshot::twa_rad, src, now, wa);
            ok = true;
            break;
        case n2k::WindTarget::Drop:  // ref 1 / reserved — counted, never TWA
        default:
            s_wind_ref_dropped++;
            break;
        }
        break;
    }

    // --- Raymarine pilot PGNs (spec 12 §4). Decoded from public
    // protocol references - no GPL source imported.
    case 127237: {  // Heading/Track Control (rudder commanded + locked heading)
        if (n < 8) break;
        // Bytes layout (subset): byte 4-5 = heading_to_steer (rad * 1e-4)
        uint16_t heading_raw = u16le(d + 4);
        if (heading_raw != 0xFFFF) {
            double hdg = heading_raw * 0.0001;
            hdg = units::wrap_pi(hdg);
            boat::publish(&Snapshot::autopilot_target_rad, src, now, hdg);
            ok = true;
        }
        break;
    }
    case 65360: {  // Raymarine Pilot Locked Heading
        // Manufacturer PGN: bytes 0-1 = manufacturer id + industry
        //                   bytes 2-3 = locked heading (rad * 1e-4)
        if (n < 8) break;
        uint16_t hdg_raw = u16le(d + 2);
        if (hdg_raw != 0xFFFF) {
            double hdg = hdg_raw * 0.0001;
            hdg = units::wrap_pi(hdg);
            boat::publish(&Snapshot::autopilot_target_rad, src, now, hdg);
            ok = true;
        }
        break;
    }
    case 65379: {  // Raymarine Pilot Mode / Submode
        // bytes 4-5 = pilot mode enum
        if (n < 8) break;
        uint16_t mode_raw = u16le(d + 4);
        const char *state = nullptr;
        switch (mode_raw) {
        case 0x0000:
            state = "standby";
            break;
        case 0x0040:
            state = "auto";
            break;
        case 0x0100:
            state = "wind";
            break;
        case 0x0180:
            state = "track";
            break;
        default:
            break;
        }
        if (state) {
            boat::publish_autopilot_state(src, now, state);
            ok = true;
        }
        break;
    }
    case 65288: {  // Raymarine Pilot Alarm State
        // Minimal: any non-zero alarm code -> mark autopilot warning
        // by appending "/alarm" suffix to the current state, or
        // record the bare alarm flag in the field for now. Since we
        // don't have a dedicated autopilot_alarm field, just log.
        if (n < 8) break;
        uint16_t alarm = u16le(d + 2);
        if (alarm) {
            net::logf("[n2k] Raymarine alarm code=0x%04x", alarm);
            ok = true;
        }
        break;
    }
    case 65345: {  // Raymarine Pilot Wind Angle (track-wind ref)
        // bytes 2-3 = locked wind angle (rad * 1e-4)
        if (n < 8) break;
        uint16_t wa_raw = u16le(d + 2);
        if (wa_raw != 0xFFFF) {
            double wa = wa_raw * 0.0001;
            wa = units::wrap_pi(wa);
            // No dedicated field; piggyback on twa_rad as
            // "target wind angle" hint when in wind mode.
            boat::publish(&Snapshot::twa_rad, src, now, wa);
            ok = true;
        }
        break;
    }

    default:
        break;
    }

    if (ok)
        s_pgns_decoded++;
    else
        s_pgns_unknown++;
}

void worker(void *) {
    bool installed = false;
    for (;;) {
        if (!s_enabled || s_rx_pin < 0 || s_tx_pin < 0) {
            if (installed) {
                twai_stop();
                twai_driver_uninstall();
                installed = false;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!installed) {
            twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
                (gpio_num_t)s_tx_pin, (gpio_num_t)s_rx_pin, TWAI_MODE_LISTEN_ONLY);
            twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
            twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
            if (twai_driver_install(&g, &t, &f) != ESP_OK) {
                net::logf("[n2k] driver install failed");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            if (twai_start() != ESP_OK) {
                net::logf("[n2k] start failed");
                twai_driver_uninstall();
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            installed = true;
            net::logf("[n2k] listening rx=%d tx=%d", s_rx_pin, s_tx_pin);
        }
        twai_message_t msg{};
        esp_err_t e = twai_receive(&msg, pdMS_TO_TICKS(100));
        if (e == ESP_OK) {
            s_frames_rx++;
            if (msg.extd) {
                J1939Id id = decode_id(msg.identifier);
                if (s_sniff) {
                    // Dump frame as hex. Keep one log line so UDP
                    // listeners can grep. Compact format: pgn, src,
                    // prio, dlc, bytes.
                    char hex[3 * 8 + 1];  // up to 8 bytes -> "XX XX ..."
                    int hp = 0;
                    for (int i = 0; i < msg.data_length_code && i < 8; ++i) {
                        hp += snprintf(hex + hp, sizeof(hex) - hp, "%02X%s", msg.data[i],
                                       i + 1 < msg.data_length_code ? " " : "");
                    }
                    hex[sizeof(hex) - 1] = '\0';
                    net::logf("[n2k-sniff] pgn=%lu src=%u prio=%u dlc=%u %s", (unsigned long)id.pgn,
                              id.source, id.priority, msg.data_length_code, hex);
                }
                decode_pgn(id.pgn, msg.data, msg.data_length_code);
            }
        }
    }
}

#else  // !ENABLE_NMEA2000

void worker(void *) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

#endif

}  // namespace

void setup() {
    load_prefs();
#ifdef ENABLE_NMEA2000
    if (!s_task) {
        xTaskCreatePinnedToCore(worker, "n2k", 4096, nullptr, 1, &s_task, 0);
    }
    net::logf("[n2k] %s rx=%d tx=%d", s_enabled ? "enabled" : "disabled", s_rx_pin, s_tx_pin);
#else
    net::logf("[n2k] not compiled in (build with -DENABLE_NMEA2000)");
#endif
}

Status status() {
    Status s{};
#ifdef ENABLE_NMEA2000
    s.compiled_in = true;
#else
    s.compiled_in = false;
#endif
    s.enabled = s_enabled;
    s.sniff = s_sniff;
    s.tx_enabled = s_tx_enabled;
    s.rx_pin = s_rx_pin;
    s.tx_pin = s_tx_pin;
    s.frames_rx = s_frames_rx;
    s.pgns_decoded = s_pgns_decoded;
    s.pgns_unknown = s_pgns_unknown;
    s.last_rx_ms = s_last_rx_ms;
    return s;
}

bool handleSerialCommand(const String &line) {
    if (!line.startsWith("n2k")) return false;
    String rest = line.length() > 3 ? line.substring(3) : String("");
    rest.trim();
    if (rest.length() == 0 || rest == "status") {
        Status st = status();
        net::logf("[n2k] compiled=%d enabled=%d sniff=%d tx_enabled=%d "
                  "rx=%d tx=%d frames=%lu decoded=%lu unknown=%lu "
                  "wind_ref_dropped=%lu last_rx_ago=%lums",
                  st.compiled_in, st.enabled, st.sniff, st.tx_enabled, st.rx_pin, st.tx_pin,
                  (unsigned long)st.frames_rx, (unsigned long)st.pgns_decoded,
                  (unsigned long)st.pgns_unknown, (unsigned long)s_wind_ref_dropped,
                  st.last_rx_ms ? (unsigned long)(millis() - st.last_rx_ms) : 0UL);
        return true;
    }
    if (rest == "enable") {
        s_enabled = true;
        save_prefs();
        net::logf("[n2k] enabled");
        return true;
    }
    if (rest == "disable") {
        s_enabled = false;
        save_prefs();
        net::logf("[n2k] disabled");
        return true;
    }
    if (rest == "sniff on" || rest == "sniff") {
        s_sniff = true;
        save_prefs();
        net::logf("[n2k] sniff on - logging every received frame");
        return true;
    }
    if (rest == "sniff off") {
        s_sniff = false;
        save_prefs();
        net::logf("[n2k] sniff off");
        return true;
    }
    if (rest == "tx on") {
        // Spec 12 §4 transmit gate. We never wire TX in code today; the
        // gate exists so a future autopilot/NMEA transmit backend can
        // refuse to send until the operator opts in explicitly. The
        // hardware is still held in LISTEN_ONLY by the worker - the
        // gate doesn't bypass that.
        s_tx_enabled = true;
        save_prefs();
        net::logf("[n2k] tx gate OPEN (hardware still listen-only; "
                  "transmit code path will honor this flag)");
        return true;
    }
    if (rest == "tx off") {
        s_tx_enabled = false;
        save_prefs();
        net::logf("[n2k] tx gate closed");
        return true;
    }
    if (rest.startsWith("pins ")) {
        String args = rest.substring(5);
        args.trim();
        int sp = args.indexOf(' ');
        if (sp < 0) {
            net::logf("[n2k] usage: n2k pins <rx> <tx>");
            return true;
        }
        s_rx_pin = (int8_t)args.substring(0, sp).toInt();
        s_tx_pin = (int8_t)args.substring(sp + 1).toInt();
        save_prefs();
        net::logf("[n2k] rx=%d tx=%d (save)", s_rx_pin, s_tx_pin);
        return true;
    }
    net::logf("[n2k] usage: n2k [status|enable|disable|sniff on|sniff off|"
              "tx on|tx off|pins <rx> <tx>]");
    return true;
}

}  // namespace nmea2000
