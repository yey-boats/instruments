// Host harness that renders the system touch screens (touch_grid, touch_cal)
// headlessly via LVGL, writing a 24-bit BMP. Content screens are MIDL records
// (env:sim-midl). Screen id is argv[1], output path argv[2].
//
// touch_grid / touch_cal are full-screen roots that poll raw touch from an
// lv_timer. With SIM_REBUILD=1 they are deleted the way reset_screens() does
// it (park on a blank screen, lv_obj_delete the root) and a tap is fed while
// no root exists: a poll timer that outlives its root then writes freed
// objects (instruments#73). Build env:sim-screens-asan to make that fatal.

#include <Arduino.h>  // sim clock: g_sim_millis
#include <lvgl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board_pins.h"
#include "screens.h"
#include "sim_theme.h"  // SIM_THEME env -> ui::use_theme
#include "signalk.h"

static void flush_cb(lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
}

static void write_bmp(const char *path, const uint8_t *rgb565, int w, int h, int stride) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return;
    }
    const int row = w * 3;
    const int pad = (4 - (row % 4)) % 4;
    const int img = (row + pad) * h;
    const int off = 54;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    uint32_t fsz = off + img;
    hdr[2] = fsz;
    hdr[3] = fsz >> 8;
    hdr[4] = fsz >> 16;
    hdr[5] = fsz >> 24;
    hdr[10] = off;
    hdr[14] = 40;
    hdr[18] = w;
    hdr[19] = w >> 8;
    hdr[20] = w >> 16;
    hdr[22] = h;
    hdr[23] = h >> 8;
    hdr[24] = h >> 16;
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t *line = (uint8_t *)malloc(row + pad);
    for (int y = h - 1; y >= 0; --y) {
        const uint16_t *src = (const uint16_t *)(rgb565 + (size_t)y * stride);
        for (int x = 0; x < w; ++x) {
            uint16_t p = src[x];
            uint8_t r = ((p >> 11) & 0x1F) << 3;
            uint8_t g = ((p >> 5) & 0x3F) << 2;
            uint8_t b = (p & 0x1F) << 3;
            line[x * 3 + 0] = b;
            line[x * 3 + 1] = g;
            line[x * 3 + 2] = r;
        }
        for (int p = 0; p < pad; ++p)
            line[row + p] = 0;
        fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

// Raw touch fed to the touch screens' poll timers (main.cpp on device).
static int s_touch_x = -1, s_touch_y = -1, s_touch_pressed = 0;
extern "C" void main_touch_raw(int *raw_x, int *raw_y, int *pressed) {
    *raw_x = s_touch_x;
    *raw_y = s_touch_y;
    *pressed = s_touch_pressed;
}

// Advance LVGL ticks and the millis() clock together, running due timers.
static void pump(int ms) {
    for (int t = 0; t < ms; t += 10) {
        lv_tick_inc(10);
        g_sim_millis += 10;
        lv_timer_handler();
    }
}

// One 200 ms press + release at (x, y): long enough for both screens' glitch
// filters (25 ms grid, 60 ms cal) and several poll periods on each edge.
static void tap(int x, int y) {
    s_touch_x = x;
    s_touch_y = y;
    s_touch_pressed = 1;
    pump(200);
    s_touch_pressed = 0;
    pump(200);
}

static bool is_touch_screen(const char *id) {
    return !strcmp(id, "touch_grid") || !strcmp(id, "touch_cal");
}

static lv_obj_t *build_screen(const char *id) {
    // Touch screens are standalone roots (lazy-built with a NULL parent on
    // device) and only act while loaded, so load them like show() does.
    lv_obj_t *r = nullptr;
    if (!strcmp(id, "touch_grid")) r = ui::touch_grid_screen::build(nullptr);
    if (!strcmp(id, "touch_cal")) r = ui::touch_cal_screen::build(nullptr);
    if (r) lv_screen_load(r);
    return r;
}

static void refresh_screen(const char *id) {
    if (!strcmp(id, "touch_grid")) return ui::touch_grid_screen::refresh();
    if (!strcmp(id, "touch_cal")) return ui::touch_cal_screen::refresh();
}

int main(int argc, char **argv) {
    const char *id = (argc > 1) ? argv[1] : "touch_grid";
    const char *out = (argc > 2) ? argv[2] : "screen.bmp";

    if (!sim::apply_theme_from_env()) return 2;
    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    static uint8_t *buf = (uint8_t *)malloc((size_t)LCD_W * LCD_H * 2);
    lv_display_set_buffers(disp, buf, nullptr, (size_t)LCD_W * LCD_H * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_obj_t *root = build_screen(id);
    if (!root) {
        fprintf(stderr, "unknown screen id '%s'\n", id);
        return 1;
    }
    for (int i = 0; i < 8; ++i) {
        refresh_screen(id);
        lv_tick_inc(20);
        lv_timer_handler();
    }
    const bool touch = is_touch_screen(id);
    // A tap the screen consumes: lights a grid cell / advances the cal cross.
    if (touch) tap(LCD_W / 2, LCD_H / 2);
    // SIM_REBUILD=1: delete and rebuild the screen, as a live theme switch or
    // config push does (reset_screens -> build), then refresh again. The
    // render must match a single build: tools/check_rebuild_render.sh diffs
    // the two, catching dirty-value caches that outlive their widgets.
    const char *rebuild = getenv("SIM_REBUILD");
    if (rebuild && rebuild[0] == '1') {
        lv_obj_t *parking = nullptr;
        if (touch) {
            // reset_screens(): park on a blank screen, then free the root. A
            // lazy screen stays unbuilt until next shown, so a tap lands
            // while no root exists - any poll timer left running touches
            // freed objects here.
            parking = lv_obj_create(nullptr);
            lv_screen_load(parking);
        }
        lv_obj_delete(root);
        if (touch) tap(LCD_W / 2, LCD_H / 2);
        root = build_screen(id);
        if (parking) lv_obj_delete(parking);
        for (int i = 0; i < 8; ++i) {
            refresh_screen(id);
            lv_tick_inc(20);
            lv_timer_handler();
        }
        if (touch) tap(LCD_W / 2, LCD_H / 2);
    }
    lv_refr_now(disp);

    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        fprintf(stderr, "snapshot failed\n");
        return 1;
    }
    write_bmp(out, (const uint8_t *)snap->data, snap->header.w, snap->header.h,
              snap->header.stride);
    lv_draw_buf_destroy(snap);
    return 0;
}
