#include "ui_theme.h"

#include <string.h>

namespace ui {

// Default = night theme, "glass cockpit" language (Garmin GPSMAP school):
// cool near-black ground, bordered cells with a subtle vertical gradient for
// depth, semantic side colors. Field order must match struct Palette exactly.
//                bg        panel     panel_bot panel_edge badge     fg
//                fg_dim    accent    warn      alarm     good      port
//                starboard grid
Palette theme = {
    0x0a1018, 0x101b29, 0x0b1320, 0x1f2d3d, 0x16222f, 0xeef4fa, 0x8fa7bd, 0x4fc3f7,
    0xffb84d, 0xff5252, 0x36d399, 0xff5252, 0x36d399, 0x2a3a4c, 0xf2f6fb,
};

// Canonical name of the active palette; kept in lockstep by the use_*()
// setters so console/web/state endpoints can report the live theme.
static const char *s_theme_id = "night";

void use_night() {
    theme = {
        0x0a1018, 0x101b29, 0x0b1320, 0x1f2d3d, 0x16222f, 0xeef4fa, 0x8fa7bd, 0x4fc3f7,
        0xffb84d, 0xff5252, 0x36d399, 0xff5252, 0x36d399, 0x2a3a4c, 0xf2f6fb,
    };
    s_theme_id = "night";
}

void use_day() {
    // Sun-readable glass cockpit: bright panels with a faint gradient, dark
    // text, saturated semantic colors. Same chrome structure as night.
    theme = {
        0xeef1f4, 0xffffff, 0xeef2f6, 0xc7d2da, 0xe7edf2, 0x14211d, 0x5a6b78, 0x0277bd,
        0xb56f00, 0xc62828, 0x1b7a4b, 0xc62828, 0x1b7a4b, 0x9fb0bd, 0xdfe7ee,
    };
    s_theme_id = "day";
}

void use_high_contrast() {
    // Maximum legibility: pure black ground, white text/edges, saturated
    // primaries. Matches the web reference (midl/web/src/theme.ts
    // THEMES["high-contrast"] + WIDGETS_HC: accent #00d0ff, warn #ffd000,
    // alarm #ff3030, good #00ff88, ticks #888888, HUD band #ffffff).
    theme = {
        0x000000, 0x0a0a0a, 0x000000, 0xffffff, 0x141414, 0xffffff, 0xbdbdbd, 0x00d0ff,
        0xffd000, 0xff3030, 0x00ff88, 0xff3030, 0x00ff88, 0x888888, 0xffffff,
    };
    s_theme_id = "high-contrast";
}

void use_red_night() {
    // Night-vision skin (standard practice on marine nav displays): every
    // foreground in red/amber on near-black so the helm keeps dark
    // adaptation. No greens/blues anywhere — "good"/starboard fall back to
    // amber tones, alarm stays the brightest pure red.
    theme = {
        0x000000, 0x140302, 0x0a0100, 0x4a1410, 0x1f0705, 0xff4b3e, 0xa63527, 0xff7a29,
        0xffa028, 0xff1a1a, 0xe0921e, 0xff5252, 0xd9a441, 0x3c100c, 0xff8a75,
    };
    s_theme_id = "red-night";
}

void use_classic() {
    // Warm analog-instrument look: cream dial faces, charcoal ink, brass-gold
    // accent, oxblood/racing-green semantic colors. Light-theme structure
    // (panels brighter than ground) like use_day().
    theme = {
        0xe8e1cf, 0xf7f1e3, 0xe9e1cc, 0xb9ab8c, 0xe4dbc4, 0x2e2a24, 0x6f6454, 0x9a741b,
        0xb56f00, 0x9e2f28, 0x3e6b41, 0x9e2f28, 0x3e6b41, 0x9b8d70, 0xefe8d6,
    };
    s_theme_id = "classic";
}

namespace {
struct NamedTheme {
    const char *name;
    void (*apply)();
};
// Single source for name -> palette. day/night/high-contrast mirror the MIDL
// catalog; red-night/classic are firmware-extra (see ui_theme.h).
constexpr NamedTheme k_themes[] = {
    {"night", use_night},         {"day", use_day},         {"high-contrast", use_high_contrast},
    {"red-night", use_red_night}, {"classic", use_classic},
};
}  // namespace

bool theme_known(const char *name) {
    if (!name || !*name) return false;
    for (const auto &t : k_themes)
        if (strcmp(t.name, name) == 0) return true;
    return false;
}

bool use_theme(const char *name) {
    if (!name) return false;
    for (const auto &t : k_themes) {
        if (strcmp(t.name, name) == 0) {
            t.apply();
            return true;
        }
    }
    return false;
}

const char *theme_id() {
    return s_theme_id;
}

void style_screen(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, c(theme.bg), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
}

void style_panel(lv_obj_t *o, uint32_t accent) {
    // Glass-cockpit cell: subtle top->bottom vertical gradient for depth,
    // 10px radius, hairline border (accent-tinted when a widget is in an
    // alert/active state). All metrics come from ui::chrome (no inline magic).
    lv_obj_set_style_bg_color(o, c(theme.panel), 0);
    lv_obj_set_style_bg_grad_color(o, c(theme.panel_bot), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, c(accent ? accent : theme.panel_edge), 0);
    lv_obj_set_style_border_opa(o, accent ? LV_OPA_80 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, chrome::panel_border, 0);
    lv_obj_set_style_radius(o, chrome::panel_radius, 0);
    lv_obj_set_style_pad_all(o, chrome::panel_pad, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
}

void style_caption(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(o, c(theme.fg_dim), 0);
}

void style_value(lv_obj_t *o, const lv_font_t *font, uint32_t color) {
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, c(color), 0);
}

lv_obj_t *panel_accent(lv_obj_t *parent, uint32_t color) {
    lv_obj_t *a = lv_obj_create(parent);
    lv_obj_set_size(a, 4, 28);
    lv_obj_set_pos(a, 0, 0);
    lv_obj_set_style_bg_color(a, c(color), 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(a, 0, 0);
    lv_obj_set_style_radius(a, 2, 0);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

}  // namespace ui
