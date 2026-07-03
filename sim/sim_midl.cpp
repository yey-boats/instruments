// Host MIDL design-parity harness: renders one screen of the baked YB-MIDL
// demo document (midl::demo::SQUARE_480_JSON, 6 gallery screens) through the
// REAL device render path — midl::solve_screen -> midl::render::map_element ->
// ui::layouts::create_freeform — headlessly via LVGL, snapshots the active
// screen, and writes a 24-bit BMP. This mirrors src/midl_render_apply.cpp's
// apply_doc() flow (which is device-only and not in the native/sim build), so
// the snapshot is a faithful preview of what the firmware paints for the demo
// doc. Compare against preview-square-480.png.
//
// Usage (no SDL/display server required):
//   program <out.bmp>            render the doc's first screen (legacy form,
//                                kept for tools/sim_render.sh)
//   program <screen> <out.bmp>   render the gallery screen selected by exact
//                                id ("dash","nav",...) or case-insensitive
//                                title ("wind","course","engine","power",
//                                "race","anchor")
// SIM_THEME selects the palette (see sim/sim_theme.h).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <lvgl.h>

#include <ArduinoJson.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board_pins.h"  // sim LCD_W/LCD_H from build defines
#include "midl_demo_doc.h"
#include "midl_render.h"
#include "midl_solve.h"
#include "signalk.h"    // boat::View, boat::current_view (stub)
#include "sim_theme.h"  // SIM_THEME env -> ui::use_theme
#include "ui_layouts.h"

using namespace ui::layouts;

static void flush_cb(lv_display_t *d, const lv_area_t *, uint8_t *) {
    lv_display_flush_ready(d);
}

// Write a 24-bit BGR bottom-up BMP from an RGB565 buffer. (Identical to
// sim_main.cpp so the produced BMP/PNG matches the other sim shots.)
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

// String storage backing the MetricBinding non-owning pointers (id/label/unit),
// plus the MetricBinding table and solved rects. Host stack is generous, but
// mirror the device arena layout for fidelity (and to keep map_element happy:
// it COPIES into these caller-owned buffers).
static constexpr size_t MAX_TILES = midl::FirmwareLimits::max_tiles_per_screen;  // 4
static constexpr size_t STR_CAP = midl::FirmwareLimits::str_len;                 // 32

// Case-insensitive ASCII string equality (screen titles are single words).
static bool iequals(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

int main(int argc, char **argv) {
    const char *sel = (argc > 2) ? argv[1] : nullptr;
    const char *out = (argc > 2) ? argv[2] : (argc > 1 ? argv[1] : "midl.bmp");

    if (!sim::apply_theme_from_env()) return 2;
    lv_init();
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    static uint8_t *buf = (uint8_t *)malloc((size_t)LCD_W * LCD_H * 2);
    lv_display_set_buffers(disp, buf, nullptr, (size_t)LCD_W * LCD_H * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    // --- Parse the baked MIDL demo document ---
    JsonDocument midl_doc;
    DeserializationError jerr = deserializeJson(midl_doc, midl::demo::SQUARE_480_JSON);
    if (jerr) {
        fprintf(stderr, "deserializeJson failed: %s\n", jerr.c_str());
        return 1;
    }

    // --- Locate the requested screen (mirrors apply_doc step 1). With no
    // selector, fall back to screens[0] like the legacy single-shot harness.
    JsonVariantConst screens = midl_doc["screens"];
    if (!screens.is<JsonArrayConst>() || screens.size() == 0) {
        fprintf(stderr, "doc has no screens[]\n");
        return 1;
    }
    JsonVariantConst screen = screens[0];
    const char *screen_id = screen["id"] | "screen0";
    const char *screen_title = screen["title"] | screen_id;
    if (sel) {
        bool found = false;
        for (JsonVariantConst s : screens.as<JsonArrayConst>()) {
            const char *id = s["id"] | "";
            const char *title = s["title"] | "";
            if (strcmp(sel, id) == 0 || iequals(sel, title)) {
                screen = s;
                screen_id = id;
                screen_title = title[0] ? title : id;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "no demo screen matches '%s' (id or title of:", sel);
            for (JsonVariantConst s : screens.as<JsonArrayConst>())
                fprintf(stderr, " %s/%s", s["id"] | "?", s["title"] | "?");
            fprintf(stderr, ")\n");
            return 2;
        }
    }

    // --- Solve the layout (mirrors apply_doc step 2) ---
    midl::PlacementSet ps;
    midl::SolveStatus st = midl::solve_screen(screen["layout"], {0, 0, LCD_W, LCD_H}, ps);
    if (st != midl::SOLVE_OK) {
        fprintf(stderr, "solve_screen failed: status=%d\n", (int)st);
        return 1;
    }
    if (ps.count == 0) {
        fprintf(stderr, "solve_screen produced 0 placements\n");
        return 1;
    }

    // --- Map each placement -> MetricBinding into a local arena (apply_doc step 3) ---
    static char ids[MAX_TILES][STR_CAP];
    static char labels[MAX_TILES][STR_CAP];
    static char units[MAX_TILES][STR_CAP];
    static char actions[MAX_TILES][STR_CAP];
    static char zooms[MAX_TILES][STR_CAP];
    static MetricBinding metrics[MAX_TILES];
    static Rect rects[MAX_TILES];
    memset(ids, 0, sizeof(ids));
    memset(labels, 0, sizeof(labels));
    memset(units, 0, sizeof(units));
    memset(actions, 0, sizeof(actions));
    memset(zooms, 0, sizeof(zooms));
    memset(metrics, 0, sizeof(metrics));
    memset(rects, 0, sizeof(rects));

    JsonVariantConst elements = screen["elements"];
    size_t n = ps.count;
    if (n > MAX_TILES) n = MAX_TILES;

    for (size_t i = 0; i < n; ++i) {
        const midl::Placement &pl = ps.items[i];
        // Look up the element by a FRESH stack copy of the key. Indexing the
        // ArduinoJson object directly with pl.element (a pointer into the solver's
        // PlacementSet buffer) returns null in this host build — ArduinoJson 7's
        // operator[](const char *) takes a pointer-identity (zero-copy) path for
        // that buffer instead of a by-value string compare. Copying the key onto a
        // fresh stack slot forces the string lookup and the element resolves.
        // (The device apply_doc() in src/midl_render_apply.cpp indexes pl.element
        // directly; this copy keeps the host harness faithful to that mapping.)
        char key[STR_CAP];
        strncpy(key, pl.element, STR_CAP - 1);
        key[STR_CAP - 1] = 0;
        JsonVariantConst el = elements[(const char *)key];
        bool ok = midl::render::map_element(el, pl.element, metrics[i], ids[i], labels[i], units[i],
                                            actions[i], zooms[i]);
        if (!ok) {
            strncpy(ids[i], pl.element, STR_CAP - 1);
            ids[i][STR_CAP - 1] = 0;
            metrics[i].id = ids[i];
            metrics[i].label = ids[i];
            metrics[i].unit = units[i];
            metrics[i].source = MetricSource::None;
        }
        rects[i] = {pl.rect.x, pl.rect.y, pl.rect.w, pl.rect.h};
    }

    // --- Build the ScreenVariantSpec + freeform screen (apply_doc steps 4-5) ---
    ScreenVariantSpec spec = {screen_id, screen_title, TemplateId::QuadGrid,
                              metrics,   (uint8_t)n,   0};

    lv_obj_t *root = create_freeform(nullptr, spec, rects);
    if (!root) {
        fprintf(stderr, "create_freeform() returned null\n");
        return 1;
    }
    lv_screen_load(root);

    // --- Drive a few refresh cycles with a demo snapshot so tiles aren't blank ---
    boat::View d;
    boat::current_view(d);
    for (int i = 0; i < 8; ++i) {
        update_freeform(root, spec, d);
        lv_tick_inc(20);
        lv_timer_handler();
    }
    lv_refr_now(disp);

    // --- No-overlap / in-bounds assertion (ported from sim_main.cpp). Each tile
    // panel is a direct child of the screen root; it must sit within the display
    // and not overlap a sibling. Also flag tiles that resolved to a degenerate
    // (zero-area) rect as "blank".
    int rc = 0;
    uint32_t childc = lv_obj_get_child_count(root);
    lv_area_t a[16];
    uint32_t na = 0;
    for (uint32_t i = 0; i < childc && na < 16; ++i) {
        lv_obj_t *c = lv_obj_get_child(root, i);
        lv_area_t ar;
        lv_obj_get_coords(c, &ar);
        if (ar.x1 < 0 || ar.y1 < 0 || ar.x2 >= LCD_W || ar.y2 >= LCD_H) {
            fprintf(stderr, "BOUNDS: tile %u out of bounds (%d,%d)-(%d,%d) on %dx%d\n", i, ar.x1,
                    ar.y1, ar.x2, ar.y2, LCD_W, LCD_H);
            rc = 1;
        }
        if (ar.x2 <= ar.x1 || ar.y2 <= ar.y1) {
            fprintf(stderr, "BLANK: tile %u has zero area (%d,%d)-(%d,%d)\n", i, ar.x1, ar.y1,
                    ar.x2, ar.y2);
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
    if (rc == 0)
        printf("screen '%s' (%s) layout ok (%u tiles, no overlap, in bounds, non-blank) %dx%d\n",
               screen_id, screen_title, na, LCD_W, LCD_H);

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
