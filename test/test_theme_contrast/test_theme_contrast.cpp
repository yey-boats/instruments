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

static constexpr double kMinContrast = 4.5;

struct Pair {
    const char *what;  // "<ink> on <ground>"
    uint32_t Palette::*ink;
    uint32_t Palette::*ground;
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
    {"mob_fg on mob_bg", &Palette::mob_fg, &Palette::mob_bg},
    // Engaged autopilot pill / chip / badge: good fill, ground-colour ink.
    {"bg on good", &Palette::bg, &Palette::good},
};

static void test_contrast_math_reference_points() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 21.0, contrast(0x000000, 0xffffff));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.0, contrast(0x777777, 0x777777));
    // The #67 regression: sun's old panel_edge button with fg ink.
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 1.21, contrast(0x000000, 0x1a1a1a));
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
            if (cr < kMinContrast) {
                snprintf(msg, sizeof(msg), "%s: %s %06x/%06x = %.2f:1 < %.1f:1", t.name, pair.what,
                         (unsigned)(p.*pair.ink), (unsigned)(p.*pair.ground), cr, kMinContrast);
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
        uint32_t r = (c[i] >> 16) & 0xFF, g = (c[i] >> 8) & 0xFF, b = c[i] & 0xFF;
        char msg[64];
        snprintf(msg, sizeof(msg), "red-night token %u = %06x", (unsigned)i, (unsigned)c[i]);
        TEST_ASSERT_TRUE_MESSAGE(r >= g && r >= b, msg);
        TEST_ASSERT_TRUE_MESSAGE(b <= 0x60, msg);
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
    RUN_TEST(test_red_night_stays_red);
    RUN_TEST(test_names_unique_and_complete);
    return UNITY_END();
}
