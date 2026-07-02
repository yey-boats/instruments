// Host harness that renders the REAL "controlled" frame overlay
// (src/ui/control_frame.cpp: ui::control_frame::build + set_sessions)
// headlessly via LVGL and writes a 24-bit BMP. Same pattern as sim_knob.cpp's
// menu-overlay path: build the overlay on lv_layer_top(), drive it with
// synthetic proto::Session structs, then snapshot the top layer directly.
//
// Shape + resolution are chosen at runtime via argv so one binary renders both
// the rectangular 480x480 Sunton panel and the round 360x360 knob panel:
//   sim_control <rect|round> <count> <out.bmp>
// where <count> is the number of synthetic active sessions (1..kMaxSessions).
// This driver OWNS board::geometry() (below) so the overlay's round/rect branch
// is exercised without pulling in a board impl.

#include <lvgl.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board.h"
#include "proto/proto.h"
#include "screens.h"  // ui::control_frame::{build,set_sessions}

// ---- board::geometry() shape switch (driver-owned) ----------------------
// g_round + g_w/g_h are set from argv before build() runs.
static bool g_round = false;
static int g_w = 480;
static int g_h = 480;

namespace board {
Geometry geometry() {
    Geometry g{};
    g.width_px = (uint16_t)g_w;
    g.height_px = (uint16_t)g_h;
    g.rotation = 0;
    g.square = (g_w == g_h);
    g.shape = g_round ? DisplayShape::Round : DisplayShape::Rectangle;
    g.layout_class = LayoutClass::SquareCompact;
    g.density_class = DensityClass::Hdpi;
    if (g_round) {
        // Mirror src/boards/board_waveshare_knob.cpp inscribed-square inset.
        const uint16_t inset = 53;
        g.usable_x = inset;
        g.usable_y = inset;
        g.usable_width = g.width_px - inset * 2;
        g.usable_height = g.height_px - inset * 2;
    } else {
        g.usable_x = 0;
        g.usable_y = 0;
        g.usable_width = g.width_px;
        g.usable_height = g.height_px;
    }
    return g;
}
}  // namespace board

static void flush_cb(lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
}

// Write a 24-bit BGR bottom-up BMP from an RGB565 buffer. `round` masks pixels
// outside the inscribed circle to black so the snapshot reads as a round panel.
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
                    r8 = g8 = b8 = 0;
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

// A small palette of distinct controller colors + names so stacked rings read
// as different controllers.
static const char *kColors[] = {"#ff4060", "#40c0ff", "#40ff80", "#ffc000",
                                "#c060ff", "#ff8040", "#00e0c0", "#ffffff"};
static const char *kNames[] = {"helm",     "nav-pc", "phone",  "tablet",
                               "engineer", "bridge", "remote", "guest"};

int main(int argc, char **argv) {
    const char *shape = (argc > 1) ? argv[1] : "rect";
    int count = (argc > 2) ? atoi(argv[2]) : 1;
    const char *out = (argc > 3) ? argv[3] : "control.bmp";

    g_round = (strcmp(shape, "round") == 0);
    g_w = g_round ? 360 : 480;
    g_h = g_round ? 360 : 480;
    // Optional rect-only resolution override (argv[4]/argv[5]): the round
    // panel is a fixed physical 360x360, but the rect overlay is shared
    // across every square/landscape touch panel, so let callers (e.g.
    // tools/render_all_resolutions.sh) sweep 480x480/800x480/1024x600.
    if (!g_round && argc > 5) {
        g_w = atoi(argv[4]);
        g_h = atoi(argv[5]);
    }
    if (count < 0) count = 0;
    if (count > proto::kMaxSessions) count = proto::kMaxSessions;

    lv_init();
    lv_display_t *disp = lv_display_create(g_w, g_h);
    static uint8_t *buf = nullptr;
    buf = (uint8_t *)malloc((size_t)g_w * g_h * 2);
    lv_display_set_buffers(disp, buf, nullptr, (size_t)g_w * g_h * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    // Give the active screen a dark background so the colored rings read.
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x0a1018), 0);

    ui::control_frame::build(nullptr);

    // Synthesize `count` active sessions. lastSeen ascending by index so the
    // overlay's most-recent-first reorder puts session[count-1] outermost; we
    // pass them in slot order (as active_session_snapshot would).
    proto::Session s[proto::kMaxSessions];
    memset(s, 0, sizeof(s));
    for (int i = 0; i < count; ++i) {
        strncpy(s[i].name, kNames[i % 8], sizeof(s[i].name) - 1);
        strncpy(s[i].color, kColors[i % 8], sizeof(s[i].color) - 1);
        s[i].lastSeen = 1000 + i * 100;  // later index == more recent
    }
    ui::control_frame::set_sessions(s, count);

    for (int i = 0; i < 4; ++i) {
        lv_tick_inc(20);
        lv_timer_handler();
    }
    lv_refr_now(lv_display_get_default());

    // DIRECT render mode composites screen + layer_top into `buf` (the full
    // framebuffer), so the transparent-filled rings show over the screen
    // background. Snapshotting layer_top alone would lose the composite.
    write_bmp(out, buf, g_w, g_h, g_w * 2, g_round);
    printf("control-frame '%s' count=%d %dx%d\n", shape, count, g_w, g_h);
    return 0;
}
