// instruments#112: the rules the tile painters apply (include/ui_paint_rules.h),
// exercised directly: bezel separators, button ink, compass captions, and the
// round value-only unit.

#include <math.h>
#include <unity.h>

#include "ui_paint_rules.h"

using namespace ui::paint;

void setUp(void) {
}
void tearDown(void) {
}

static double lin(uint32_t c) {
    double v = c / 255.0;
    return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
static double lum(uint32_t rgb) {
    return 0.2126 * lin((rgb >> 16) & 0xFF) + 0.7152 * lin((rgb >> 8) & 0xFF) +
           0.0722 * lin(rgb & 0xFF);
}
static double contrast(uint32_t a, uint32_t b) {
    double la = lum(a), lb = lum(b);
    return (fmax(la, lb) + 0.05) / (fmin(la, lb) + 0.05);
}

// #104 drew heavy ticks at 22/112/202/292: each cardinal got a heavy separator
// on one side and a light one on the other. Separators sit BETWEEN labels, so
// both flanks of every label must look the same.
static void test_bezel_separators_symmetric_around_every_label() {
    for (int k = 0; k < 8; ++k) {
        BezelTick before = bezel_tick((k + 7) % 8), after = bezel_tick(k);
        TEST_ASSERT_EQUAL_INT(before.len, after.len);
        TEST_ASSERT_EQUAL_INT(before.width, after.width);
        TEST_ASSERT_EQUAL(before.strong, after.strong);
    }
}

static void test_bezel_separators_sit_between_labels() {
    for (int k = 0; k < 8; ++k) {
        int a = bezel_tick(k).angle_deg % 45;
        TEST_ASSERT_TRUE(a >= 20 && a <= 25);  // never on a label (multiples of 45)
    }
}

// #104: white ink on a bright MIDL style.color (e.g. #ffd000) read ~1.4:1. The
// button painter now takes its ink from this rule (badge_ink).
static void test_button_ink_reads_on_bright_and_dark_fills() {
    const uint32_t dark = 0x05101c, light = 0xffffff;
    const uint32_t fills[] = {0xffd000, 0x00e5ff, 0x3ccf4e, 0xffffff, 0x1a3a5c, 0x7a5a10, 0x9e2f28};
    for (uint32_t f : fills) {
        uint32_t ink = ink_for_fill(f, dark, light);
        TEST_ASSERT_TRUE_MESSAGE(contrast(ink, f) >= 3.0, "ink must read on its fill");
    }
    TEST_ASSERT_EQUAL_HEX32(dark, ink_for_fill(0xffd000, dark, light));
}

// #111: the inner caption showed the element label unbounded: long names ran
// across the cardinals, "HDG / CTS" repeated the tile caption, and an unnamed
// compass showed its raw element id.
static void test_compass_caption_falls_back_and_stays_short() {
    char b[16];
    TEST_ASSERT_EQUAL_STRING("HDG", compass_caption(nullptr, "hdg", b, sizeof b, 8));
    TEST_ASSERT_EQUAL_STRING("HDG", compass_caption("", "hdg", b, sizeof b, 8));
    TEST_ASSERT_EQUAL_STRING("HDG", compass_caption("compass1", "compass1", b, sizeof b, 8));
    TEST_ASSERT_EQUAL_STRING("TWD", compass_caption("TWD", "twd", b, sizeof b, 8));
    TEST_ASSERT_EQUAL_STRING("HDG", compass_caption("HDG / CTS", "hdg", b, sizeof b, 8));
    TEST_ASSERT_EQUAL_STRING("COG", compass_caption("COG/SOG", "c", b, sizeof b, 8));
    TEST_ASSERT_EQUAL_STRING("HDG",
                             compass_caption("Apparent wind direction", "awd", b, sizeof b, 8));
    // legend rows hold ~3-4 glyphs next to "COG"/"CTS"
    TEST_ASSERT_EQUAL_STRING("TWD", compass_caption("TWD", "twd", b, sizeof b, 4));
    TEST_ASSERT_EQUAL_STRING("HDG", compass_caption("Heading", "h", b, sizeof b, 4));
}

// #111 forced unit "deg" on every compass; the round value-only fallback then
// drew "046 deg" and shrank the font. Degrees ride as no word unit there.
static void test_value_only_unit_skips_compass_degrees() {
    TEST_ASSERT_FALSE(value_only_word_unit(false, true, "deg", false, 0, 0));
    TEST_ASSERT_FALSE(value_only_word_unit(false, false, "deg", false, 0, 0));
    TEST_ASSERT_TRUE(value_only_word_unit(false, false, "kn", false, 0, 0));
    TEST_ASSERT_FALSE(value_only_word_unit(true, false, "%", false, 0, 100));
    TEST_ASSERT_FALSE(value_only_word_unit(false, false, "", false, 0, 0));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_bezel_separators_symmetric_around_every_label);
    RUN_TEST(test_bezel_separators_sit_between_labels);
    RUN_TEST(test_button_ink_reads_on_bright_and_dark_fills);
    RUN_TEST(test_compass_caption_falls_back_and_stays_short);
    RUN_TEST(test_value_only_unit_skips_compass_degrees);
    return UNITY_END();
}
