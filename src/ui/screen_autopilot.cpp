#include "screens.h"
#include "ui_screens.h"
#include "ui_theme.h"
#include "ui_compass.h"
#include "ui_data.h"
#include "ui_dirty.h"
#include "ui_fonts.h"
#include "signalk.h"
#include "net.h"
#include "board_pins.h"
#include "app_events.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Reference glass-cockpit autopilot HUD. A semicircular heading compass with a
// rotating scale, a fixed red lubber, and a HDG/COG/CTS + amber AP-target marker
// ring orbiting the rim; a big HDG value
// with a COG/SOG sub-line inside the arc; a cross-track-error strip; and a row
// of numeric tiles (DEPTH / SPEED / AWS / AWA). Controls are touch + the
// external network knob:
//   ON / STBY button   -> engage <-> standby (short), mode picker (long press)
//   tap dial port/stbd -> adjust target heading -1 / +1 (short), -10 / +10 (long)
// All PUTs route through the net worker via app::post_net (sk::putValue is too
// slow for the UI handler). Modes map to the SignalK autopilot backend:
//   STANDBY -> "standby"  AUTO -> "auto"  WIND -> "wind"  ROUTE -> "route"

namespace ui::autopilot {

static lv_obj_t *s_root = nullptr;
static ui::Compass s_cp;
static ui::XteStrip s_xte;
static lv_obj_t *lbl_mode, *lbl_hdg_value, *lbl_cogsog;
static lv_obj_t *btn_onstby, *lbl_onstby;
static lv_obj_t *tile_depth, *tile_speed, *tile_aws, *tile_awa;

// Mode picker overlay (hidden until ON/STBY long-press).
static lv_obj_t *mode_modal = nullptr;
static const char *kModeName[4] = {"AUTO", "WIND", "ROUTE", "STANDBY"};
static const char *kModeState[4] = {"auto", "wind", "route", "standby"};

static double s_target_local = NAN;         // local pending target heading (rad)
static const char *s_last_engage = "auto";  // mode to re-engage from STBY

static void put_heading(double rad) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/target/headingTrue", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "%.4f", rad);
    app::post_net(cmd, 50);
    net::logf("[ap] target -> %.0f\xC2\xB0 queued", rad * 180.0 / M_PI);
}

static void put_state(const char *state) {
    app::Command cmd;
    cmd.type = app::CommandType::SignalKPut;
    strncpy(cmd.a, "steering/autopilot/state", sizeof(cmd.a) - 1);
    snprintf(cmd.b, sizeof(cmd.b), "\"%s\"", state);
    app::post_net(cmd, 50);
    net::logf("[ap] state -> %s queued", state);
}

static void adjust_heading(int delta_deg) {
    double target = s_target_local;
    if (isnan(target)) {
        sk::Data d_snap;
        sk::copyData(d_snap);
        target = d_snap.apTargetHdg;
        if (isnan(target)) target = d_snap.headingTrue;
    }
    if (isnan(target)) target = 0;
    target += delta_deg * M_PI / 180.0;
    while (target < 0)
        target += 2 * M_PI;
    while (target >= 2 * M_PI)
        target -= 2 * M_PI;
    s_target_local = target;
    put_heading(target);
}

// ---- input handlers --------------------------------------------------------

static void on_nudge(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    adjust_heading(delta);
}

static void show_mode_modal(bool show) {
    if (!mode_modal) return;
    if (show)
        lv_obj_clear_flag(mode_modal, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(mode_modal, LV_OBJ_FLAG_HIDDEN);
}

static void on_mode_pick(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 3) return;
    if (idx != 3) {
        s_last_engage = kModeState[idx];
        s_target_local = NAN;  // entering a steering mode clears local nudge
    }
    put_state(kModeState[idx]);
    show_mode_modal(false);
}

static void on_onstby_short(lv_event_t *e) {
    (void)e;
    sk::Data d;
    sk::copyData(d);
    bool engaged = d.apState[0] && strcmp(d.apState, "standby") != 0;
    put_state(engaged ? "standby" : s_last_engage);
}

static void on_onstby_long(lv_event_t *e) {
    (void)e;
    show_mode_modal(true);
}

static void on_home(lv_event_t *e) {
    (void)e;
    ui::show_by_id("dashboard");
}

static void on_modal_bg(lv_event_t *e) {
    (void)e;
    show_mode_modal(false);
}

// ---- small builders --------------------------------------------------------

static lv_obj_t *chip(lv_obj_t *parent, const char *txt, int w, lv_event_cb_t short_cb,
                      lv_event_cb_t long_cb) {
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
    if (long_cb) lv_obj_add_event_cb(b, long_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_set_user_data(b, l);  // stash the label for state-driven text
    return b;
}

// Invisible tap target over one half of the dial: short = +/-1, long = +/-10.
static void dial_tap_zone(lv_obj_t *parent, int x, int y, int w, int h, int step) {
    lv_obj_t *z = lv_obj_create(parent);
    lv_obj_set_size(z, w, h);
    lv_obj_set_pos(z, x, y);
    lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(z, 0, 0);
    lv_obj_set_style_radius(z, 0, 0);
    lv_obj_set_style_pad_all(z, 0, 0);
    lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(z, on_nudge, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)step);
    lv_obj_add_event_cb(z, on_nudge, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(step * 10));
}

static void build_mode_modal(lv_obj_t *parent) {
    mode_modal = lv_obj_create(parent);
    lv_obj_set_size(mode_modal, LCD_W, LCD_H);
    lv_obj_set_pos(mode_modal, 0, 0);
    lv_obj_set_style_bg_color(mode_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mode_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(mode_modal, 0, 0);
    lv_obj_set_style_radius(mode_modal, 0, 0);
    lv_obj_clear_flag(mode_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mode_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mode_modal, on_modal_bg, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(mode_modal);
    int cw = LCD_W < 360 ? LCD_W - 40 : 280;
    lv_obj_set_size(card, cw, 280);
    lv_obj_center(card);
    style_panel(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "MODE");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(theme.fg_dim), 0);

    for (int i = 0; i < 4; ++i) {
        lv_obj_t *b = lv_button_create(card);
        lv_obj_set_size(b, cw - 32, 44);
        lv_obj_set_style_bg_color(b, lv_color_hex(i == 3 ? theme.panel_edge : theme.accent), 0);
        lv_obj_set_style_radius(b, ui::chrome::panel_radius, 0);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, kModeName[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(i == 3 ? theme.fg : 0x05101c), 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, on_mode_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_add_flag(mode_modal, LV_OBJ_FLAG_HIDDEN);
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

    // --- top bar: ON/STBY, mode badge, HOME ---
    btn_onstby = chip(s_root, "ON", 110, on_onstby_short, on_onstby_long);
    lv_obj_align(btn_onstby, LV_ALIGN_TOP_LEFT, 8, 8);
    lbl_onstby = (lv_obj_t *)lv_obj_get_user_data(btn_onstby);

    lv_obj_t *btn_h = chip(s_root, "HOME", 110, on_home, nullptr);
    lv_obj_align(btn_h, LV_ALIGN_TOP_RIGHT, -8, 8);

    lbl_mode = lv_label_create(s_root);
    lv_label_set_text(lbl_mode, "STANDBY");
    lv_obj_set_style_text_font(lbl_mode, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_mode, lv_color_hex(theme.fg_dim), 0);
    lv_obj_align(lbl_mode, LV_ALIGN_TOP_MID, 0, 14);

    // --- compass --- (wider on the roomy wide panels; square is width-bound)
    int top_bar_h = 56;
    int cw = wide ? (LCD_H - 36) : (LCD_W - 32);
    int cox = (LCD_W - cw) / 2;
    int coy = top_bar_h;
    s_cp = ui::build_compass(s_root, cox, coy, cw);
    int scx = cox + s_cp.cx;
    int scy = coy + s_cp.cy;

    // --- center readouts (over the dial face) ---
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

    lbl_cogsog = lv_label_create(s_root);
    lv_label_set_text(lbl_cogsog, "COG --\xC2\xB0 | SOG -- kn");
    lv_obj_set_style_text_font(lbl_cogsog, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_cogsog, lv_color_hex(theme.fg_dim), 0);
    lv_obj_set_style_text_align(lbl_cogsog, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl_cogsog, LCD_W);
    lv_obj_set_pos(lbl_cogsog, scx - LCD_W / 2, scy - 4);

    // --- HDG/COG/CTS + AP-target marker ring ---
    // Built on s_root (the full screen), NOT on the compass root: the compass
    // root is sized exactly to the dial and would clip any glyph orbiting near
    // its top/sides. Centered at the compass's screen-space center (scx, scy).
    // A holder glyph orbits ~16px OUTSIDE the radius passed here (make_holder's
    // MARGIN), so we pass r - 42 to land the glyphs on the white band just inside
    // the green rail (at ~r - 26) — clear of the top bar above the dial AND the
    // inset degree labels (LABEL_INSET = 44). occlude_lower hides the bottom half
    // (it would overlap the XTE strip / tiles). Built AFTER the center readouts so
    // it draws on top. glyph/filled/color are baked here; bearings fill in refresh.
    ui::MarkerSpec ap_markers[4] = {
        {NAN, ui::Glyph::Triangle, true, theme.accent},  // HDG (under the red lubber at offset 0)
        {NAN, ui::Glyph::Triangle, false, theme.good},   // COG
        {NAN, ui::Glyph::Diamond, true, theme.alarm},    // CTS
        {NAN, ui::Glyph::Diamond, true, theme.warn},     // AP target (amber bug), hidden until set
    };
    s_cp.markers = ui::build_marker_ring(s_root, scx, scy, s_cp.r - ui::kSemiMarkerInset,
                                         ap_markers, 4, /*occlude_lower=*/true);

    // --- dial tap zones (left = port/-, right = stbd/+) ---
    int dz_y = coy + 20;
    int dz_h = s_cp.r - 10;
    dial_tap_zone(s_root, cox + 6, dz_y, s_cp.r - 12, dz_h, -1);
    dial_tap_zone(s_root, scx + 6, dz_y, s_cp.r - 12, dz_h, +1);

    // --- XTE strip below the compass (placed by the compass's real height) ---
    int xte_h = 44;
    int xte_y = coy + s_cp.h + 4;
    int xte_x = wide ? cox : 16;
    int xte_w = wide ? cw : (LCD_W - 32);
    s_xte = ui::build_xte_strip(s_root, xte_x, xte_y, xte_w, xte_h);

    // --- numeric tiles (exactly square, anchored clear of the XTE strip) ---
    const lv_font_t *tv = &lv_font_montserrat_38;
    int gap = 8;
    if (!wide) {
        int n = 4;
        int sq = (LCD_W - gap * (n + 1)) / n;  // square side
        int ty = LCD_H - sq - gap;             // bottom row, below the XTE strip
        tile_depth = ui::numeric_tile(s_root, gap, ty, sq, sq, "DEPTH", "m", tv, theme.fg);
        tile_speed =
            ui::numeric_tile(s_root, gap * 2 + sq, ty, sq, sq, "SPEED", "kn", tv, theme.fg);
        tile_aws =
            ui::numeric_tile(s_root, gap * 3 + sq * 2, ty, sq, sq, "AWS", "kn", tv, theme.warn);
        tile_awa = ui::numeric_tile(s_root, gap * 4 + sq * 3, ty, sq, sq, "AWA", "", tv, theme.fg);
    } else {
        int sq = cox - gap * 2;               // square side = side-column width
        int ty = (LCD_H - 2 * sq - gap) / 2;  // two squares stacked, vertically centred
        int rx = cox + cw + gap;
        tile_depth = ui::numeric_tile(s_root, gap, ty, sq, sq, "DEPTH", "m", tv, theme.fg);
        tile_speed =
            ui::numeric_tile(s_root, gap, ty + sq + gap, sq, sq, "SPEED", "kn", tv, theme.fg);
        tile_aws = ui::numeric_tile(s_root, rx, ty, sq, sq, "AWS", "kn", tv, theme.warn);
        tile_awa = ui::numeric_tile(s_root, rx, ty + sq + gap, sq, sq, "AWA", "", tv, theme.fg);
    }

    build_mode_modal(s_root);
    return s_root;
}

// ---- refresh ---------------------------------------------------------------

static char s_last_mode[16] = {(char)0xFF};
static uint32_t s_last_mode_color = 0xFFFFFFFF;
static char s_last_onstby[8] = {(char)0xFF};
static char s_last_hdg[16] = {(char)0xFF};
static char s_last_cogsog[48] = {(char)0xFF};
static char s_last_depth[12] = {(char)0xFF};
static char s_last_speed[12] = {(char)0xFF};
static char s_last_aws[12] = {(char)0xFF};
static char s_last_awa[12] = {(char)0xFF};
static int16_t s_last_scale_rot = INT16_MIN;
static int s_last_xte_x = INT16_MIN;

static int16_t deg_to_lvgl(double deg) {
    int16_t r = (int16_t)(lround(deg) * 10);
    while (r < 0)
        r += 3600;
    while (r >= 3600)
        r -= 3600;
    return r;
}

void refresh() {
    sk::Data d_snap;
    sk::copyData(d_snap);
    const sk::Data &d = d_snap;
    char buf[64];

    bool engaged = d.apState[0] && strcmp(d.apState, "standby") != 0;

    // Mode badge.
    if (d.apState[0]) {
        char up[16];
        size_t i = 0;
        for (; d.apState[i] && i < sizeof(up) - 1; ++i)
            up[i] = toupper(d.apState[i]);
        up[i] = 0;
        set_text_if_changed(lbl_mode, s_last_mode, sizeof(s_last_mode), up);
    } else {
        set_text_if_changed(lbl_mode, s_last_mode, sizeof(s_last_mode), "OFFLINE");
    }
    set_text_color_if_changed(lbl_mode, &s_last_mode_color, engaged ? theme.good : theme.fg_dim);
    set_text_if_changed(lbl_onstby, s_last_onstby, sizeof(s_last_onstby), engaged ? "STBY" : "ON");

    // Heading: big value + rotate the compass tick ring by -heading and
    // reposition the upright degree labels to match (north-up when no heading).
    double hdg_deg = NAN;
    if (!isnan(d.headingTrue)) {
        hdg_deg = rad_to_deg_pos(d.headingTrue);
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", hdg_deg);
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), buf);
    } else {
        set_text_if_changed(lbl_hdg_value, s_last_hdg, sizeof(s_last_hdg), "--\xC2\xB0");
    }
    double label_hdg = isnan(hdg_deg) ? 0.0 : hdg_deg;
    int16_t scale_rot = deg_to_lvgl(-label_hdg);
    if (scale_rot != s_last_scale_rot) {
        s_last_scale_rot = scale_rot;
        lv_obj_set_style_transform_rotation(s_cp.scale, scale_rot, 0);
        ui::compass_layout_labels(s_cp, label_hdg);
    }

    // Marker ring: HDG/COG/CTS + AP target. The reference is HDG (heading-up,
    // matching the rotating scale), so the HDG marker rides at offset 0 under the
    // red lubber. The target uses the local pending nudge if set, else the AP's
    // reported target; NaN hides any marker.
    double hdg_b = hdg_deg;
    double cog_b = isnan(d.cogTrue) ? NAN : rad_to_deg_pos(d.cogTrue);
    double cts_b = isnan(d.cts) ? NAN : rad_to_deg_pos(d.cts);
    double tgt_b = !isnan(s_target_local)
                       ? rad_to_deg_pos(s_target_local)
                       : (isnan(d.apTargetHdg) ? NAN : rad_to_deg_pos(d.apTargetHdg));
    ui::MarkerSpec live[4] = {
        {hdg_b, ui::Glyph::Triangle, true, theme.accent},
        {cog_b, ui::Glyph::Triangle, false, theme.good},
        {cts_b, ui::Glyph::Diamond, true, theme.alarm},
        {tgt_b, ui::Glyph::Diamond, true, theme.warn},
    };
    double ref = isnan(hdg_b) ? 0.0 : hdg_b;
    ui::marker_ring_update(s_cp.markers, live, 4, ref);

    // COG / SOG sub-line.
    char cogs[16], sogs[16];
    if (!isnan(d.cogTrue))
        snprintf(cogs, sizeof(cogs), "%03.0f\xC2\xB0", rad_to_deg_pos(d.cogTrue));
    else
        snprintf(cogs, sizeof(cogs), "--\xC2\xB0");
    if (!isnan(d.sog))
        snprintf(sogs, sizeof(sogs), "%.1f kn", mps_to_kn(d.sog));
    else
        snprintf(sogs, sizeof(sogs), "-- kn");
    snprintf(buf, sizeof(buf), "COG %s  |  SOG %s", cogs, sogs);
    set_text_if_changed(lbl_cogsog, s_last_cogsog, sizeof(s_last_cogsog), buf);

    // XTE needle (cross-track error, nm; +stbd). Clamp to +/-1.0 full-scale.
    double xte = d.xte;  // meters; convert to nm
    if (!isnan(xte)) {
        double nm = xte / 1852.0;
        if (nm > 1.0) nm = 1.0;
        if (nm < -1.0) nm = -1.0;
        int nx = s_xte.center_x + (int)(nm * s_xte.half_px) - 1;
        if (nx != s_last_xte_x) {
            s_last_xte_x = nx;
            lv_obj_set_x(s_xte.needle, nx);
        }
    }

    // Tiles.
    if (!isnan(d.depth)) {
        snprintf(buf, sizeof(buf), "%.1f", d.depth);
        set_text_if_changed(tile_depth, s_last_depth, sizeof(s_last_depth), buf);
    } else {
        set_text_if_changed(tile_depth, s_last_depth, sizeof(s_last_depth), "--");
    }
    if (!isnan(d.stw)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.stw));
        set_text_if_changed(tile_speed, s_last_speed, sizeof(s_last_speed), buf);
    } else {
        set_text_if_changed(tile_speed, s_last_speed, sizeof(s_last_speed), "--");
    }
    if (!isnan(d.aws)) {
        snprintf(buf, sizeof(buf), "%.1f", mps_to_kn(d.aws));
        set_text_if_changed(tile_aws, s_last_aws, sizeof(s_last_aws), buf);
    } else {
        set_text_if_changed(tile_aws, s_last_aws, sizeof(s_last_aws), "--");
    }
    if (!isnan(d.awa)) {
        double deg = rad_to_deg_pos(d.awa);
        bool starboard = deg <= 180.0;
        double mag = starboard ? deg : 360.0 - deg;
        snprintf(buf, sizeof(buf), "%.0f%c", mag, starboard ? 'S' : 'P');
        set_text_if_changed(tile_awa, s_last_awa, sizeof(s_last_awa), buf);
    } else {
        set_text_if_changed(tile_awa, s_last_awa, sizeof(s_last_awa), "--");
    }
}

}  // namespace ui::autopilot
