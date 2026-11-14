#pragma once

// Pure palette data for the firmware skins (no LVGL): ui_theme.cpp copies one
// of these into the live ui::theme, and the native test_theme_contrast suite
// checks every skin's text/button token pairs against WCAG AA (4.5:1).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <stddef.h>
#include <stdint.h>

namespace ui {

struct Palette {
    uint32_t bg;          // screen background
    uint32_t panel;       // card / quadrant background (gradient top)
    uint32_t panel_bot;   // card gradient bottom (glass-cockpit depth)
    uint32_t panel_edge;  // panel border
    uint32_t badge;       // unit-badge background
    uint32_t fg;          // primary text
    uint32_t fg_dim;      // secondary text / units / labels
    uint32_t accent;      // hero metric color
    uint32_t warn;        // amber-ish accent
    uint32_t alarm;       // red / urgent
    uint32_t good;        // green / ok
    uint32_t port;        // red side (port wind)
    uint32_t starboard;   // green side (starboard wind)
    uint32_t grid;        // axis / compass tick color
    uint32_t arc_band;    // gauge band (compass rail, wind bezel)
    // MIDL colour tokens with no firmware chrome slot of their own (#45):
    // `tide` (current/tide vectors) and `stale` (aged data), values from the
    // matching midl/web/src/theme.ts palette.
    uint32_t tide;
    uint32_t stale;
    // Ink drawn ON arc_band: major ticks + numerals / minor ticks. Dark ink
    // for a light band; red-night's dark band takes light red ink.
    uint32_t band_ink;
    uint32_t band_ink_dim;
    // Buttons (#67): inactive fill + label ink, active/selected fill + ink.
    // Explicit tokens instead of panel_edge/fg + accent/literal, which broke
    // on skins whose border colour is close to their text colour (sun,
    // high-contrast).
    uint32_t button_bg;
    uint32_t button_fg;
    uint32_t button_active_bg;
    uint32_t button_active_fg;
    // Ink on an `alarm`-filled surface (alarm banner, SILENCE/REBOOT, alert
    // tile in alarm). Red-night uses black, not white, to keep dark adaptation.
    uint32_t alarm_fg;
    // MOB rescue overlay ground and its readout ink.
    uint32_t mob_bg;
    uint32_t mob_fg;
    // "MAN OVERBOARD" title on mob_bg (red, but bright enough for 4.5:1 on
    // the dark-red ground; `port`/`alarm` are too dark on the light skins).
    uint32_t mob_title;
    // True-wind ("T") marker on the wind dial; the apparent ("A") marker uses
    // `warn`. Both include a 20 px letter, so they are held to text contrast.
    uint32_t wind_true;
};

// Positional initialisers below list exactly this many colours, in the field
// order above.
constexpr size_t kPaletteColours = 28;
static_assert(sizeof(Palette) == kPaletteColours * sizeof(uint32_t),
              "update every Palette initialiser");

namespace palettes {

// clang-format off

// Order: bg, panel, panel_bot, panel_edge, badge, fg, fg_dim, accent, warn,
//        alarm, good, port, starboard, grid, arc_band, tide, stale,
//        band_ink, band_ink_dim,
//        button_bg, button_fg, button_active_bg, button_active_fg,
//        alarm_fg, mob_bg, mob_fg, mob_title, wind_true.

// Dark blue "glass cockpit" (Garmin GPSMAP school), the default: cool
// near-black ground, bordered cells with a subtle vertical gradient for
// depth, semantic side colors.
constexpr Palette night = {
    0x0a1018, 0x101b29, 0x0b1320, 0x1f2d3d, 0x16222f, 0xeef4fa, 0x8fa7bd,
    0x4fc3f7, 0xffb84d, 0xff5252, 0x36d399, 0xff5252, 0x36d399, 0x2a3a4c,
    0xf2f6fb, 0x288cff, 0x4a5666, 0x16222f, 0x5a6b78,
    0x1f2d3d, 0xeef4fa, 0x4fc3f7, 0x05101c, 0x05101c, 0x1a0000, 0xffffff,
    0xff5252, 0x2bd4e8,
};

// Sun-readable glass cockpit: bright panels with a faint gradient, dark
// text, saturated semantic colors. Same chrome structure as night.
constexpr Palette day = {
    0xeef1f4, 0xffffff, 0xeef2f6, 0xc7d2da, 0xe7edf2, 0x14211d, 0x5a6b78,
    0x0266a8, 0x8a5500, 0xc62828, 0x1b7a4b, 0xc62828, 0x1b7a4b, 0x9fb0bd,
    0xdfe7ee, 0x0a5fc0, 0x9aa7b4, 0x16222f, 0x52616d,
    0xc7d2da, 0x14211d, 0x0266a8, 0xffffff, 0xffffff, 0x1a0000, 0xffffff,
    0xff5252, 0x00707f,
};

// Maximum legibility: pure black ground, white text/edges, saturated
// primaries. Matches the web reference (midl/web/src/theme.ts
// THEMES["high-contrast"] + WIDGETS_HC: accent #00d0ff, warn #ffd000,
// alarm #ff3030, good #00ff88, ticks #888888, HUD band #ffffff). Buttons get
// a dark-grey fill: the white panel_edge fill made white labels vanish.
constexpr Palette high_contrast = {
    0x000000, 0x0a0a0a, 0x000000, 0xffffff, 0x141414, 0xffffff, 0xbdbdbd,
    0x00d0ff, 0xffd000, 0xff3030, 0x00ff88, 0xff3030, 0x00ff88, 0x888888,
    0xffffff, 0x3aa0ff, 0x666666, 0x16222f, 0x5a6b78,
    0x333333, 0xffffff, 0x00d0ff, 0x000000, 0x000000, 0x1a0000, 0xffffff,
    0xff3030, 0x2bd4e8,
};

// Night-vision skin (standard practice on marine nav displays): every
// foreground in red/amber on near-black so the helm keeps dark adaptation.
// No greens/blues/whites anywhere: "good"/starboard fall back to amber tones,
// alarm stays the brightest pure red (with black ink on it), MOB readouts are
// red-orange. The gauge band is a dark red with red ink on it: the old salmon
// band (0xff8a75) was the brightest thing on a night screen.
constexpr Palette red_night = {
    0x000000, 0x140302, 0x0a0100, 0x4a1410, 0x1f0705, 0xff4b3e, 0xd84a3a,
    0xff7a29, 0xffa028, 0xff1a1a, 0xe0921e, 0xff5252, 0xd9a441, 0x3c100c,
    0x2a0805, 0xc03a30, 0x5c211c, 0xff6a50, 0xe0503c,
    0x4a1410, 0xff4b3e, 0xff7a29, 0x000000, 0x000000, 0x1a0000, 0xff7a29,
    0xff5252, 0xff4b3e,
};

// Warm analog-instrument look: cream dial faces, charcoal ink, brass-gold
// accent, oxblood/racing-green semantic colors. Light-theme structure
// (panels brighter than ground) like day.
constexpr Palette classic = {
    0xe8e1cf, 0xf7f1e3, 0xe9e1cc, 0xb9ab8c, 0xe4dbc4, 0x2e2a24, 0x685d4e,
    0x7a5a10, 0x8a5500, 0x9e2f28, 0x3e6b41, 0x9e2f28, 0x3e6b41, 0x9b8d70,
    0xefe8d6, 0x35566b, 0xa89878, 0x16222f, 0x5a6b78,
    0xb9ab8c, 0x2e2a24, 0x9a741b, 0x000000, 0xffffff, 0x1a0000, 0xffffff,
    0xff5252, 0x2f6170,
};

// Direct sunlight on the IPS panel: white ground and flat white panels
// (no gradient), black text, thin black panel borders, darker saturated
// semantic colours that hold up against glare. Buttons are light grey with
// black ink; the selected one is the accent blue with white ink.
constexpr Palette sun = {
    0xffffff, 0xffffff, 0xffffff, 0x1a1a1a, 0xeeeeee, 0x000000, 0x3a3a3a,
    0x0050a0, 0xa05a00, 0xc00000, 0x007a3d, 0xc00000, 0x007a3d, 0x444444,
    0xe6e6e6, 0x0a5fc0, 0x8a8a8a, 0x000000, 0x555555,
    0xcccccc, 0x000000, 0x0050a0, 0xffffff, 0xffffff, 0x1a0000, 0xffffff,
    0xff5252, 0x006c80,
};

// clang-format on

struct Named {
    const char *name;
    const Palette *palette;
};

// Single source for name -> palette. day/night/high-contrast mirror the MIDL
// catalog; red-night/classic/sun are firmware-extra (see ui_theme.h).
constexpr Named all[] = {
    {"night", &night},         {"day", &day},         {"high-contrast", &high_contrast},
    {"red-night", &red_night}, {"classic", &classic}, {"sun", &sun},
};

}  // namespace palettes
}  // namespace ui
