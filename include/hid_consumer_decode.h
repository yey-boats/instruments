#pragma once

// Pure (host-testable) decoding of BLE HID input reports from consumer-control
// remotes into firmware UI actions. No Arduino / NimBLE dependencies — the
// device-side ble_hid_host.cpp feeds notify payloads in here and posts the
// resulting actions to the app::Command queue; the native test suite
// (test/test_hid_decode) exercises the same code on the host.
//
// Report styles handled pragmatically (we do not parse the Report Map):
//   - usage-array reports: one or more little-endian 16-bit Consumer Page
//     usage codes (e.g. {0xE9, 0x00} = Volume Up). Detected when every
//     non-zero word is a usage code we recognize.
//   - bitmap reports: 1-2 bytes where each bit is one key, in the de-facto
//     order popularized by ESP32-BLE-Keyboard style firmwares
//     (bit0=NextTrack, bit1=PrevTrack, bit2=Stop, bit3=Play/Pause,
//     bit4=Mute, bit5=Vol+, bit6=Vol-).
//   - boot keyboard reports (8 bytes): scanned for Enter / KP-Enter only.
// An all-zero report is a key release and decodes to no actions.

#include <stddef.h>
#include <stdint.h>

namespace hid_decode {

// Consumer Page (0x0C) usage codes we care about.
constexpr uint16_t USAGE_MENU_PICK = 0x0041;  // "select"
constexpr uint16_t USAGE_NEXT_TRACK = 0x00B5;
constexpr uint16_t USAGE_PREV_TRACK = 0x00B6;
constexpr uint16_t USAGE_STOP = 0x00B7;
constexpr uint16_t USAGE_PLAY_PAUSE = 0x00CD;
constexpr uint16_t USAGE_MUTE = 0x00E2;
constexpr uint16_t USAGE_VOL_UP = 0x00E9;
constexpr uint16_t USAGE_VOL_DOWN = 0x00EA;

enum class Action : uint8_t {
    None = 0,
    BrightnessUp,    // VOL+
    BrightnessDown,  // VOL-
    ScreenNext,      // Next Track
    ScreenPrev,      // Prev Track
    Select,          // Play/Pause, Menu Pick, keyboard Enter
    Count,           // sentinel (array sizing)
};

// Map one Consumer Page usage code to an Action (None when unmapped).
Action usage_to_action(uint16_t usage);

// Decode a consumer-control input report into usage codes (usage-array or
// bitmap style, per the heuristics above). Returns the number of usages
// written to `usages` (at most `max`).
size_t decode_consumer_report(const uint8_t *data, size_t len, uint16_t *usages, size_t max);

// Full pipeline: decode the report (consumer or boot-keyboard) and map to
// actions, dropping unmapped usages. Returns the number of actions written.
size_t decode_actions(const uint8_t *data, size_t len, Action *out, size_t max);

// Human-readable action name (logging / tests).
const char *action_name(Action a);

}  // namespace hid_decode
