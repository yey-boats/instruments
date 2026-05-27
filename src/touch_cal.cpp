#include "touch_cal.h"

#include <Preferences.h>
#include <math.h>

#include "net.h"

namespace touch_cal {

static Matrix s_m = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
static bool s_default = true;

static const char *NS = "touch_cal";

void setup() {
    Preferences p;
    p.begin(NS, true);
    if (p.isKey("a")) {
        s_m.a = p.getFloat("a", 1.0f);
        s_m.b = p.getFloat("b", 0.0f);
        s_m.c = p.getFloat("c", 0.0f);
        s_m.d = p.getFloat("d", 0.0f);
        s_m.e = p.getFloat("e", 1.0f);
        s_m.f = p.getFloat("f", 0.0f);
        s_default = false;
        net::logf("[cal] loaded a=%.4f b=%.4f c=%.2f d=%.4f e=%.4f f=%.2f",
                  s_m.a, s_m.b, s_m.c, s_m.d, s_m.e, s_m.f);
    } else {
        net::logf("[cal] no saved calibration - using identity");
    }
    p.end();
}

void apply(int16_t *x, int16_t *y) {
    if (!x || !y) return;
    if (s_default) return;
    float rx = (float)*x;
    float ry = (float)*y;
    float sx = s_m.a * rx + s_m.b * ry + s_m.c;
    float sy = s_m.d * rx + s_m.e * ry + s_m.f;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx > 479) sx = 479;
    if (sy > 479) sy = 479;
    *x = (int16_t)lroundf(sx);
    *y = (int16_t)lroundf(sy);
}

void set(const Matrix &m) {
    s_m = m;
    s_default = false;
    Preferences p;
    p.begin(NS, false);
    p.putFloat("a", m.a);
    p.putFloat("b", m.b);
    p.putFloat("c", m.c);
    p.putFloat("d", m.d);
    p.putFloat("e", m.e);
    p.putFloat("f", m.f);
    p.end();
    net::logf("[cal] saved a=%.4f b=%.4f c=%.2f d=%.4f e=%.4f f=%.2f",
              m.a, m.b, m.c, m.d, m.e, m.f);
}

Matrix current() { return s_m; }

bool is_default() { return s_default; }

void reset() {
    s_m = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    s_default = true;
    Preferences p;
    p.begin(NS, false);
    p.clear();
    p.end();
    net::logf("[cal] reset to identity (effective immediately)");
}

// Solve A x = b in the least-squares sense for A 2N x 6, b 2N x 1.
// Each sample contributes two rows:
//   target_x = a*raw_x + b*raw_y + c
//   target_y = d*raw_x + e*raw_y + f
//
// Decouples cleanly into two independent 3-parameter problems:
//   target_x_i = a*raw_x_i + b*raw_y_i + c
//   target_y_i = d*raw_x_i + e*raw_y_i + f
//
// Each is solved via the 3x3 normal equations A^T A x = A^T b.
static bool solve3(const Sample *samples, size_t n, bool x_axis,
                   float &k1, float &k2, float &k3) {
    // Build 3x3 normal matrix M = A^T A and 3-vector v = A^T b.
    double M[3][3] = {{0}};
    double v[3] = {0};
    for (size_t i = 0; i < n; ++i) {
        double rx = samples[i].raw_x;
        double ry = samples[i].raw_y;
        double t = x_axis ? samples[i].target_x : samples[i].target_y;
        // Row of A: [rx, ry, 1]
        M[0][0] += rx * rx;  M[0][1] += rx * ry;  M[0][2] += rx;
        M[1][0] += ry * rx;  M[1][1] += ry * ry;  M[1][2] += ry;
        M[2][0] += rx;       M[2][1] += ry;       M[2][2] += 1;
        v[0] += rx * t;
        v[1] += ry * t;
        v[2] += t;
    }
    // Gaussian elimination on 3x3.
    for (int i = 0; i < 3; ++i) {
        // Partial pivot
        int piv = i;
        for (int j = i + 1; j < 3; ++j) {
            if (fabs(M[j][i]) > fabs(M[piv][i])) piv = j;
        }
        if (piv != i) {
            for (int k = 0; k < 3; ++k) {
                double t = M[i][k]; M[i][k] = M[piv][k]; M[piv][k] = t;
            }
            double t = v[i]; v[i] = v[piv]; v[piv] = t;
        }
        if (fabs(M[i][i]) < 1e-9) return false;  // singular
        for (int j = i + 1; j < 3; ++j) {
            double r = M[j][i] / M[i][i];
            for (int k = i; k < 3; ++k) M[j][k] -= r * M[i][k];
            v[j] -= r * v[i];
        }
    }
    // Back substitute.
    double x[3];
    for (int i = 2; i >= 0; --i) {
        double s = v[i];
        for (int j = i + 1; j < 3; ++j) s -= M[i][j] * x[j];
        x[i] = s / M[i][i];
    }
    k1 = (float)x[0];
    k2 = (float)x[1];
    k3 = (float)x[2];
    return true;
}

// Plausibility bounds for a sane screen-touch affine matrix. Bounds
// must cover real-world panels that the IC reports with a compressed
// or expanded range. On the Sunton 4848S040 the GT911 reports Y as
// approximately raw 12..297 for visual 0..479 - a scale of ~1.68
// with no rotation/skew. The previous tight bound [0.5, 1.5] was
// rejecting that valid matrix and forcing the user back to identity.
//
// Widened bounds, with sanity still in place to reject obvious garbage
// (rotation, big offset, mirrored axes):
//   scale  : 0.4 .. 2.5  (handles up to ~2.5x compression)
//   skew   : +/- 0.4     (no significant rotation expected)
//   offset : +/- 400 px  (compressed ranges produce large offsets)
static bool plausible(const Matrix &m) {
    if (m.a < 0.4f || m.a > 2.5f) return false;
    if (m.e < 0.4f || m.e > 2.5f) return false;
    if (m.b < -0.4f || m.b > 0.4f) return false;
    if (m.d < -0.4f || m.d > 0.4f) return false;
    if (m.c < -400.0f || m.c > 400.0f) return false;
    if (m.f < -400.0f || m.f > 400.0f) return false;
    return true;
}

bool solve(const Sample *samples, size_t n, Matrix &out) {
    if (!samples || n < 3) return false;
    float a, b, c, d, e, f;
    if (!solve3(samples, n, true, a, b, c)) return false;
    if (!solve3(samples, n, false, d, e, f)) return false;
    Matrix candidate = {a, b, c, d, e, f};
    if (!plausible(candidate)) {
        net::logf("[cal] rejected implausible matrix "
                  "a=%.3f b=%.3f c=%.1f d=%.3f e=%.3f f=%.1f",
                  a, b, c, d, e, f);
        return false;
    }
    out = candidate;
    return true;
}

}  // namespace touch_cal
