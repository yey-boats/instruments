// Host resolution harness: renders the real device layout code (ui_layouts)
// headlessly via LVGL at a build-defined resolution, snapshots the screen, and
// writes a 24-bit BMP. Run once per supported display class (see
// tools/sim_render.sh). No SDL/display server required.

#include <lvgl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "board_pins.h"  // sim LCD_W/LCD_H from build defines
#include "signalk.h"     // boat::View, boat::current_view (stub)
#include "ui_layouts.h"

using namespace ui::layouts;

static void flush_cb(lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
}

// Write a 24-bit BGR bottom-up BMP from an RGB565 buffer.
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
    const char *out = (argc > 1) ? argv[1] : "dash.bmp";

    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    static uint8_t *buf = (uint8_t *)malloc((size_t)LCD_W * LCD_H * 2);
    lv_display_set_buffers(disp, buf, nullptr, (size_t)LCD_W * LCD_H * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    // 2x2 dashboard: wind dial, SOG, depth, battery bar.
    static const MetricBinding tiles[] = {
        {"wind",
         "WIND",
         "kn",
         MetricSource::AWS_kn,
         0xffb84d,
         nullptr,
         0,
         {},
         WidgetKind::WindRose},
        {"sog", "SOG", "kn", MetricSource::SOG_kn, 0x4fc3f7, nullptr, 0, {}, WidgetKind::Numeric},
        {"depth",
         "DEPTH",
         "m",
         MetricSource::Depth_m,
         0x4fc3f7,
         nullptr,
         0,
         {},
         WidgetKind::Numeric},
        {"batt",
         "BATT",
         "%",
         MetricSource::BatterySOC_pct,
         0x36d399,
         nullptr,
         0,
         {},
         WidgetKind::Bar},
    };
    static const ScreenVariantSpec spec = {"dashboard", "Dashboard", TemplateId::QuadGrid,
                                           tiles,       4,           0};

    lv_obj_t *root = create(nullptr, spec);
    if (!root) {
        fprintf(stderr, "create() returned null\n");
        return 1;
    }
    lv_screen_load(root);

    boat::View d;
    boat::current_view(d);
    for (int i = 0; i < 8; ++i) {
        update(root, spec, d);
        lv_tick_inc(20);
        lv_timer_handler();
    }
    lv_refr_now(disp);

    // No-overlap / in-bounds assertion: every tile panel (direct child of the
    // screen root) must sit within the display and not overlap a sibling.
    int rc = 0;
    uint32_t n = lv_obj_get_child_count(root);
    lv_area_t a[16];
    uint32_t na = 0;
    for (uint32_t i = 0; i < n && na < 16; ++i) {
        lv_obj_t *c = lv_obj_get_child(root, i);
        lv_area_t ar;
        lv_obj_get_coords(c, &ar);
        if (ar.x1 < 0 || ar.y1 < 0 || ar.x2 >= LCD_W || ar.y2 >= LCD_H) {
            fprintf(stderr, "OVERLAP/BOUNDS: tile %u out of bounds (%d,%d)-(%d,%d) on %dx%d\n", i,
                    ar.x1, ar.y1, ar.x2, ar.y2, LCD_W, LCD_H);
            rc = 1;
        }
        a[na++] = ar;
    }
    for (uint32_t i = 0; i < na; ++i)
        for (uint32_t j = i + 1; j < na; ++j) {
            bool overlap =
                !(a[i].x2 < a[j].x1 || a[j].x2 < a[i].x1 || a[i].y2 < a[j].y1 || a[j].y2 < a[i].y1);
            if (overlap) {
                fprintf(stderr, "OVERLAP: tiles %u and %u overlap on %dx%d\n", i, j, LCD_W, LCD_H);
                rc = 1;
            }
        }
    if (rc == 0) printf("layout ok (%u tiles, no overlap, in bounds) %dx%d\n", na, LCD_W, LCD_H);

    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!snap) {
        fprintf(stderr, "snapshot failed\n");
        return 1;
    }
    write_bmp(out, (const uint8_t *)snap->data, snap->header.w, snap->header.h,
              snap->header.stride);
    lv_draw_buf_destroy(snap);
    return rc;
}
