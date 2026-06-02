#include <unity.h>

#include <string>

#include "manager_endpoint.h"

using manager_endpoint::base_url;
using manager_endpoint::discovery_method_from_string;
using manager_endpoint::discovery_method_to_string;
using manager_endpoint::DiscoveryMethod;
using manager_endpoint::Endpoint;
using manager_endpoint::parse_url;
using manager_endpoint::redacted_secret_state;

void setUp(void) {
}

void tearDown(void) {
}

static void test_parse_http_url_with_plugin_path() {
    Endpoint ep;
    TEST_ASSERT_TRUE(parse_url("http://signalk.local:3000/plugins/espdisp-manager", ep));
    TEST_ASSERT_EQUAL_STRING("http", ep.scheme.c_str());
    TEST_ASSERT_FALSE(ep.tls);
    TEST_ASSERT_EQUAL_STRING("signalk.local", ep.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(3000, ep.port);
    TEST_ASSERT_EQUAL_STRING("/plugins/espdisp-manager", ep.base_path.c_str());
    TEST_ASSERT_EQUAL_STRING("http://signalk.local:3000/plugins/espdisp-manager",
                             base_url(ep).c_str());
}

static void test_parse_defaults_to_http_without_scheme() {
    Endpoint ep;
    TEST_ASSERT_TRUE(parse_url("192.168.4.10:3000", ep));
    TEST_ASSERT_EQUAL_STRING("http", ep.scheme.c_str());
    TEST_ASSERT_FALSE(ep.tls);
    TEST_ASSERT_EQUAL_STRING("192.168.4.10", ep.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(3000, ep.port);
    TEST_ASSERT_EQUAL_STRING("", ep.base_path.c_str());
    TEST_ASSERT_EQUAL_STRING("http://192.168.4.10:3000", base_url(ep).c_str());
}

static void test_parse_https_defaults_port_and_tls() {
    Endpoint ep;
    TEST_ASSERT_TRUE(parse_url("https://boat.example/signalk-espdisp-manager/", ep));
    TEST_ASSERT_EQUAL_STRING("https", ep.scheme.c_str());
    TEST_ASSERT_TRUE(ep.tls);
    TEST_ASSERT_EQUAL_STRING("boat.example", ep.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(443, ep.port);
    TEST_ASSERT_EQUAL_STRING("/signalk-espdisp-manager", ep.base_path.c_str());
}

static void test_rejects_bad_urls() {
    Endpoint ep;
    std::string err;
    TEST_ASSERT_FALSE(parse_url("", ep, &err));
    TEST_ASSERT_EQUAL_STRING("empty url", err.c_str());
    TEST_ASSERT_FALSE(parse_url("ftp://host", ep, &err));
    TEST_ASSERT_EQUAL_STRING("unsupported scheme", err.c_str());
    TEST_ASSERT_FALSE(parse_url("http://host:nope", ep, &err));
    TEST_ASSERT_EQUAL_STRING("invalid port", err.c_str());
    TEST_ASSERT_FALSE(parse_url("http://:3000", ep, &err));
    TEST_ASSERT_EQUAL_STRING("missing host", err.c_str());
}

static void test_discovery_method_roundtrip() {
    TEST_ASSERT_EQUAL_STRING("none", discovery_method_to_string(DiscoveryMethod::None));
    TEST_ASSERT_EQUAL_STRING("stored", discovery_method_to_string(DiscoveryMethod::Stored));
    TEST_ASSERT_EQUAL_STRING("manual", discovery_method_to_string(DiscoveryMethod::Manual));
    TEST_ASSERT_EQUAL_STRING("mdns", discovery_method_to_string(DiscoveryMethod::Mdns));
    TEST_ASSERT_EQUAL_STRING("signalk-well-known",
                             discovery_method_to_string(DiscoveryMethod::SignalKWellKnown));
    TEST_ASSERT_EQUAL(DiscoveryMethod::Manual, discovery_method_from_string("manual"));
    TEST_ASSERT_EQUAL(DiscoveryMethod::Mdns, discovery_method_from_string("mdns"));
    TEST_ASSERT_EQUAL(DiscoveryMethod::None, discovery_method_from_string("bad"));
}

static void test_redacted_secret_state_does_not_expose_value() {
    TEST_ASSERT_EQUAL_STRING("none", redacted_secret_state("").c_str());
    TEST_ASSERT_EQUAL_STRING("set(len=11)", redacted_secret_state("supersecret").c_str());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_http_url_with_plugin_path);
    RUN_TEST(test_parse_defaults_to_http_without_scheme);
    RUN_TEST(test_parse_https_defaults_port_and_tls);
    RUN_TEST(test_rejects_bad_urls);
    RUN_TEST(test_discovery_method_roundtrip);
    RUN_TEST(test_redacted_secret_state_does_not_expose_value);
    return UNITY_END();
}
