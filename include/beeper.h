#pragma once

// Audible alerts per docs/specs/12 §5 Beeper Task.
//
// Boards without a beeper still link this module - all calls become
// no-ops + a single log line. board::capabilities().beeper tells
// callers whether actual sound will come out; UI surfaces (settings
// page, manager beep command) check this flag and skip the control
// when false.
//
// Patterns run on a low-priority FreeRTOS task pinned to core 0 so a
// long alarm pattern never blocks the UI. Critical alarms repeat;
// short taps are nonblocking.

#include <Arduino.h>
#include <stdint.h>

namespace beeper {

// Returns true if the firmware was built with a beeper backend on the
// current board.
bool available();

// Returns the user-configurable audible_alarms preference. When false,
// every public function (beep_short, alarm_pattern) becomes a no-op
// even on hardware-capable boards.
bool audible_alarms_enabled();
void set_audible_alarms(bool enabled);

// Single short beep. Returns immediately.
void beep_short(uint32_t duration_ms = 50);

// Repeating pattern. Runs `repeat` times of `on_ms` on / `off_ms` off.
// `repeat=0` means infinite until alarm_stop() is called. Returns
// immediately.
void alarm_pattern(uint32_t on_ms, uint32_t off_ms, uint16_t repeat);

// Cancel any running alarm. Safe to call when no alarm is active.
void alarm_stop();

// One-time module init. Called from main.cpp setup().
void setup();

// Console: beep [ms] | beep-alarm <on> <off> <count> | beep-stop |
//          audible-alarms <on|off>
bool handleSerialCommand(const String &line);

}  // namespace beeper
