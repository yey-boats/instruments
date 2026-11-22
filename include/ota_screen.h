// On-device firmware-update screen (instruments#106). Rules and pacing live in
// include/ota_progress.h (host-tested); this is the device half.
//
// The OTA task (manager pull OTA in manager.cpp, ArduinoOTA/espota in net.cpp)
// owns the calls below; the LVGL work happens on the UI task, which is paused
// for the OTA (app_pause_ui) and serves one request at a time from its paused
// loop (ui_service). Every OTA-task call blocks until the UI task has rendered
// the request and the panel has scanned it out (bounded by a timeout), so no
// LVGL/panel work overlaps an Update.write: redraws happen between chunks.
//
// Call order on the OTA task:
//   app_pause_ui(true);
//   begin(...)              before the first flash write (Update.begin/write)
//   progress(done, total)   after each Update.write; redraws at 4 % steps
//   status("Verifying")     optional phase text
//   succeeded()  -> caller reboots      | failed(detail) -> restores the UI,
//                                         then the caller app_pause_ui(false)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <stdint.h>

namespace ota_screen {

// Shows the update screen. `version` = the incoming firmware version, or
// nullptr/"" when unknown; `total` = image bytes, 0 when unknown (the bar is
// indeterminate until progress() brings a total). Returns true when the
// screen was on the panel before returning (false: the UI task did not
// answer in time; the OTA goes on regardless).
bool begin(const char *source, const char *version, uint32_t total);
// True between begin() and succeeded()/failed().
bool active();
// After an Update.write. Redraws only when the bar moves a whole step.
void progress(uint32_t done, uint32_t total);
// Phase text under the bar ("Verifying", "Installing"); redraws now.
void status(const char *text);
// Image written and verified: show "Restarting" (backlight on). The caller
// reboots.
void succeeded();
// The OTA failed: show the reason for a moment, then put the previous screen,
// the top layer and the user's backlight back. Returns false when no update
// screen was up (nothing to restore). The caller resumes the UI after it.
bool failed(const char *detail);

// UI task only, from the paused loop: wait up to `wait_ms` for a request and
// serve it.
void ui_service(uint32_t wait_ms);
// UI task only, from the running loop: finishes a restore that timed out.
void ui_poll();

}  // namespace ota_screen
