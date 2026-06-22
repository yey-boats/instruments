// Host harness that renders the REAL autopilot HUD (src/ui/screen_autopilot.cpp)
// headlessly via LVGL and writes a 24-bit BMP, so the semicircular compass,
// target bug, XTE strip, and numeric tiles can be eyeballed without a panel.
// AP / heading / wind values come from the boat::current_view stub in sim/stubs.cpp.
// No SDL/display server required.

#include <lvgl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "board_pins.h"  // sim LCD_W/LCD_H from build defines
#include "screens.h"     // ui::autopilot::build / refresh
#include "signalk.h"     // boat::View, boat::current_view (stub)

static void flush_cb(lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
}

// Write a 24-bit BGR bottom-up BMP from an RGB565 buffer (same as sim_main.cpp).
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
    for (int y = h - 1; y >= 0; --y) {  // BMP is bottom-up
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

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "wind.bmp";

    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    static uint8_t *buf = (uint8_t *)malloc((size_t)LCD_W * LCD_H * 2);
    lv_display_set_buffers(disp, buf, nullptr, (size_t)LCD_W * LCD_H * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_obj_t *root = ui::autopilot::build(lv_screen_active());
    if (!root) {
        fprintf(stderr, "ui::autopilot::build() returned null\n");
        return 1;
    }

    // A few refresh ticks so the dirty-value caches latch the stub snapshot and
    // the compass scale + target bug rotate to heading / target.
    for (int i = 0; i < 8; ++i) {
        ui::autopilot::refresh();
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
