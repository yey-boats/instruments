#pragma once

// Documentation catalog for every console command the firmware accepts
// over BLE NUS, USB serial, and (where allowed) /api/cmd.
//
// Lives in flash (PROGMEM) and is rendered by:
//   GET /api/commands       -> JSON list (machine readable)
//   GET /help/commands      -> HTML page
//
// Commands flagged http=false are blocked at /api/cmd (HTTP 403):
//   input injection (tap/swipe/gesture/touch) - per security policy

#include <Arduino.h>

namespace cmd_catalog {

struct Entry {
    const char *category;  // "net" / "sk" / "boat" / "manager" / ...
    const char *syntax;    // canonical usage string
    const char *summary;   // one-line description
    bool http;             // reachable over /api/cmd
    bool ble_serial;       // always true today; reserved for future gating
};

const Entry *entries();
size_t entry_count();

}  // namespace cmd_catalog
