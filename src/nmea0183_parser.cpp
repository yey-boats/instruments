#include "nmea0183.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace nmea0183 {

namespace {

inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Split a comma-separated payload into up to MAX tokens. Tokens may be
// empty. Writes into `buf` (which holds a NUL-terminated mutable copy
// of the payload). Returns count.
int split_csv(char *buf, char **out, int max) {
    int n = 0;
    out[n++] = buf;
    for (char *p = buf; *p && n < max; ++p) {
        if (*p == ',') {
            *p = 0;
            out[n++] = p + 1;
        }
    }
    return n;
}

// Atoi-with-empty-check returning NaN when blank.
double atof_or_nan(const char *s) {
    if (!s || !*s) return NAN;
    char *end = nullptr;
    double v = strtod(s, &end);
    if (end == s) return NAN;
    return v;
}

inline void push_field(ParseResult &r, FieldKind k, double v) {
    if (r.count >= sizeof(r.fields) / sizeof(r.fields[0])) return;
    r.fields[r.count++] = {k, v};
}

// Wrap apparent wind angle [0..360) into signed [-180..180).
inline double wrap_signed_deg(double a) {
    if (!isfinite(a)) return a;
    while (a < -180.0)
        a += 360.0;
    while (a >= 180.0)
        a -= 360.0;
    return a;
}

void parse_rmc(char **t, int n, ParseResult &r) {
    // t[0]=utc, t[1]=status, t[2]=lat, t[3]=ns, t[4]=lon, t[5]=ew,
    // t[6]=sog_kn, t[7]=cog_true, t[8]=date, t[9]=magvar, t[10]=ew
    if (n < 8) return;
    if (t[1][0] != 'A') return;  // V = warning, ignore
    double lat = parse_lat_lon(t[2], t[3][0]);
    double lon = parse_lat_lon(t[4], t[5][0]);
    double sog = atof_or_nan(t[6]);
    double cog = atof_or_nan(t[7]);
    if (isfinite(lat)) push_field(r, FieldKind::LatDeg, lat);
    if (isfinite(lon)) push_field(r, FieldKind::LonDeg, lon);
    if (isfinite(sog)) push_field(r, FieldKind::SogKn, sog);
    if (isfinite(cog)) push_field(r, FieldKind::CogTrueDeg, cog);
}

void parse_gga(char **t, int n, ParseResult &r) {
    // t[0]=utc, t[1]=lat, t[2]=ns, t[3]=lon, t[4]=ew, t[5]=fix
    if (n < 6) return;
    int fix = (int)atof_or_nan(t[5]);
    if (fix == 0) return;
    double lat = parse_lat_lon(t[1], t[2][0]);
    double lon = parse_lat_lon(t[3], t[4][0]);
    if (isfinite(lat)) push_field(r, FieldKind::LatDeg, lat);
    if (isfinite(lon)) push_field(r, FieldKind::LonDeg, lon);
}

void parse_vtg(char **t, int n, ParseResult &r) {
    // t[0]=cog_true, t[1]=T, t[2]=cog_mag, t[3]=M, t[4]=sog_kn, t[5]=N
    if (n < 6) return;
    double cog = atof_or_nan(t[0]);
    double sog = atof_or_nan(t[4]);
    if (isfinite(cog)) push_field(r, FieldKind::CogTrueDeg, cog);
    if (isfinite(sog)) push_field(r, FieldKind::SogKn, sog);
}

void parse_vhw(char **t, int n, ParseResult &r) {
    // t[0]=hdg_true, t[1]=T, t[2]=hdg_mag, t[3]=M, t[4]=stw_kn, t[5]=N
    if (n < 5) return;
    double ht = atof_or_nan(t[0]);
    double hm = atof_or_nan(t[2]);
    double stw = atof_or_nan(t[4]);
    if (isfinite(ht)) push_field(r, FieldKind::HeadingTrueDeg, ht);
    if (isfinite(hm)) push_field(r, FieldKind::HeadingMagDeg, hm);
    if (isfinite(stw)) push_field(r, FieldKind::StwKn, stw);
}

void parse_hdt(char **t, int n, ParseResult &r) {
    if (n < 1) return;
    double h = atof_or_nan(t[0]);
    if (isfinite(h)) push_field(r, FieldKind::HeadingTrueDeg, h);
}

void parse_hdg(char **t, int n, ParseResult &r) {
    if (n < 1) return;
    double h = atof_or_nan(t[0]);
    if (isfinite(h)) push_field(r, FieldKind::HeadingMagDeg, h);
}

void parse_mwv(char **t, int n, ParseResult &r) {
    // t[0]=wind_angle (0..360), t[1]=R/T, t[2]=speed, t[3]=K/M/N, t[4]=A/V
    if (n < 5) return;
    if (t[4][0] != 'A') return;  // status invalid
    double angle = atof_or_nan(t[0]);
    double speed = atof_or_nan(t[2]);
    if (isfinite(speed)) {
        // Convert to knots based on unit.
        switch (t[3][0]) {
        case 'N':
            break;  // already knots
        case 'K':
            speed *= 0.539957;
            break;  // km/h -> kn
        case 'M':
            speed *= 1.943844;
            break;  // m/s -> kn
        default:
            speed = NAN;
            break;
        }
    }
    double signed_angle = wrap_signed_deg(angle);
    bool true_wind = (t[1][0] == 'T');
    if (isfinite(signed_angle)) {
        push_field(r, true_wind ? FieldKind::TwaDeg : FieldKind::AwaDeg, signed_angle);
    }
    if (isfinite(speed)) {
        push_field(r, true_wind ? FieldKind::TwsKn : FieldKind::AwsKn, speed);
    }
}

void parse_dpt(char **t, int n, ParseResult &r) {
    if (n < 1) return;
    double d = atof_or_nan(t[0]);
    if (isfinite(d)) push_field(r, FieldKind::DepthM, d);
}

void parse_dbt(char **t, int n, ParseResult &r) {
    // ...,depth_ft,f,depth_m,M,depth_fath,F
    if (n < 3) return;
    double d = atof_or_nan(t[2]);
    if (isfinite(d)) push_field(r, FieldKind::DepthM, d);
}

void parse_mtw(char **t, int n, ParseResult &r) {
    if (n < 1) return;
    double t_c = atof_or_nan(t[0]);
    if (isfinite(t_c)) push_field(r, FieldKind::WaterTempC, t_c);
}

void parse_xte(char **t, int n, ParseResult &r) {
    // t[0]=A status, t[1]=A status, t[2]=value, t[3]=L/R, t[4]=units (N/K)
    if (n < 5) return;
    if (t[0][0] != 'A') return;
    double val = atof_or_nan(t[2]);
    if (!isfinite(val)) return;
    if (t[4][0] == 'K') val *= 0.539957;  // km -> nm
    if (t[3][0] == 'L') val = -val;
    push_field(r, FieldKind::XteNm, val);
}

void parse_bwc(char **t, int n, ParseResult &r) {
    // t[0]=utc, t[1]=lat, t[2]=ns, t[3]=lon, t[4]=ew,
    // t[5]=btw_true, t[6]=T, t[7]=btw_mag, t[8]=M,
    // t[9]=dtw_nm, t[10]=N
    if (n < 10) return;
    double btw = atof_or_nan(t[5]);
    double dtw = atof_or_nan(t[9]);
    if (isfinite(btw)) push_field(r, FieldKind::BtwTrueDeg, btw);
    if (isfinite(dtw)) push_field(r, FieldKind::DtwNm, dtw);
}

}  // namespace

size_t verify_checksum(const char *line, size_t len, uint8_t *cksum_out) {
    if (!line || len < 4) return 0;
    size_t start = (line[0] == '$' || line[0] == '!') ? 1 : 0;
    // Find '*'
    size_t star = (size_t)-1;
    for (size_t i = start; i < len; ++i) {
        if (line[i] == '*') {
            star = i;
            break;
        }
    }
    if (star == (size_t)-1 || star + 2 >= len) return 0;
    int hi = hex_nibble(line[star + 1]);
    int lo = hex_nibble(line[star + 2]);
    if (hi < 0 || lo < 0) return 0;
    uint8_t want = (uint8_t)((hi << 4) | lo);
    uint8_t have = 0;
    for (size_t i = start; i < star; ++i)
        have ^= (uint8_t)line[i];
    if (cksum_out) *cksum_out = have;
    if (want != have) return 0;
    return star - start;
}

double parse_lat_lon(const char *value, char hemi) {
    if (!value || !*value) return NAN;
    double raw = atof_or_nan(value);
    if (!isfinite(raw)) return NAN;
    double deg_int = floor(raw / 100.0);
    double minutes = raw - deg_int * 100.0;
    double deg = deg_int + minutes / 60.0;
    if (hemi == 'S' || hemi == 's' || hemi == 'W' || hemi == 'w') deg = -deg;
    return deg;
}

ParseResult parse_sentence(const char *line, size_t len) {
    ParseResult r{};
    r.count = 0;
    if (!line || len < 7) return r;
    size_t start = (line[0] == '$' || line[0] == '!') ? 1 : 0;
    size_t payload_len = verify_checksum(line, len, nullptr);
    if (payload_len < 6) return r;  // need at least "XXYYY,"
    // Talker = chars [start..start+1], sentence = [start+2..start+4]
    if (start + 5 > len) return r;
    r.talker[0] = line[start];
    r.talker[1] = line[start + 1];
    r.talker[2] = 0;
    r.sentence[0] = line[start + 2];
    r.sentence[1] = line[start + 3];
    r.sentence[2] = line[start + 4];
    r.sentence[3] = 0;
    // Copy fields part (after comma following sentence id) to mutable buffer.
    size_t star = start + payload_len;
    size_t fields_start = start + 5;
    if (fields_start >= star || line[fields_start] != ',') return r;
    fields_start++;
    size_t fields_len = star - fields_start;
    char buf[96];
    if (fields_len >= sizeof(buf)) fields_len = sizeof(buf) - 1;
    memcpy(buf, line + fields_start, fields_len);
    buf[fields_len] = 0;
    char *tok[20] = {nullptr};
    int n = split_csv(buf, tok, 20);

    const char *s = r.sentence;
    if (!strcmp(s, "RMC"))
        parse_rmc(tok, n, r);
    else if (!strcmp(s, "GGA"))
        parse_gga(tok, n, r);
    else if (!strcmp(s, "VTG"))
        parse_vtg(tok, n, r);
    else if (!strcmp(s, "VHW"))
        parse_vhw(tok, n, r);
    else if (!strcmp(s, "HDT"))
        parse_hdt(tok, n, r);
    else if (!strcmp(s, "HDG"))
        parse_hdg(tok, n, r);
    else if (!strcmp(s, "MWV"))
        parse_mwv(tok, n, r);
    else if (!strcmp(s, "DPT"))
        parse_dpt(tok, n, r);
    else if (!strcmp(s, "DBT"))
        parse_dbt(tok, n, r);
    else if (!strcmp(s, "MTW"))
        parse_mtw(tok, n, r);
    else if (!strcmp(s, "XTE"))
        parse_xte(tok, n, r);
    else if (!strcmp(s, "BWC"))
        parse_bwc(tok, n, r);
    else
        return r;  // unrecognized but checksum valid

    r.ok = true;
    return r;
}

void stream_init(Stream &s, Stream::Callback cb, void *user) {
    s.cb = cb;
    s.user = user;
    s.pos = 0;
    s.inside = false;
}

void stream_feed(Stream &s, const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = bytes[i];
        if (c == '$' || c == '!') {
            s.inside = true;
            s.pos = 0;
            s.buf[s.pos++] = (char)c;
            continue;
        }
        if (!s.inside) continue;
        if (c == '\r' || c == '\n') {
            if (s.pos > 0) {
                ParseResult r = parse_sentence(s.buf, s.pos);
                if (r.ok && s.cb) s.cb(r, s.user);
            }
            s.inside = false;
            s.pos = 0;
            continue;
        }
        if (s.pos >= sizeof(s.buf)) {
            // Overflow - reset.
            s.inside = false;
            s.pos = 0;
            continue;
        }
        s.buf[s.pos++] = (char)c;
    }
}

}  // namespace nmea0183
