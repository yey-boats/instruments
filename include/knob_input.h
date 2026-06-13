#pragma once

// Encoder + push-button input source for the Waveshare knob. Runs a
// dedicated task that classifies quadrature detents (ESP32Encoder/PCNT)
// and button gestures (OneButton) into knob::Event values, posted to the
// UI queue as app::CommandType::Knob. Device-only; empty on other boards.

namespace knob_input {

// Start the encoder/button task. No-op unless BOARD_ID_WAVESHARE_KNOB_1_8.
void setup();

// Runtime encoder calibration. The Waveshare knob's quadrature counts and
// rotation polarity depend on the physical encoder wiring, which can't be
// verified until the hardware is in hand. These let the user tune both over
// the console (serial/BLE) without reflashing; values persist in NVS.
// Defaults reproduce the original compile-time behavior exactly
// (counts_per_detent == 4, invert == false). On non-knob boards these are
// no-op / default-returning stubs.

// Encoder transitions consumed per emitted detent event. Clamped to [1, 8].
// Persisted; applied live to the running knob task.
void set_counts_per_detent(int n);
int counts_per_detent();

// When true, swap which physical rotation direction emits CW vs CCW.
// Persisted; applied live.
void set_invert(bool on);
bool invert();

}  // namespace knob_input
