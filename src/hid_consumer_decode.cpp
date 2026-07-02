#include "hid_consumer_decode.h"

// Pure C++ — must stay free of Arduino / NimBLE / ESP-IDF includes so the
// native test env can compile it (test/test_hid_decode includes this TU
// directly, like test_boat_data does with source_signalk.cpp).

namespace hid_decode {

// Consumer usages we RECOGNIZE (for the "is this a usage-array report?"
// heuristic). A superset of the mapped ones: seeing e.g. Stop or Mute still
// proves the report carries usage codes rather than a bitmap.
static bool is_known_usage(uint16_t u) {
    switch (u) {
    case USAGE_MENU_PICK:
    case 0x0040:  // Menu
    case USAGE_NEXT_TRACK:
    case USAGE_PREV_TRACK:
    case USAGE_STOP:
    case 0x00B8:  // Eject
    case USAGE_PLAY_PAUSE:
    case USAGE_MUTE:
    case USAGE_VOL_UP:
    case USAGE_VOL_DOWN:
    case 0x0030:  // Power
    case 0x0223:  // AC Home
    case 0x0224:  // AC Back
        return true;
    default:
        return false;
    }
}

Action usage_to_action(uint16_t usage) {
    switch (usage) {
    case USAGE_VOL_UP:
        return Action::BrightnessUp;
    case USAGE_VOL_DOWN:
        return Action::BrightnessDown;
    case USAGE_NEXT_TRACK:
        return Action::ScreenNext;
    case USAGE_PREV_TRACK:
        return Action::ScreenPrev;
    case USAGE_PLAY_PAUSE:
    case USAGE_MENU_PICK:
        return Action::Select;
    default:
        return Action::None;
    }
}

const char *action_name(Action a) {
    switch (a) {
    case Action::BrightnessUp:
        return "brightness-up";
    case Action::BrightnessDown:
        return "brightness-down";
    case Action::ScreenNext:
        return "screen-next";
    case Action::ScreenPrev:
        return "screen-prev";
    case Action::Select:
        return "select";
    default:
        return "none";
    }
}

// Bitmap bit order (byte 0), the de-facto layout used by ESP32-BLE-Keyboard
// style remote firmwares. Byte 1 (browser/launcher keys) is ignored.
static const uint16_t kBitmapUsages[8] = {
    USAGE_NEXT_TRACK,
    USAGE_PREV_TRACK,
    USAGE_STOP,
    USAGE_PLAY_PAUSE,
    USAGE_MUTE,
    USAGE_VOL_UP,
    USAGE_VOL_DOWN,
    0,  // bit7 unused
};

// Try the usage-array interpretation: consecutive LE16 words, every non-zero
// word must be a recognized usage. Returns the count on success, or SIZE_MAX
// (~0) when the report does not look like a usage array.
static size_t try_usage_array(const uint8_t *data, size_t len, uint16_t *usages, size_t max) {
    size_t n = 0;
    bool any_nonzero = false;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t u = (uint16_t)(data[i] | ((uint16_t)data[i + 1] << 8));
        if (u == 0) continue;  // released slot
        any_nonzero = true;
        if (!is_known_usage(u)) return (size_t)~0;  // not a usage array
        if (n < max) usages[n++] = u;
    }
    if (!any_nonzero) return 0;  // all-zero = key release
    return n;
}

size_t decode_consumer_report(const uint8_t *data, size_t len, uint16_t *usages, size_t max) {
    if (!data || len == 0 || !usages || max == 0) return 0;

    if (len >= 2) {
        size_t n = try_usage_array(data, len, usages, max);
        if (n != (size_t)~0) return n;
    }

    // Bitmap fallback: only sensible for short reports (1-2 bytes).
    if (len > 2) return 0;
    size_t n = 0;
    for (int bit = 0; bit < 8 && n < max; ++bit) {
        if ((data[0] >> bit) & 1) {
            uint16_t u = kBitmapUsages[bit];
            if (u) usages[n++] = u;
        }
    }
    return n;
}

size_t decode_actions(const uint8_t *data, size_t len, Action *out, size_t max) {
    if (!data || len == 0 || !out || max == 0) return 0;

    // Boot-keyboard report (8 bytes: modifiers, reserved, 6 keycodes): only
    // Enter (0x28) / KP Enter (0x58) are interesting — map to Select. Try
    // the consumer usage-array reading first so an 8-byte usage report
    // isn't misread as a keyboard report.
    uint16_t usages[8];
    size_t nu = decode_consumer_report(data, len, usages, 8);
    size_t n = 0;
    for (size_t i = 0; i < nu && n < max; ++i) {
        Action a = usage_to_action(usages[i]);
        if (a != Action::None) out[n++] = a;
    }
    if (n > 0 || nu > 0) return n;

    if (len == 8 && data[1] == 0x00) {
        for (size_t i = 2; i < 8 && n < max; ++i) {
            if (data[i] == 0x28 || data[i] == 0x58) {
                out[n++] = Action::Select;
                break;
            }
        }
    }
    return n;
}

}  // namespace hid_decode
