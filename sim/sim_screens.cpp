// Host harness that renders any of the QuadGrid-template screens (nav, depth,
// steering, route, trip) and the full-screen "zoom" view headlessly via LVGL,
// writing a 24-bit BMP. Lets every screen be eyeballed at every display class
// without a panel. Screen id is argv[1], output path argv[2]. Values come from
// the sk::copyData stub in sim/stubs.cpp.

#include <lvgl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board_pins.h"
#include "screens.h"
#include "ui_layouts.h"
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

// Zoom targets used to exercise the full-screen single-value view.
static const ui::layouts::MetricBinding kZoomPos = {
    "pos",   "POSITION", "", ui::layouts::MetricSource::Position, 0x39d98a,
    nullptr, 0,          {}, ui::layouts::WidgetKind::Text};
static const ui::layouts::MetricBinding kZoomNum = {
    "sog",   "SOG", "kn", ui::layouts::MetricSource::SOG_kn, 0x57c7d8,
    nullptr, 0,     {},   ui::layouts::WidgetKind::Numeric};

static lv_obj_t *build_screen(const char *id) {
    lv_obj_t *p = lv_screen_active();
    if (!strcmp(id, "nav")) return ui::nav::build(p);
    if (!strcmp(id, "depth")) return ui::depth::build(p);
    if (!strcmp(id, "steering")) return ui::steering::build(p);
    if (!strcmp(id, "route")) return ui::route::build(p);
    if (!strcmp(id, "trip")) return ui::trip::build(p);
    if (!strcmp(id, "wind_steer")) return ui::wind_steer::build(p);
    if (!strcmp(id, "wind_classic")) return ui::wind_classic::build(p);
    if (!strcmp(id, "zoom-pos")) {
        ui::layouts::set_zoom_target(kZoomPos);
        return ui::zoom::build(p);
    }
    if (!strcmp(id, "zoom-num")) {
        ui::layouts::set_zoom_target(kZoomNum);
        return ui::zoom::build(p);
    }
    return nullptr;
}

static void refresh_screen(const char *id) {
    if (!strcmp(id, "nav")) return ui::nav::refresh();
    if (!strcmp(id, "depth")) return ui::depth::refresh();
    if (!strcmp(id, "steering")) return ui::steering::refresh();
    if (!strcmp(id, "route")) return ui::route::refresh();
    if (!strcmp(id, "trip")) return ui::trip::refresh();
    if (!strcmp(id, "wind_steer")) return ui::wind_steer::refresh();
    if (!strcmp(id, "wind_classic")) return ui::wind_classic::refresh();
    if (!strncmp(id, "zoom", 4)) return ui::zoom::refresh();
}

int main(int argc, char **argv) {
    const char *id = (argc > 1) ? argv[1] : "nav";
    const char *out = (argc > 2) ? argv[2] : "screen.bmp";

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
