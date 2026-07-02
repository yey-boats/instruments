#include <unity.h>

#include <cstring>

// Pure logic under test. Neither TU is in the native env's build_src_filter;
// include them directly (same pattern as test_boat_data / source_signalk).
#include "../../src/hid_consumer_decode.cpp"
#include "../../src/wifi_scan_json.cpp"

using namespace hid_decode;

void setUp(void) {
}
void tearDown(void) {
}

// ---- usage -> action mapping -------------------------------------------

static void test_usage_mapping() {
    TEST_ASSERT_EQUAL((int)Action::BrightnessUp, (int)usage_to_action(USAGE_VOL_UP));
    TEST_ASSERT_EQUAL((int)Action::BrightnessDown, (int)usage_to_action(USAGE_VOL_DOWN));
    TEST_ASSERT_EQUAL((int)Action::ScreenNext, (int)usage_to_action(USAGE_NEXT_TRACK));
    TEST_ASSERT_EQUAL((int)Action::ScreenPrev, (int)usage_to_action(USAGE_PREV_TRACK));
    TEST_ASSERT_EQUAL((int)Action::Select, (int)usage_to_action(USAGE_PLAY_PAUSE));
    TEST_ASSERT_EQUAL((int)Action::Select, (int)usage_to_action(USAGE_MENU_PICK));
    TEST_ASSERT_EQUAL((int)Action::None, (int)usage_to_action(0x00E2));  // mute unmapped
    TEST_ASSERT_EQUAL((int)Action::None, (int)usage_to_action(0x1234));
}

// ---- 16-bit usage-array reports ------------------------------------------

static void test_usage_array_vol_up() {
    const uint8_t rpt[] = {0xE9, 0x00};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(rpt, sizeof(rpt), a, 4));
    TEST_ASSERT_EQUAL((int)Action::BrightnessUp, (int)a[0]);
}

static void test_usage_array_next_prev() {
    const uint8_t next[] = {0xB5, 0x00};
    const uint8_t prev[] = {0xB6, 0x00};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(next, 2, a, 4));
    TEST_ASSERT_EQUAL((int)Action::ScreenNext, (int)a[0]);
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(prev, 2, a, 4));
    TEST_ASSERT_EQUAL((int)Action::ScreenPrev, (int)a[0]);
}

static void test_usage_array_release_is_empty() {
    const uint8_t rpt[] = {0x00, 0x00};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(0, decode_actions(rpt, sizeof(rpt), a, 4));
}

static void test_usage_array_multi_slot() {
    // Two-slot report: play/pause pressed, second slot released.
    const uint8_t rpt[] = {0xCD, 0x00, 0x00, 0x00};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(rpt, sizeof(rpt), a, 4));
    TEST_ASSERT_EQUAL((int)Action::Select, (int)a[0]);
}

static void test_usage_array_menu_pick() {
    // Menu-Pick (0x0041) is the dedicated "select" consumer usage; the
    // firmware routes Select to app::CommandType::Select (ble_hid_host.cpp),
    // which dismisses a MIDL zoom overlay or toggles to/from the dashboard.
    const uint8_t rpt[] = {0x41, 0x00};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(rpt, sizeof(rpt), a, 4));
    TEST_ASSERT_EQUAL((int)Action::Select, (int)a[0]);
}

static void test_usage_array_unmapped_known_usage() {
    // Mute is recognized (usage-array mode) but maps to no action.
    const uint8_t rpt[] = {0xE2, 0x00};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(0, decode_actions(rpt, sizeof(rpt), a, 4));
}

// ---- bitmap-style reports -------------------------------------------------

static void test_bitmap_vol_up() {
    // bit5 = Vol+ in the ESP32-BLE-Keyboard bit order. 0x20 is not a known
    // usage code, so the decoder must fall through to bitmap mode.
    const uint8_t rpt[] = {0x20, 0x00};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(rpt, sizeof(rpt), a, 4));
    TEST_ASSERT_EQUAL((int)Action::BrightnessUp, (int)a[0]);
}

static void test_bitmap_single_byte() {
    const uint8_t vol_down[] = {0x40};  // bit6
    const uint8_t next[] = {0x01};      // bit0
    const uint8_t play[] = {0x08};      // bit3
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(vol_down, 1, a, 4));
    TEST_ASSERT_EQUAL((int)Action::BrightnessDown, (int)a[0]);
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(next, 1, a, 4));
    TEST_ASSERT_EQUAL((int)Action::ScreenNext, (int)a[0]);
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(play, 1, a, 4));
    TEST_ASSERT_EQUAL((int)Action::Select, (int)a[0]);
}

static void test_bitmap_multiple_bits() {
    // Vol+ and NextTrack chord (bit5 | bit0).
    const uint8_t rpt[] = {0x21};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(2, decode_actions(rpt, 1, a, 4));
    TEST_ASSERT_EQUAL((int)Action::ScreenNext, (int)a[0]);
    TEST_ASSERT_EQUAL((int)Action::BrightnessUp, (int)a[1]);
}

static void test_long_unknown_report_ignored() {
    // 4 bytes that neither parse as known usages nor qualify for bitmap
    // (len > 2) — must decode to nothing, not garbage actions.
    const uint8_t rpt[] = {0x13, 0x37, 0x13, 0x37};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(0, decode_actions(rpt, sizeof(rpt), a, 4));
}

// ---- boot keyboard reports -----------------------------------------------

static void test_keyboard_enter() {
    const uint8_t rpt[] = {0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00};
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(1, decode_actions(rpt, sizeof(rpt), a, 4));
    TEST_ASSERT_EQUAL((int)Action::Select, (int)a[0]);
}

static void test_keyboard_other_key_ignored() {
    const uint8_t rpt[] = {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};  // 'a'
    Action a[4];
    TEST_ASSERT_EQUAL_UINT32(0, decode_actions(rpt, sizeof(rpt), a, 4));
}

// ---- action names -----------------------------------------------------

static void test_action_name_select() {
    TEST_ASSERT_EQUAL_STRING("select", action_name(Action::Select));
}

// ---- wifi scan JSON --------------------------------------------------------

static void test_scan_json_empty() {
    char out[64];
    size_t n = wifi_scan_json::to_json(nullptr, 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("[]", out);
    TEST_ASSERT_EQUAL_UINT32(2, n);
}

static void test_scan_json_sorted_by_rssi() {
    wifi_scan_json::Ap aps[3] = {};
    strcpy(aps[0].ssid, "weak");
    aps[0].rssi = -80;
    aps[0].sec = false;
    strcpy(aps[1].ssid, "strong");
    aps[1].rssi = -40;
    aps[1].sec = true;
    strcpy(aps[2].ssid, "mid");
    aps[2].rssi = -60;
    aps[2].sec = true;
    char out[512];
    size_t n = wifi_scan_json::to_json(aps, 3, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("[{\"ssid\":\"strong\",\"rssi\":-40,\"sec\":true},"
                             "{\"ssid\":\"mid\",\"rssi\":-60,\"sec\":true},"
                             "{\"ssid\":\"weak\",\"rssi\":-80,\"sec\":false}]",
                             out);
}

static void test_scan_json_escapes_ssid() {
    wifi_scan_json::Ap aps[1] = {};
    strcpy(aps[0].ssid, "my \"boat\" \\ wifi");
    aps[0].rssi = -50;
    aps[0].sec = true;
    char out[256];
    wifi_scan_json::to_json(aps, 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("[{\"ssid\":\"my \\\"boat\\\" \\\\ wifi\",\"rssi\":-50,\"sec\":true}]",
                             out);
}

static void test_scan_json_truncates_to_strongest() {
    // 20 APs with long SSIDs cannot all fit in 512 bytes; the output must
    // stay valid JSON, keep the strongest APs, and stay under the cap.
    wifi_scan_json::Ap aps[20] = {};
    for (int i = 0; i < 20; ++i) {
        snprintf(aps[i].ssid, sizeof(aps[i].ssid), "network-with-a-long-name-%02d", i);
        aps[i].rssi = (int16_t)(-30 - i);  // index 0 strongest
        aps[i].sec = true;
    }
    char out[512];
    size_t n = wifi_scan_json::to_json(aps, 20, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(n < 512);
    TEST_ASSERT_EQUAL('[', out[0]);
    TEST_ASSERT_EQUAL(']', out[n - 1]);
    // Strongest AP present, weakest dropped.
    TEST_ASSERT_NOT_NULL(strstr(out, "network-with-a-long-name-00"));
    TEST_ASSERT_NULL(strstr(out, "network-with-a-long-name-19"));
    // Entries stay whole (no torn objects): count braces.
    int open = 0, close = 0;
    for (size_t i = 0; i < n; ++i) {
        if (out[i] == '{') open++;
        if (out[i] == '}') close++;
    }
    TEST_ASSERT_EQUAL(open, close);
    TEST_ASSERT_TRUE(open >= 5);
}

static void test_scan_json_skips_empty_ssid() {
    wifi_scan_json::Ap aps[2] = {};
    aps[0].ssid[0] = 0;  // hidden network
    aps[0].rssi = -10;
    strcpy(aps[1].ssid, "visible");
    aps[1].rssi = -50;
    char out[128];
    wifi_scan_json::to_json(aps, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("[{\"ssid\":\"visible\",\"rssi\":-50,\"sec\":false}]", out);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_usage_mapping);
    RUN_TEST(test_usage_array_vol_up);
    RUN_TEST(test_usage_array_next_prev);
    RUN_TEST(test_usage_array_release_is_empty);
    RUN_TEST(test_usage_array_multi_slot);
    RUN_TEST(test_usage_array_menu_pick);
    RUN_TEST(test_usage_array_unmapped_known_usage);
    RUN_TEST(test_bitmap_vol_up);
    RUN_TEST(test_bitmap_single_byte);
    RUN_TEST(test_bitmap_multiple_bits);
    RUN_TEST(test_long_unknown_report_ignored);
    RUN_TEST(test_keyboard_enter);
    RUN_TEST(test_keyboard_other_key_ignored);
    RUN_TEST(test_action_name_select);
    RUN_TEST(test_scan_json_empty);
    RUN_TEST(test_scan_json_sorted_by_rssi);
    RUN_TEST(test_scan_json_escapes_ssid);
    RUN_TEST(test_scan_json_truncates_to_strongest);
    RUN_TEST(test_scan_json_skips_empty_ssid);
    return UNITY_END();
}
