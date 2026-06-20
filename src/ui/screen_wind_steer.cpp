#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_compass.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "ui_fonts.h"
#include "signalk.h"
#include "net.h"
#include "autopilot.h"
#include "app_events.h"
#include "board_pins.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Wind-steering screen. Deliberately IDENTICAL in layout to the autopilot
// (steering) screen -- same top bar, semicircular compass, big centre readout,
// XTE strip and square data tiles -- so the two read as a matched pair. The only
// differences are the data fields (wind metrics) and the "wind display": the
// compass carries a red NO-GO sector and green TARGET sectors (the laylines /
// optimal sailing angles) plus an amber wind bug, driven by the SignalK polar
// beat/gybe angles. Headings: the compass is heading-up; a true bearing H shows
// at screen angle (H - heading), LVGL angle 270 + (H - heading).

namespace ui::wind_steer {

static lv_obj_t *s_root = nullptr;
static ui::Compass s_cp;
static ui::XteStrip s_xte;
static lv_obj_t *lbl_mode, *lbl_hdg_value, *lbl_sub;
static lv_obj_t *lbl_onstby;
static lv_obj_t *nogo, *target_a, *target_b;
static lv_obj_t *tile_aws, *tile_awa, *tile_tws, *tile_twa;

static void put_state(const char *state) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/state", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "\"%s\"", state);
    app::post_net(cmd, 50);
    net::logf("[wind] state -> %s queued", state);
}

static void on_onstby_short(lv_event_t *) {
    sk::Data d;
    sk::copyData(d);
    bool engaged = d.apState[0] && strcmp(d.apState, "standby") != 0;
    put_state(engaged ? "standby" : "wind");  // engage in WIND mode
}

static void on_home(lv_event_t *) {
    ui::show_by_id("dashboard");
}

// ---- course-adjust + tack/gybe ---------------------------------------------
// autopilot::adjust_heading_deg() rejects |delta| > 90 (InvalidPayload), so the
// tack/gybe turns (which can exceed a quadrant) are clamped to one PUT's worth;
// a second tap completes a large mirror. All run on the LVGL task with no large
// stack scratch.
static int clamp_delta(int d) {
    if (d > 90) return 90;
    if (d < -90) return -90;
    return d;
}

static void on_nudge(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    ::autopilot::adjust_heading_deg(delta);
}

// TACK: mirror the boat across the wind onto the opposite tack. Reflecting the
// true wind angle through 0 means turning by -2*TWA (TWA in [-180,180]).
static void on_tack(lv_event_t *) {
    sk::Data d;
    sk::copyData(d);
    if (isnan(d.twa)) return;
    double twa_deg = d.twa * 180.0 / M_PI;
    while (twa_deg > 180.0)
        twa_deg -= 360.0;
    while (twa_deg < -180.0)
        twa_deg += 360.0;
    ::autopilot::adjust_heading_deg(clamp_delta((int)lround(-2.0 * twa_deg)));
}

// GYBE: mirror the boat across the stern (downwind), reflecting |TWA| through
// 180. Turn the short way through dead-downwind onto the opposite tack.
static void on_gybe(lv_event_t *) {
    sk::Data d;
    sk::copyData(d);
    if (isnan(d.twa)) return;
    double twa_deg = d.twa * 180.0 / M_PI;
    while (twa_deg > 180.0)
        twa_deg -= 360.0;
    while (twa_deg < -180.0)
        twa_deg += 360.0;
    int delta =
        twa_deg >= 0 ? (int)lround(2.0 * (180.0 - twa_deg)) : (int)lround(-2.0 * (180.0 + twa_deg));
    ::autopilot::adjust_heading_deg(clamp_delta(delta));
}

// Small course-adjust / maneuver button overlaid along the bottom strip.
static lv_obj_t *course_btn(lv_obj_t *parent, const char *txt, int w, lv_event_cb_t cb,
                            void *user_data) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 36);
    lv_obj_set_style_bg_color(b, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(theme.panel_bot), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(b, ui::chrome::panel_border, 0);
    lv_obj_set_style_radius(b, ui::chrome::panel_radius, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(theme.fg), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, user_data);
    return b;
}

// Build the course-adjust + maneuver row: [-10][-1][+1][+10] [TACK] [GYBE].
// Sits in the reserved bottom strip (the numeric tiles are raised to make room),
// centered across the full panel width.
static void build_course_row(lv_obj_t *parent, int y) {
    int gap = 6;
    int nudge_w = 50;
    int man_w = 66;
    int total = 4 * nudge_w + 2 * man_w + 5 * gap;
    int x = (LCD_W - total) / 2;
    static const struct {
        const char *txt;
        int delta;
    } nudges[4] = {{"-10", -10}, {"-1", -1}, {"+1", +1}, {"+10", +10}};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *b =
            course_btn(parent, nudges[i].txt, nudge_w, on_nudge, (void *)(intptr_t)nudges[i].delta);
        lv_obj_set_pos(b, x, y);
        x += nudge_w + gap;
    }
    lv_obj_t *bt = course_btn(parent, "TACK", man_w, on_tack, nullptr);
    lv_obj_set_pos(bt, x, y);
    x += man_w + gap;
    lv_obj_t *bg = course_btn(parent, "GYBE", man_w, on_gybe, nullptr);
    lv_obj_set_pos(bg, x, y);
}

// Top-bar chip identical to the autopilot screen.
static lv_obj_t *chip(lv_obj_t *parent, const char *txt, int w, lv_event_cb_t short_cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_style_bg_color(b, lv_color_hex(theme.panel), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(theme.panel_bot), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(theme.panel_edge), 0);
    lv_obj_set_style_border_width(b, ui::chrome::panel_border, 0);
    lv_obj_set_style_radius(b, ui::chrome::panel_radius, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(theme.fg), 0);
    lv_obj_center(l);
    if (short_cb) lv_obj_add_event_cb(b, short_cb, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_set_user_data(b, l);
    return b;
}

// A colored sector arc on the compass band (no-go / target). Angles set live.
static lv_obj_t *make_sector(lv_obj_t *parent, int cx, int cy, int radius, int width,
                             uint32_t color) {
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    int d = radius * 2;
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - radius, cy - radius);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_bg_angles(a, 250, 290);
    lv_arc_set_angles(a, 250, 290);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    return a;
}

lv_obj_t *build(lv_obj_t *parent) {
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, LCD_W, LCD_H);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(theme.bg), 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_EVENT_BUBBLE);

    bool wide = (LCD_W * 100 > LCD_H * 125);

    // --- top bar: ON/STBY (wind mode), mode badge, HOME --- (same as AP)
    lv_obj_t *btn_onstby = chip(s_root, "ON", 110, on_onstby_short);
    lv_obj_align(btn_onstby, LV_ALIGN_TOP_LEFT, 8, 8);
    lbl_onstby = (lv_obj_t *)lv_obj_get_user_data(btn_onstby);

    lv_obj_t *btn_h = chip(s_root, "HOME", 110, on_home);
    lv_obj_align(btn_h, LV_ALIGN_TOP_RIGHT, -8, 8);

    lbl_mode = lv_label_create(s_root);
    lv_label_set_text(lbl_mode, "WIND");
    lv_obj_set_style_text_font(lbl_mode, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_mode, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_mode, LV_ALIGN_TOP_MID, 0, 14);

    // --- compass --- (identical geometry to the autopilot screen)
    int top_bar_h = 56;
    int cw = wide ? (LCD_H - 36) : (LCD_W - 32);
    int cox = (LCD_W - cw) / 2;
    int coy = top_bar_h;
    s_cp = ui::build_compass(s_root, cox, coy, cw);
    int scx = cox + s_cp.cx;
    int scy = coy + s_cp.cy;

    // Wind display: LIGHT marks on the outer band -- a faint red no-go tint and
    // crisp slim green layline marks -- so the white band and scale numbers stay
    // clear. Raise the ticks / numbers / lubber back above them.
    int band_r = s_cp.r - 10;  // outer edge of the white band
    nogo = make_sector(s_cp.root, s_cp.cx, s_cp.cy, band_r, 14, theme.alarm);
    lv_obj_set_style_arc_opa(nogo, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(nogo, LV_OPA_40, LV_PART_INDICATOR);
    target_a = make_sector(s_cp.root, s_cp.cx, s_cp.cy, band_r, 16, theme.good);
    target_b = make_sector(s_cp.root, s_cp.cx, s_cp.cy, band_r, 16, theme.good);
    lv_obj_move_foreground(s_cp.scale);
    for (int i = 0; i < 12; ++i)
        if (s_cp.nums[i]) lv_obj_move_foreground(s_cp.nums[i]);
    lv_obj_move_foreground(s_cp.lubber);

    // Steering marker ring -> HDG/COG/CTS + amber TWD bug, heading-up. Built on
    // s_root (not the dial-sized compass root, which would clip it) at the
    // compass's screen-space centre; r - 42 lands the glyphs on the white band
    // just inside the rail (same as the AP HUD). occlude_lower hides them in the
    // bottom half. Driven by marker_ring_update in refresh().
    ui::MarkerSpec ws_markers[4] = {
        {NAN, ui::Glyph::Triangle, true, theme.accent},  // HDG (under lubber at offset 0)
        {NAN, ui::Glyph::Triangle, false, theme.good},   // COG
        {NAN, ui::Glyph::Diamond, true, theme.alarm},    // CTS
        {NAN, ui::Glyph::Diamond, true, theme.warn},     // TWD wind bug (amber)
    };
    s_cp.markers = ui::build_marker_ring(s_root, scx, scy, s_cp.r - ui::kSemiMarkerInset,
                                         ws_markers, 4, /*occlude_lower=*/true);

    // --- centre readouts (over the dial face) --- HDG + wind sub-line ---
    lv_obj_t *cap = lv_label_create(s_root);
    lv_label_set_text(cap, "HDG");
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cap, 120);
    lv_obj_set_pos(cap, scx - 60, scy - s_cp.r / 2 - 30);

    lbl_hdg_value = lv_label_create(s_root);
    lv_label_set_text(lbl_hdg_value, "--\xC2\xB0");
    lv_obj_set_style_text_font(lbl_hdg_value, &font_xl_64, 0);
    lv_obj_set_style_text_color(lbl_hdg_value, lv_color_hex(theme.fg), 0);
    lv_obj_set_style_text_align(lbl_hdg_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_hdg_value, 240);
    lv_obj_set_pos(lbl_hdg_value, scx - 120, scy - s_cp.r / 2 + 2);

    lbl_sub = lv_label_create(s_root);
    lv_label_set_text(lbl_sub, "TWA --\xC2\xB0 | TWD --\xC2\xB0");
    lv_obj_set_style_text_font(lbl_sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_sub, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_sub, LCD_W);
    lv_obj_set_pos(lbl_sub, scx - LCD_W / 2, scy - 4);

    // --- XTE strip below the compass --- (same as AP)
    int xte_h = 44;
    int xte_y = coy + s_cp.h + 4;
    int xte_x = wide ? cox : 16;
    int xte_w = wide ? cw : (LCD_W - 32);
    s_xte = ui::build_xte_strip(s_root, xte_x, xte_y, xte_w, xte_h);

    // --- numeric tiles (square, same as AP) --- wind data fields ---
    // Reserve a bottom strip for the course-adjust / tack-gybe row so it never
    // covers the tiles. On the square panel the four tiles shrink to clear it; on
    // the wide panel the side columns already leave the bottom-center free.
    const int course_h = 36;
    const int course_pad = 6;
    const lv_font_t *tv = &lv_font_montserrat_38;
    int gap = 8;
    if (!wide) {
        int n = 4;
        int sq = (LCD_W - gap * (n + 1)) / n;
        int ty = LCD_H - sq - gap - course_h - course_pad;  // raised to clear the row
        tile_aws = ui::numeric_tile(s_root, gap, ty, sq, sq, "AWS", "kn", tv, theme.warn);
        tile_awa = ui::numeric_tile(s_root, gap * 2 + sq, ty, sq, sq, "AWA", "", tv, theme.fg);
        tile_tws =
            ui::numeric_tile(s_root, gap * 3 + sq * 2, ty, sq, sq, "TWS", "kn", tv, theme.fg);
        tile_twa = ui::numeric_tile(s_root, gap * 4 + sq * 3, ty, sq, sq, "TWA", "", tv, theme.fg);
    } else {
        int sq = cox - gap * 2;
        int ty = (LCD_H - 2 * sq - gap) / 2;
        int rx = cox + cw + gap;
        tile_aws = ui::numeric_tile(s_root, gap, ty, sq, sq, "AWS", "kn", tv, theme.warn);
        tile_awa = ui::numeric_tile(s_root, gap, ty + sq + gap, sq, sq, "AWA", "", tv, theme.fg);
        tile_tws = ui::numeric_tile(s_root, rx, ty, sq, sq, "TWS", "kn", tv, theme.fg);
        tile_twa = ui::numeric_tile(s_root, rx, ty + sq + gap, sq, sq, "TWA", "", tv, theme.fg);
    }

    // Course-adjust + tack/gybe row along the bottom strip.
    build_course_row(s_root, LCD_H - course_h - course_pad);

    return s_root;
}

// ---- refresh ----

static char s_last_mode[16] = {(char)0xFF};
static uint32_t s_last_mode_color = 0xFFFFFFFF;
static char s_last_onstby[8] = {(char)0xFF};
static char s_last_hdg[16] = {(char)0xFF};
static char s_last_sub[40] = {(char)0xFF};
static char s_last_aws[12] = {(char)0xFF};
static char s_last_awa[12] = {(char)0xFF};
static char s_last_tws[12] = {(char)0xFF};
static char s_last_twa[12] = {(char)0xFF};
static int16_t s_last_scale_rot = INT16_MIN;
static int s_last_xte_x = INT16_MIN;
static char s_last_xte_txt[16] = {(char)0xFF};

static double norm360(double d) {
    while (d < 0)
        d += 360;
    while (d >= 360)
        d -= 360;
    return d;
}
static int16_t deg_to_lvgl(double deg) {
    int16_t r = (int16_t)(lround(deg) * 10);
    while (r < 0)
        r += 3600;
    while (r >= 3600)
        r -= 3600;
    return r;
}
// Place a sector spanning screen angles [t0,t1] (deg from top, +cw) in LVGL.
static void set_sector(lv_obj_t *a, double t0, double t1) {
    int s = (int)norm360(270 + t0);
    int e = (int)norm360(270 + t1);
    lv_arc_set_bg_angles(a, s, e);
    lv_arc_set_angles(a, s, e);
}

void refresh() {
    sk::Data d;
    sk::copyData(d);
    char buf[64];

    bool engaged = d.apState[0] && strcmp(d.apState, "standby") != 0;
    if (d.apState[0]) {
        char up[16];
        size_t i = 0;
        for (; d.apState[i] && i < sizeof(up) - 1; ++i)
            up[i] = toupper(d.apState[i]);
        up[i] = 0;
        set_text_if_changed(lbl_mode, s_last_mode, sizeof(s_last_mode), up);
    } else {
        set_text_if_changed(lbl_mode, s_last_mode, sizeof(s_last_mode), "WIND");
    }
    set_text_color_if_changed(lbl_mode, &s_last_mode_color, engaged ? theme.good : theme.fg_dim);
    set_text_if_changed(lbl_onstby, s_last_onstby, sizeof(s_last_onstby), engaged ? "STBY" : "ON");

    // Heading: big value + rotate the compass like the autopilot screen.
    double hdg = isnan(d.headingTrue) ? NAN : rad_to_deg_pos(d.headingTrue);
    if (!isnan(hdg)) {
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", hdg);
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), buf);
    } else {
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), "--\xC2\xB0");
    }
    double label_hdg = isnan(hdg) ? 0.0 : hdg;
    int16_t scale_rot = deg_to_lvgl(-label_hdg);
    if (scale_rot != s_last_scale_rot) {
        s_last_scale_rot = scale_rot;
        lv_obj_set_style_transform_rotation(s_cp.scale, scale_rot, 0);
        ui::compass_layout_labels(s_cp, label_hdg);
    }

    // Wind angles + TWD.
    double twa_raw = isnan(d.twa) ? NAN : rad_to_deg_pos(d.twa);
    double twa_mag = NAN;
    bool stbd = true;
    if (!isnan(twa_raw)) {
        stbd = twa_raw <= 180.0;
        twa_mag = stbd ? twa_raw : 360.0 - twa_raw;
    }
    double twd = (!isnan(hdg) && !isnan(twa_raw)) ? norm360(hdg + twa_raw) : NAN;

    // Sub-line: TWA + TWD.
    char twas[12], twds[12];
    if (!isnan(twa_mag))
        snprintf(twas, sizeof(twas), "%.0f\xC2\xB0%c", twa_mag, stbd ? 'S' : 'P');
    else
        snprintf(twas, sizeof(twas), "--\xC2\xB0");
    if (!isnan(twd))
        snprintf(twds, sizeof(twds), "%03.0f\xC2\xB0", twd);
    else
        snprintf(twds, sizeof(twds), "--\xC2\xB0");
    snprintf(buf, sizeof(buf), "TWA %s  |  TWD %s", twas, twds);
    set_text_if_changed(lbl_sub, s_last_sub, sizeof(s_last_sub), buf);

    // --- wind display: no-go + target sectors + bug, heading-relative ---
    bool up = isnan(twa_mag) ? true : (twa_mag <= 90.0);
    double beat = isnan(d.beatAngle) ? NAN : rad_to_deg_pos(d.beatAngle);
    double gybe = isnan(d.gybeAngle) ? NAN : rad_to_deg_pos(d.gybeAngle);
    double opt = up ? (isnan(beat) ? 45.0 : beat) : (isnan(gybe) ? 150.0 : gybe);
    double tws_kn = isnan(d.tws) ? NAN : mps_to_kn(d.tws);
    double tol = isnan(tws_kn) ? 10.0 : 16.0 - 0.6 * tws_kn;  // TWS-responsive band
    if (tol < 4.0) tol = 4.0;
    if (tol > 16.0) tol = 16.0;

    if (!isnan(twd) && !isnan(hdg)) {
        double twd_rel = norm360(twd - hdg);                    // wind dir from the bow
        double badc = up ? twd_rel : norm360(twd_rel + 180.0);  // heading to avoid
        double half = up ? opt : (180.0 - opt);                 // no-go half width
        if (half < 4) half = 4;
        if (half > 88) half = 88;
        // Map screen angle to -180..180 for the arc helper.
        double c = badc > 180.0 ? badc - 360.0 : badc;
        set_sector(nogo, c - half, c + half);
        set_sector(target_a, c + half, c + half + tol);
        set_sector(target_b, c - half - tol, c - half);
        lv_obj_clear_flag(nogo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(target_a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(target_b, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(nogo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(target_a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(target_b, LV_OBJ_FLAG_HIDDEN);
    }

    // Steering marker ring -> HDG/COG/CTS + amber TWD bug (heading-up). Bearings
    // match the AP HUD exactly; marker_ring_update hides any NaN marker.
    double hdg_b = hdg;
    double cog_b = isnan(d.cogTrue) ? NAN : rad_to_deg_pos(d.cogTrue);
    double cts_b = isnan(d.cts) ? NAN : rad_to_deg_pos(d.cts);
    ui::MarkerSpec steer_live[4] = {
        {hdg_b, ui::Glyph::Triangle, true, theme.accent},
        {cog_b, ui::Glyph::Triangle, false, theme.good},
        {cts_b, ui::Glyph::Diamond, true, theme.alarm},
        {twd, ui::Glyph::Diamond, true, theme.warn},
    };
    double wind_ref = isnan(hdg) ? 0.0 : hdg;
    ui::marker_ring_update(s_cp.markers, steer_live, 4, wind_ref);

    // XTE needle (same as AP).
    if (!isnan(d.xte)) {
        double nm = d.xte / 1852.0;
        if (nm > 1.0) nm = 1.0;
        if (nm < -1.0) nm = -1.0;
        int nx = s_xte.center_x + (int)(nm * s_xte.half_px) - 1;
        if (nx != s_last_xte_x) {
            s_last_xte_x = nx;
            lv_obj_set_x(s_xte.needle, nx);
        }
    }
    // Numeric XTE readout (meters + P/S, over-range clamped). Same as AP.
    if (s_xte.value) {
        char xbuf[16];
        ui::format_xte(d.xte, xbuf, sizeof(xbuf));
        set_text_if_changed(s_xte.value, s_last_xte_txt, sizeof(s_last_xte_txt), xbuf);
    }

    // Tiles: AWS / AWA / TWS / TWA.
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.aws));
        set_text_if_changed(tile_aws, s_last_aws, sizeof(s_last_aws), buf);
    } else {
        set_text_if_changed(tile_aws, s_last_aws, sizeof(s_last_aws), "--");
    }
    if (!isnan(d.awa)) {
        double a = rad_to_deg_pos(d.awa);
        bool s = a <= 180.0;
        snprintf(buf, sizeof(buf), "%.0f%c", s ? a : 360.0 - a, s ? 'S' : 'P');
        set_text_if_changed(tile_awa, s_last_awa, sizeof(s_last_awa), buf);
    } else {
        set_text_if_changed(tile_awa, s_last_awa, sizeof(s_last_awa), "--");
    }
    if (!isnan(d.tws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.tws));
        set_text_if_changed(tile_tws, s_last_tws, sizeof(s_last_tws), buf);
    } else {
        set_text_if_changed(tile_tws, s_last_tws, sizeof(s_last_tws), "--");
    }
    if (!isnan(twa_mag)) {
        snprintf(buf, sizeof(buf), "%.0f%c", twa_mag, stbd ? 'S' : 'P');
        set_text_if_changed(tile_twa, s_last_twa, sizeof(s_last_twa), buf);
    } else {
        set_text_if_changed(tile_twa, s_last_twa, sizeof(s_last_twa), "--");
    }
}

}  // namespace ui::wind_steer
