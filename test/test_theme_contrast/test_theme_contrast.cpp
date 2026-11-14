// Host tests for include/ui_palette.h (instruments#67): every skin's text and
// button token pairs must reach WCAG 2.x AA contrast (4.5:1). The sun skin
// shipped with panel_edge-filled buttons inked in fg at 1.21:1; this suite
// makes such a regression fail CI instead of a sailor's eyes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "ui_palette.h"

using ui::Palette;

void setUp() {
}
void tearDown() {
}

// WCAG 2.x relative luminance of a 0xRRGGBB colour.
static double luminance(uint32_t rgb) {
    auto lin = [](uint32_t c8) {
        double c = c8 / 255.0;
        return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin((rgb >> 16) & 0xFF) + 0.7152 * lin((rgb >> 8) & 0xFF) +
           0.0722 * lin(rgb & 0xFF);
}

static double contrast(uint32_t a, uint32_t b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) {
        double t = la;
        la = lb;
        lb = t;
    }
    return (la + 0.05) / (lb + 0.05);
}

// WCAG 2.x SC 1.4.3: 4.5:1 for text. SC 1.4.11 asks 3:1 for non-text
// graphics (arrows, shafts, rings), which carry no letters to read.
static constexpr double kText = 4.5;
static constexpr double kGraphic = 3.0;

struct Pair {
    const char *what;  // "<ink> on <ground>"
    uint32_t Palette::*ink;
    uint32_t Palette::*ground;
    double min = kText;
};

// Text / ink drawn on a filled ground, as the painters pair them.
static const Pair kPairs[] = {
    {"fg on bg", &Palette::fg, &Palette::bg},
    {"fg on panel", &Palette::fg, &Palette::panel},
    {"fg on panel_bot", &Palette::fg, &Palette::panel_bot},
    {"fg_dim on bg", &Palette::fg_dim, &Palette::bg},
    {"fg_dim on panel", &Palette::fg_dim, &Palette::panel},
    {"fg_dim on panel_bot", &Palette::fg_dim, &Palette::panel_bot},
    {"button_fg on button_bg", &Palette::button_fg, &Palette::button_bg},
    {"button_active_fg on button_active_bg", &Palette::button_active_fg,
     &Palette::button_active_bg},
    {"alarm_fg on alarm", &Palette::alarm_fg, &Palette::alarm},
    // DEMO badge uses the same alarm / alarm_fg pair.
    // Every text token on the MOB overlay ground: distance, bearing, return,
    // elapsed (mob_fg) and the title (mob_title). The HOLD TO CLEAR label is
    // button_fg on its own button_bg fill, covered above.
    {"mob_fg on mob_bg", &Palette::mob_fg, &Palette::mob_bg},
    {"mob_title on mob_bg", &Palette::mob_title, &Palette::mob_bg},
    // Engaged autopilot pill / chip / badge: good fill, ground-colour ink.
    {"bg on good", &Palette::bg, &Palette::good},
    // Gauge band: cardinal (20 px) and intercardinal (14 px) labels and ticks.
    {"band_ink on arc_band", &Palette::band_ink, &Palette::arc_band},
    {"band_ink_dim on arc_band", &Palette::band_ink_dim, &Palette::arc_band},
    // accent / warn: hero numbers but also small captions (AWS secondary,
    // knob target/title), so text contrast on every ground.
    {"accent on bg", &Palette::accent, &Palette::bg},
    {"accent on panel", &Palette::accent, &Palette::panel},
    {"accent on panel_bot", &Palette::accent, &Palette::panel_bot},
    {"warn on bg", &Palette::warn, &Palette::bg},
    {"warn on panel", &Palette::warn, &Palette::panel},
    {"warn on panel_bot", &Palette::warn, &Palette::panel_bot},
    // Wind dial T marker: arrow + 20 px letter, drawn on the screen ground.
    {"wind_true on bg", &Palette::wind_true, &Palette::bg},
    {"wind_true on panel", &Palette::wind_true, &Palette::panel},
    // Tide arrow / zero ring: a pure graphic (no letters), so 3:1.
    {"tide on bg", &Palette::tide, &Palette::bg, kGraphic},
    {"tide on panel", &Palette::tide, &Palette::panel, kGraphic},
    {"tide on panel_bot", &Palette::tide, &Palette::panel_bot, kGraphic},
};

static void test_contrast_math_reference_points() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 21.0, contrast(0x000000, 0xffffff));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, contrast(0x777777, 0x777777));
    // The #67 regression: sun's old panel_edge button with fg ink.
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 1.21, contrast(0x000000, 0x1a1a1a));
}

// Red-night rule, shared by the test and its self-check below: red-dominant,
// blue low, and green low enough that nothing reads as white or yellow-white
// (amber 0xffa028 is the brightest allowed green).
static bool red_night_ok(uint32_t c) {
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return r >= g && r >= b && b <= 0x60 && g <= 0xb0;
}

static void test_red_night_rule_rejects_near_white() {
    TEST_ASSERT_FALSE(red_night_ok(0xffffff));
    TEST_ASSERT_FALSE(red_night_ok(0xffff60));  // passed the old rule
    TEST_ASSERT_FALSE(red_night_ok(0xffe0c0));
    TEST_ASSERT_FALSE(red_night_ok(0x288cff));  // the old blue tide literal
    TEST_ASSERT_TRUE(red_night_ok(0xff1a1a));
    TEST_ASSERT_TRUE(red_night_ok(0xffa028));
}

static void test_all_skins_meet_aa() {
    char msg[160];
    int failures = 0;
    for (const auto &t : ui::palettes::all) {
        const Palette &p = *t.palette;
        for (const auto &pair : kPairs) {
            double cr = contrast(p.*pair.ink, p.*pair.ground);
            printf("%-14s %-40s %06x/%06x %5.2f:1\n", t.name, pair.what, (unsigned)(p.*pair.ink),
                   (unsigned)(p.*pair.ground), cr);
            if (cr < pair.min) {
                snprintf(msg, sizeof(msg), "%s: %s %06x/%06x = %.2f:1 < %.1f:1", t.name, pair.what,
                         (unsigned)(p.*pair.ink), (unsigned)(p.*pair.ground), cr, pair.min);
                printf("FAIL %s\n", msg);
                ++failures;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, failures, "palette token pairs below WCAG AA");
}

// Red night keeps dark adaptation: no blue- or green-dominant, and no
// near-white, colour in any ink or fill token.
static void test_red_night_stays_red() {
    const Palette &p = ui::palettes::red_night;
    const uint32_t *c = reinterpret_cast<const uint32_t *>(&p);
    for (size_t i = 0; i < ui::kPaletteColours; ++i) {
        char msg[64];
        snprintf(msg, sizeof(msg), "red-night token %u = %06x", (unsigned)i, (unsigned)c[i]);
        TEST_ASSERT_TRUE_MESSAGE(red_night_ok(c[i]), msg);
    }
}

static void test_names_unique_and_complete() {
    size_t n = sizeof(ui::palettes::all) / sizeof(ui::palettes::all[0]);
    TEST_ASSERT_EQUAL_UINT(6, n);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            TEST_ASSERT_TRUE(strcmp(ui::palettes::all[i].name, ui::palettes::all[j].name) != 0);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_contrast_math_reference_points);
    RUN_TEST(test_all_skins_meet_aa);
    RUN_TEST(test_red_night_rule_rejects_near_white);
    RUN_TEST(test_red_night_stays_red);
    RUN_TEST(test_names_unique_and_complete);
    return UNITY_END();
}
