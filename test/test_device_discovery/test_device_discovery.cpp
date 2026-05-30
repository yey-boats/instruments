#include <ArduinoJson.h>
#include <unity.h>

#include "device_discovery.h"

void setUp(void) {
}
void tearDown(void) {
}

static void test_announcement_contract() {
    device_discovery::Info info;
    info.device_id = "espdisp-test";
    info.board_id = "native_fake";
    info.firmware_name = "espdisp";
    info.firmware_version = "0.5.0-test";
    info.ip = "192.168.1.50";
    info.port = 80;
    info.display_width = 480;
    info.display_height = 480;
    info.web_auth_required = true;

    JsonDocument doc;
    device_discovery::build_announcement(doc, info);

    TEST_ASSERT_EQUAL_STRING(device_discovery::DEVICE_ANNOUNCE_PROTOCOL, doc["protocol"]);
    TEST_ASSERT_EQUAL_STRING("espdisp-test", doc["deviceId"]);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", doc["address"]);
    TEST_ASSERT_EQUAL_UINT16(80, doc["port"]);
    TEST_ASSERT_TRUE(doc["authRequired"]);
    TEST_ASSERT_EQUAL_STRING("espdisp-test", doc["device"]["id"]);
    TEST_ASSERT_EQUAL_STRING("native_fake", doc["device"]["board"]);
    TEST_ASSERT_EQUAL_STRING("espdisp", doc["firmware"]["name"]);
    TEST_ASSERT_EQUAL_STRING("0.5.0-test", doc["firmware"]["version"]);
    TEST_ASSERT_EQUAL_UINT16(480, doc["display"]["width"]);
    TEST_ASSERT_EQUAL_UINT16(480, doc["display"]["height"]);
}

static void test_constants_match_discovery_spec() {
    TEST_ASSERT_EQUAL_UINT16(34301, device_discovery::DEVICE_ANNOUNCE_PORT);
    TEST_ASSERT_EQUAL_STRING("espdisp", device_discovery::MDNS_SERVICE);
    TEST_ASSERT_EQUAL_STRING("1", device_discovery::MDNS_PROTO);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_announcement_contract);
    RUN_TEST(test_constants_match_discovery_spec);
    return UNITY_END();
}
