// Host tests for the firmware-update screen rules (instruments#106):
// include/ota_progress.h decides when the bar redraws (whole steps only, so
// redraws between flash writes stay few), the indeterminate animation and
// the byte counter text.

#include <string.h>
#include <unity.h>

#include "ota_progress.h"

using ota_progress::Pacer;

void setUp(void) {
}
void tearDown(void) {
}

static void test_percent() {
    TEST_ASSERT_EQUAL(-1, ota_progress::percent(10, 0));
    TEST_ASSERT_EQUAL(0, ota_progress::percent(0, 1000));
    TEST_ASSERT_EQUAL(49, ota_progress::percent(499, 1000));
    TEST_ASSERT_EQUAL(100, ota_progress::percent(1000, 1000));
    TEST_ASSERT_EQUAL(100, ota_progress::percent(2000, 1000));  // overrun clamps
    // 2 MB images: no 32-bit overflow in done * 100.
    TEST_ASSERT_EQUAL(50, ota_progress::percent(1u << 30, 1u << 31));
}

static void test_first_call_is_due_then_only_whole_steps() {
    Pacer p(4);
    const uint32_t total = 2000000;
    TEST_ASSERT_TRUE(p.due(0, total));
    TEST_ASSERT_EQUAL(0, p.shown_pct());
    TEST_ASSERT_FALSE(p.due(1024, total));
    TEST_ASSERT_FALSE(p.due(79999, total));  // 3.99 %
    TEST_ASSERT_TRUE(p.due(80000, total));   // 4 %
    TEST_ASSERT_EQUAL(4, p.shown_pct());
    TEST_ASSERT_FALSE(p.due(100000, total));
}

static void test_redraw_count_for_a_whole_image() {
    // 1 KB chunks over a 2 MB image (the manager pull loop): one redraw per
    // 4 % step plus the first, 26 in all, never one per chunk.
    Pacer p(4);
    const uint32_t total = 2u * 1024u * 1024u;
    int redraws = 0;
    for (uint32_t done = 0; done <= total; done += 1024)
        if (p.due(done, total)) ++redraws;
    TEST_ASSERT_EQUAL(26, redraws);
    TEST_ASSERT_EQUAL(100, p.shown_pct());
}

static void test_hundred_is_due_once_even_off_step() {
    Pacer p(7);  // 100 is not a multiple of 7
    TEST_ASSERT_TRUE(p.due(0, 100));
    TEST_ASSERT_TRUE(p.due(98, 100));
    TEST_ASSERT_EQUAL(98, p.shown_pct());
    TEST_ASSERT_TRUE(p.due(100, 100));
    TEST_ASSERT_EQUAL(100, p.shown_pct());
    TEST_ASSERT_FALSE(p.due(100, 100));
}

static void test_unknown_total_steps_by_bytes() {
    Pacer p(4, 1000);
    TEST_ASSERT_TRUE(p.due(0, 0));
    TEST_ASSERT_EQUAL(-1, p.shown_pct());
    TEST_ASSERT_EQUAL(0u, p.phase());
    TEST_ASSERT_FALSE(p.due(999, 0));
    TEST_ASSERT_TRUE(p.due(1000, 0));
    TEST_ASSERT_EQUAL(1u, p.phase());
    // A total showing up later switches to percent.
    TEST_ASSERT_TRUE(p.due(1500, 3000));
    TEST_ASSERT_EQUAL(48, p.shown_pct());
}

static void test_reset_makes_next_call_due() {
    Pacer p(4);
    TEST_ASSERT_TRUE(p.due(0, 100));
    TEST_ASSERT_FALSE(p.due(1, 100));
    p.reset();
    TEST_ASSERT_TRUE(p.due(1, 100));
}

static void test_bounce_stays_in_range_and_turns() {
    const int width = 100, seg = 20;
    int prev = -1;
    bool went_up = false, came_back = false;
    for (uint32_t ph = 0; ph < 40; ++ph) {
        int x = ota_progress::bounce_x(ph, width, seg, 10);
        TEST_ASSERT_TRUE(x >= 0);
        TEST_ASSERT_TRUE(x + seg <= width);
        if (prev >= 0 && x > prev) went_up = true;
        if (prev >= 0 && x < prev) came_back = true;
        prev = x;
    }
    TEST_ASSERT_TRUE(went_up);
    TEST_ASSERT_TRUE(came_back);
    TEST_ASSERT_EQUAL(0, ota_progress::bounce_x(0, width, seg, 10));
    TEST_ASSERT_EQUAL(80, ota_progress::bounce_x(8, width, seg, 10));  // far end
    TEST_ASSERT_EQUAL(0, ota_progress::bounce_x(5, 10, 20));           // segment wider than bar
}

static void test_format_bytes() {
    char b[32];
    ota_progress::format_bytes(b, sizeof(b), 1258291, 2097152);  // 1.2 / 2.0 MB
    TEST_ASSERT_EQUAL_STRING("1.2 / 2.0 MB", b);
    ota_progress::format_bytes(b, sizeof(b), 0, 0);
    TEST_ASSERT_EQUAL_STRING("0.0 MB", b);
    char tiny[4];
    ota_progress::format_bytes(tiny, sizeof(tiny), 1258291, 2097152);  // truncates, terminated
    TEST_ASSERT_EQUAL(3, (int)strlen(tiny));
}

static void test_default_mode_is_lit() {
    TEST_ASSERT_EQUAL((int)ota_progress::Mode::Lit, (int)ota_progress::kMode);
    TEST_ASSERT_EQUAL_STRING("lit", ota_progress::mode_name(ota_progress::kMode));
    TEST_ASSERT_EQUAL_STRING("dark", ota_progress::mode_name(ota_progress::Mode::Dark));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_percent);
    RUN_TEST(test_first_call_is_due_then_only_whole_steps);
    RUN_TEST(test_redraw_count_for_a_whole_image);
    RUN_TEST(test_hundred_is_due_once_even_off_step);
    RUN_TEST(test_unknown_total_steps_by_bytes);
    RUN_TEST(test_reset_makes_next_call_due);
    RUN_TEST(test_bounce_stays_in_range_and_turns);
    RUN_TEST(test_format_bytes);
    RUN_TEST(test_default_mode_is_lit);
    return UNITY_END();
}
