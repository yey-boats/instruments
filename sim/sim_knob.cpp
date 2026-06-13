// Host harness that renders the REAL Waveshare-knob round views
// (src/ui/knob_ui.cpp: ui::ap_hud / ui::knob_compass / ui::knob_wind /
// ui::knob_big) headlessly via LVGL and writes a 24-bit BMP. Same pattern as
// sim_wind.cpp, but built with -DBOARD_ID_WAVESHARE_KNOB_1_8 at 360x360 so the
// board-gated dedicated views compile and paint. The view is chosen by argv[1]
// ("ap_hud" | "compass" | "wind" | "big"); argv[2] is the output path.
//
// Boat values come from the sk::copyData stub in sim/stubs.cpp. The panel is
// physically round, so we mask the corners outside the inscribed circle to
// black in the BMP writer — the same content area (usable_*) the firmware
// keeps its labels inside. No SDL/display server required.

#include <lvgl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board.h"       // board::geometry() usable rect (round inset)
#include "board_pins.h"  // sim LCD_W/LCD_H from build defines
#include "screens.h"     // ui::ap_hud / knob_compass / knob_wind / knob_big

static void flush_cb(lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
}

// Write a 24-bit BGR bottom-up BMP from an RGB565 buffer. `round` masks the
// pixels outside the inscribed circle to black so the snapshot reads as the
// physical round panel rather than a 360x360 square.
static void write_bmp(const char *path, const uint8_t *rgb565, int w, int h, int stride,
                      bool round) {
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
    const double cx = (w - 1) / 2.0;
    const double cy = (h - 1) / 2.0;
    const double r = (w < h ? w : h) / 2.0;
    uint8_t *line = (uint8_t *)malloc(row + pad);
    for (int y = h - 1; y >= 0; --y) {  // BMP is bottom-up
        const uint16_t *src = (const uint16_t *)(rgb565 + (size_t)y * stride);
        for (int x = 0; x < w; ++x) {
            uint16_t p = src[x];
            uint8_t r8 = ((p >> 11) & 0x1F) << 3;
            uint8_t g8 = ((p >> 5) & 0x3F) << 2;
            uint8_t b8 = (p & 0x1F) << 3;
            if (round) {
                double dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > r * r) {
                    r8 = g8 = b8 = 0;  // outside the round bezel
                }
            }
            line[x * 3 + 0] = b8;
            line[x * 3 + 1] = g8;
            line[x * 3 + 2] = r8;
        }
        for (int p = 0; p < pad; ++p)
            line[row + p] = 0;
        fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

struct View {
    const char *name;
    lv_obj_t *(*build)(lv_obj_t *);
    void (*refresh)();
};

static const View kViews[] = {
    {"ap_hud", ui::ap_hud::build, ui::ap_hud::refresh},
    {"compass", ui::knob_compass::build, ui::knob_compass::refresh},
    {"wind", ui::knob_wind::build, ui::knob_wind::refresh},
    {"big", ui::knob_big::build, ui::knob_big::refresh},
};

int main(int argc, char **argv) {
    const char *view_name = (argc > 1) ? argv[1] : "ap_hud";
    const char *out = (argc > 2) ? argv[2] : "knob.bmp";

    const View *v = nullptr;
    for (const auto &cand : kViews) {
        if (strcmp(cand.name, view_name) == 0) {
            v = &cand;
            break;
        }
    }
    if (!v) {
        fprintf(stderr, "unknown view '%s' (ap_hud|compass|wind|big)\n", view_name);
        return 2;
    }

    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    static uint8_t *buf = (uint8_t *)malloc((size_t)LCD_W * LCD_H * 2);
    lv_display_set_buffers(disp, buf, nullptr, (size_t)LCD_W * LCD_H * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_obj_t *root = v->build(lv_screen_active());
    if (!root) {
        fprintf(stderr, "%s::build() returned null\n", v->name);
        return 1;
    }

    // A few refresh ticks so the dirty-value caches latch the stub snapshot.
    for (int i = 0; i < 8; ++i) {
        v->refresh();
        lv_tick_inc(20);
        lv_timer_handler();
    }
    lv_refr_now(disp);

    // Round-panel bounds check: every visible label (descendant of the view
    // root) must sit inside the inscribed circle, i.e. within the usable_*
    // rect the firmware insets to. Out-of-circle content would clip on the
    // physical round bezel.
    int rc = 0;
    board::Geometry g = board::geometry();
    const int ux1 = g.usable_x;
    const int uy1 = g.usable_y;
    const int ux2 = g.usable_x + g.usable_width - 1;
    const int uy2 = g.usable_y + g.usable_height - 1;
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *c = lv_obj_get_child(root, i);
        lv_area_t ar;
        lv_obj_get_coords(c, &ar);
        if (ar.x1 < ux1 || ar.y1 < uy1 || ar.x2 > ux2 || ar.y2 > uy2) {
            fprintf(stderr,
                    "BOUNDS: %s child %u (%d,%d)-(%d,%d) outside usable "
                    "rect (%d,%d)-(%d,%d)\n",
                    v->name, i, ar.x1, ar.y1, ar.x2, ar.y2, ux1, uy1, ux2, uy2);
            rc = 1;
        }
    }
    if (rc == 0)
        printf("knob view '%s' ok (%u children inside usable circle) %dx%d\n", v->name, n, LCD_W,
               LCD_H);

    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        fprintf(stderr, "snapshot failed\n");
        return 1;
    }
    write_bmp(out, (const uint8_t *)snap->data, snap->header.w, snap->header.h, snap->header.stride,
              /*round=*/true);
    lv_draw_buf_destroy(snap);
    return rc;
}
