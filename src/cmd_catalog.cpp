#include "cmd_catalog.h"

namespace cmd_catalog {

// PROGMEM-resident catalog. Update alongside any new CLI handler so
// the help page stays in sync with the dispatch chain. Categories are
// ordered roughly the way net::dispatchCommand tries them.

static const Entry CATALOG[] = {
    // --- net (identity, WiFi, BLE, OTA) ---
    {"net", "ip", "Print current IP and WiFi state", true, true},
    {"net", "scan", "Async WiFi scan", true, true},
    {"net", "wifi-list", "List saved WiFi networks", true, true},
    {"net", "wifi <ssid> <pass>", "Save WiFi creds and reboot", false, true},
    {"net", "wifi-forget <ssid>", "Drop a saved network", true, true},
    {"net", "id <name>|auto", "Set device id or restore hardware-derived id; reboots", true, true},
    {"net", "reboot", "Reboot the device", true, true},

    // --- screen / UI ---
    {"ui", "screen", "Print current screen id", true, true},
    {"ui", "screen <id|next|prev>", "Navigate to a screen", true, true},
    {"ui", "bright", "Read backlight (0..255)", true, true},
    {"ui", "bright <0-255>", "Set backlight", true, true},
    {"ui", "theme <day|night|auto>", "Set theme", true, true},
    {"ui", "demo on|off", "Toggle synthetic demo data", true, true},
    {"ui", "mob", "Long-press equivalent: activate MOB", true, true},
    {"ui", "mob-clear", "Clear MOB overlay", true, true},

    // --- bench / diagnostics ---
    {"diag", "bench", "Dump FPS, flush, latencies, queues, gestures", true, true},
    {"diag", "bench-reset", "Zero the latency histograms", true, true},
    {"diag", "latency-reset", "Same as bench-reset (alias)", true, true},

    // --- SignalK ---
    {"signalk", "sk <host> [port]", "Save SK host:port and reboot", true, true},
    {"signalk", "sk-host auto", "Clear saved host; resume mDNS auto-discovery", true, true},
    {"signalk", "sk-discover", "Run one mDNS _signalk-ws._tcp query now", true, true},
    {"signalk", "sk-status", "Print SK connection + token length", true, true},
    {"signalk", "sk-dump", "Dump latest parsed SK values", true, true},
    {"signalk", "sk-token <jwt>", "Save SK auth token (reboots)", true, true},
    {"signalk", "sk-token clear", "Remove SK auth token (reboots)", true, true},
    {"signalk", "sk-test", "Plain TCP connect probe to SK host:port", true, true},
    {"signalk", "sk-reconnect", "Force ws.begin() against current host/port/token", true, true},
    {"signalk", "sk-ap-state <standby|auto|wind|track>", "PUT autopilot state via SK REST", true,
     true},
    {"signalk", "sk-ap-adjust <degrees>", "PUT actions/adjustHeading via SK REST", true, true},

    // --- NMEA-WiFi (NMEA0183 over TCP/UDP) ---
    {"nmea-wifi", "nmea-wifi", "Print current status", true, true},
    {"nmea-wifi", "nmea-wifi status", "Same as above", true, true},
    {"nmea-wifi", "nmea-wifi enable", "Enable the source", true, true},
    {"nmea-wifi", "nmea-wifi disable", "Disable the source", true, true},
    {"nmea-wifi", "nmea-wifi tcp <host> <port>", "Configure TCP-client mode and enable", true,
     true},
    {"nmea-wifi", "nmea-wifi udp <port>", "Configure UDP-listener mode and enable", true, true},

    // --- NMEA2000 (CAN listen-only) ---
    {"n2k", "n2k", "Print status (compiled-in, frames, decoded, pins)", true, true},
    {"n2k", "n2k status", "Same as above", true, true},
    {"n2k", "n2k enable", "Enable (requires -DENABLE_NMEA2000 + transceiver)", true, true},
    {"n2k", "n2k disable", "Disable", true, true},
    {"n2k", "n2k pins <rx> <tx>", "Set TWAI rx/tx GPIOs", true, true},
    {"n2k", "n2k sniff on|off", "Dump every received frame as hex (spec 12 §4)", true, true},
    {"n2k", "n2k tx on|off", "Transmit gate; hardware stays listen-only (spec 12 §4)", true, true},

    // --- boat::Snapshot ---
    {"boat", "boat", "Dump fused snapshot with per-field source + age", true, true},
    {"boat", "boat snapshot", "Same as above", true, true},
    {"boat", "boat priority", "Show source priority chain + freshness windows", true, true},
    {"boat", "boat reset", "Clear all fields", true, true},
    {"boat", "boat timeout <n2k|wifi|sk|demo> <ms>", "Tune freshness window at runtime", true,
     true},

    // --- board ---
    {"board", "board", "Print id/geometry/capabilities/backlight", true, true},
    {"board", "board bright <0-255>", "Set backlight via board:: API", true, true},

    // --- manager (spec 17) ---
    {"manager", "manager", "Print current manager state", true, true},
    {"manager", "manager-status", "Same as above", true, true},
    {"manager", "manager-register <url>", "Set the manager endpoint and re-register", true, true},
    {"manager", "manager-token <jwt|clear>",
     "Set/clear the plugin/device bearer (X-EspDisp-Authorization)", true, true},
    {"manager", "manager-sk-token <jwt|clear>",
     "Set/clear the SK-level bearer used to pass SK security", true, true},
    {"manager", "manager-forget", "Wipe endpoint+token; back to idle", true, true},
    {"manager", "manager-discover", "mDNS _espdisp-mgmt._tcp probe (spec 18 §4)", true, true},
    {"manager", "manager-errors", "Dump recent errors ring buffer (spec 17 §5)", true, true},
    {"manager", "manager-errors clear", "Reset the recent errors ring buffer", true, true},
    {"manager", "manager-layout", "Print applied render plan summary (spec 19 D7)", true, true},
    {"manager", "manager-widgets", "Print applied widget definitions (spec 19 D7)", true, true},
    {"manager", "manager-config-dump", "JSON-dump applied config summary (spec 19 D7)", true, true},
    {"manager", "font-dump", "List compiled font sizes + sample resolves (spec 19 D7)", true, true},

    // --- layout ---
    {"layout", "layout-show", "Dump current loaded layout (JSON)", true, true},
    {"layout", "layout-fetch", "Pull layout from SK REST and apply", true, true},
    {"layout", "layout-load <name>", "Load a built-in/saved layout", true, true},

    // --- input injection (BLE/serial only) ---
    {"input-test", "touch <x> <y> <0|1>", "Write raw touch snapshot", false, true},
    {"input-test", "tap <x> <y> [hold_ms]", "Synth tap: press, hold, release (LVGL CLICKED)", false,
     true},
    {"input-test", "swipe <x0> <y0> <x1> <y1> [dur_ms] [steps]",
     "Synth swipe: intermediate samples + gesture detect", false, true},
    {"input-test", "gesture <left|right|up|down>", "Post ShowScreen directly to action queue",
     false, true},

    // --- autopilot backend (spec 12 §4) ---
    {"autopilot", "autopilot", "Show current backend, mode, heading, target", true, true},
    {"autopilot", "autopilot status", "Same as above", true, true},
    {"autopilot", "autopilot mode <m>", "Set mode: standby|auto|wind|pretrack|track", true, true},
    {"autopilot", "autopilot heading <n>", "Adjust target heading by N degrees (-90..+90)", true,
     true},
    {"autopilot", "autopilot silence", "Silence the currently-sounding alarm", true, true},
    {"autopilot", "autopilot backend <b>", "Set backend: signalk | nmea2000", true, true},

    // --- beeper (spec 12 §5) ---
    {"beeper", "beep", "Short tap (50 ms default)", true, true},
    {"beeper", "beep <ms>", "Short beep of N ms", true, true},
    {"beeper", "beep-alarm <on_ms> <off_ms> <count>", "Repeating alarm pattern (count=0 = forever)",
     true, true},
    {"beeper", "beep-stop", "Cancel any running alarm", true, true},
    {"beeper", "audible-alarms <on|off>", "Toggle whether beeper sounds at all", true, true},

    // --- config (spec 08) ---
    {"config", "config-status", "Print config_runtime status", true, true},

    // --- touch IRQ probe (spec 14) ---
    {"touch", "irq-probe", "Arm GT911 INT line probe", true, true},
    {"touch", "irq-probe-dump", "Print probe counters", true, true},
    {"touch", "irq-probe-stop", "Disarm probe", true, true},
};

const Entry *entries() {
    return CATALOG;
}
size_t entry_count() {
    return sizeof(CATALOG) / sizeof(CATALOG[0]);
}

}  // namespace cmd_catalog
