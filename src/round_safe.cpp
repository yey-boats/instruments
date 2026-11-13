// Round-safe tile placement: see include/round_safe.h (instruments#50).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "round_safe.h"

#include <math.h>

namespace ui {
namespace round_safe {

static double dmax(double a, double b) {
    return a > b ? a : b;
}
static double dmin(double a, double b) {
    return a < b ? a : b;
}

Disc viewport_disc(const Rect &vp, double margin) {
    return {vp.x + vp.w / 2, vp.y + vp.h / 2, dmax(0, dmin(vp.w, vp.h) / 2 - margin)};
}

static bool inside(const Disc &d, double x, double y) {
    const double dx = x - d.cx, dy = y - d.cy;
    return dx * dx + dy * dy <= d.r * d.r + 1e-6;
}

bool rect_in_disc(const Rect &t, const Disc &d) {
    return inside(d, t.x, t.y) && inside(d, t.x + t.w, t.y) && inside(d, t.x, t.y + t.h) &&
           inside(d, t.x + t.w, t.y + t.h);
}

// For a horizontal half-width budget `half` (the farthest any column may sit
// from the disc centre) the usable height is the chord at that distance, so a
// 1-D scan over `half` finds the optimum.
Rect safe_rect(const Rect &t, const Disc &d) {
    if (rect_in_disc(t, d)) return t;
    Rect best = {t.x + t.w / 2, t.y + t.h / 2, 0, 0};
    const int steps = 240;
    for (int i = 1; i <= steps; ++i) {
        const double half = (d.r * i) / steps;
        const double x0 = dmax(t.x, d.cx - half), x1 = dmin(t.x + t.w, d.cx + half);
        if (x1 <= x0) continue;
        const double dx = dmax(fabs(x0 - d.cx), fabs(x1 - d.cx));
        if (dx > d.r) continue;
        const double chord = sqrt(d.r * d.r - dx * dx);
        const double y0 = dmax(t.y, d.cy - chord), y1 = dmin(t.y + t.h, d.cy + chord);
        if (y1 <= y0) continue;
        if ((x1 - x0) * (y1 - y0) > best.w * best.h) best = {x0, y0, x1 - x0, y1 - y0};
    }
    return best;
}

static double dist_to_box(double px, double py, const Rect &b) {
    const double dx = dmax(dmax(b.x - px, 0), px - (b.x + b.w));
    const double dy = dmax(dmax(b.y - py, 0), py - (b.y + b.h));
    return hypot(dx, dy);
}

static double radius_at(const Rect &t, const Disc &d, const Rect *avoid, double px, double py) {
    double r = d.r - hypot(px - d.cx, py - d.cy);
    r = dmin(r, px - t.x);
    r = dmin(r, t.x + t.w - px);
    r = dmin(r, py - t.y);
    r = dmin(r, t.y + t.h - py);
    if (avoid) r = dmin(r, dist_to_box(px, py, *avoid));
    return r;
}

// Coarse grid then a local refinement (same search as round-safe.ts).
Rect fit_circle(const Rect &t, const Disc &d, const Rect *avoid) {
    double bx = t.x + t.w / 2, by = t.y + t.h / 2;
    double br = radius_at(t, d, avoid, bx, by);
    const int n = 32;
    for (int i = 0; i <= n; ++i) {
        for (int j = 0; j <= n; ++j) {
            const double px = t.x + (t.w * i) / n, py = t.y + (t.h * j) / n;
            const double r = radius_at(t, d, avoid, px, py);
            if (r > br) {
                br = r;
                bx = px;
                by = py;
            }
        }
    }
    static const double dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (double step = dmax(t.w, t.h) / n; step > 0.25; step /= 2) {
        for (const auto &o : dirs) {
            const double nx = bx + o[0] * step, ny = by + o[1] * step;
            const double r = radius_at(t, d, avoid, nx, ny);
            if (r > br) {
                br = r;
                bx = nx;
                by = ny;
            }
        }
    }
    br = dmax(0, br);
    return {bx - br, by - br, 2 * br, 2 * br};
}

IRect inner_pixels(const Rect &r) {
    const int x0 = (int)ceil(r.x - 1e-9), y0 = (int)ceil(r.y - 1e-9);
    const int x1 = (int)floor(r.x + r.w + 1e-9), y1 = (int)floor(r.y + r.h + 1e-9);
    return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}

TilePlan plan_tile(const IRect &cell, const Disc &d, bool dial, size_t caption_len) {
    TilePlan p = {};
    const Rect c = {(double)cell.x, (double)cell.y, (double)cell.w, (double)cell.h};
    const Rect safe = safe_rect(c, d);
    p.cap = inner_pixels(safe);
    p.body = p.cap;
    // A cell (almost) entirely under the bezel keeps its panel and nothing else.
    if (p.cap.w < kMinSafe || p.cap.h < kCaptionBand + 12) {
        p.hidden = true;
        return p;
    }
    if (dial) {
        // Round dials fit as circles (a full-screen dial keeps its size), clear
        // of the chrome caption when the tile has one.
        const Rect cap_box = {safe.x, safe.y,
                              (double)(kCaptionLeft + (int)caption_len * kCaptionGlyphW),
                              (double)(kCaptionBand + 2)};
        const Rect inset = {c.x + 2, c.y + 2, dmax(0, c.w - 4), dmax(0, c.h - 4)};
        p.body = inner_pixels(fit_circle(inset, d, caption_len ? &cap_box : nullptr));
        // Keep it square after pixel snapping.
        const int s = p.body.w < p.body.h ? p.body.w : p.body.h;
        p.body.w = p.body.h = s;
    }
    return p;
}

}  // namespace round_safe
}  // namespace ui
