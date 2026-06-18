#include <string.h>
#include <unity.h>
#include "ota_pass.h"
void setUp() {
}
void tearDown() {
}
static void test_nvs_wins() {
    TEST_ASSERT_EQUAL_STRING("rt", ota_pass_effective("rt", "compiled"));
}
static void test_empty_nvs_falls_back() {
    TEST_ASSERT_EQUAL_STRING("compiled", ota_pass_effective("", "compiled"));
}
static void test_null_nvs_falls_back() {
    TEST_ASSERT_EQUAL_STRING("compiled", ota_pass_effective(nullptr, "compiled"));
}
static void test_both_empty() {
    TEST_ASSERT_EQUAL_STRING("", ota_pass_effective("", ""));
}
int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_nvs_wins);
    RUN_TEST(test_empty_nvs_falls_back);
    RUN_TEST(test_null_nvs_falls_back);
    RUN_TEST(test_both_empty);
    return UNITY_END();
}
