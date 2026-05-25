#pragma once

// On-device HTTP server.
//
// Endpoints (all return JSON unless noted):
//   GET  /                       human-readable HTML status page
//   GET  /api/state              { device, wifi, sk, screen, fps, mem }
//   GET  /api/screens            [ {id, title, hidden, active}, ... ]
//   POST /api/screen/<id>        switch screens (or 'next'/'prev')
//   GET  /api/sk                 current sk::Data snapshot
//   GET  /api/layout             current layout JSON (verbatim)
//   PUT  /api/layout             replace layout (text/plain or JSON body)
//   POST /api/cmd                text/plain body -> net::dispatchCommand
//   GET  /api/screenshot.bmp     BMP snapshot of the active screen
//
// Only meaningful when WiFi is up (STA or AP). Listens on port 80.

namespace web {

void setup();  // safe to call before wifi is up; defers binding
void loop();   // call frequently from main loop

}  // namespace web
