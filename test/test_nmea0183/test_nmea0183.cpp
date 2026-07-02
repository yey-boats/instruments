#include <unity.h>
#include <cmath>
#include <cstring>

#include "n2k_decode.h"  // pure NMEA2000 PGN decoders (header-only, host-clean)
#include "nmea0183.h"

using namespace nmea0183;

void setUp(void) {
}
void tearDown(void) {
}

static const FieldUpdate *find_field(const ParseResult &r, FieldKind k) {
    for (int i = 0; i < r.count; ++i) {
        if (r.fields[i].kind == k) return &r.fields[i];
    }
    return nullptr;
}

static void test_checksum_valid() {
    const char *line = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    uint8_t c = 0;
    size_t payload = verify_checksum(line, strlen(line), &c);
    TEST_ASSERT_GREATER_THAN_size_t(0, payload);
    TEST_ASSERT_EQUAL_UINT8(0x6A, c);
}

static void test_checksum_invalid() {
    const char *line = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*00";
    TEST_ASSERT_EQUAL_size_t(0, verify_checksum(line, strlen(line), nullptr));
}

static void test_lat_lon_conversion() {
    // 48 deg 07.038 min N = 48.1173
    double lat = parse_lat_lon("4807.038", 'N');
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 48.1173, lat);
    double lat_s = parse_lat_lon("4807.038", 'S');
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, -48.1173, lat_s);
    double lon = parse_lat_lon("01131.000", 'E');
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 11.51666, lon);
    double lon_w = parse_lat_lon("01131.000", 'W');
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, -11.51666, lon_w);
    TEST_ASSERT_TRUE(std::isnan(parse_lat_lon("", 'N')));
}

static void test_rmc() {
    const char *line = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("GP", r.talker);
    TEST_ASSERT_EQUAL_STRING("RMC", r.sentence);
    const FieldUpdate *sog = find_field(r, FieldKind::SogKn);
    TEST_ASSERT_NOT_NULL(sog);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 22.4, sog->value);
    const FieldUpdate *cog = find_field(r, FieldKind::CogTrueDeg);
    TEST_ASSERT_NOT_NULL(cog);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 84.4, cog->value);
    TEST_ASSERT_NOT_NULL(find_field(r, FieldKind::LatDeg));
    TEST_ASSERT_NOT_NULL(find_field(r, FieldKind::LonDeg));
}

static void test_rmc_void_status_ignored() {
    // Status field 'V' = warning - lat/lon/sog/cog should NOT be emitted.
    const char *line = "$GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,230394,,*06";
    ParseResult r = parse_sentence(line, strlen(line));
    // ok = true (recognized, valid checksum) but no fields.
    TEST_ASSERT_EQUAL_INT(0, r.count);
}

static void test_vhw() {
    // Heading true 045, mag 047, STW 5.5 kn
    const char *line = "$IIVHW,045.0,T,047.0,M,5.5,N,10.2,K*64";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("VHW", r.sentence);
    const FieldUpdate *ht = find_field(r, FieldKind::HeadingTrueDeg);
    TEST_ASSERT_NOT_NULL(ht);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 45.0, ht->value);
    const FieldUpdate *hm = find_field(r, FieldKind::HeadingMagDeg);
    TEST_ASSERT_NOT_NULL(hm);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 47.0, hm->value);
    const FieldUpdate *stw = find_field(r, FieldKind::StwKn);
    TEST_ASSERT_NOT_NULL(stw);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 5.5, stw->value);
}

static void test_mwv_apparent_knots() {
    // AWA = 045, AWS = 10 kn
    const char *line = "$IIMWV,045.0,R,10.0,N,A*0D";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    const FieldUpdate *awa = find_field(r, FieldKind::AwaDeg);
    TEST_ASSERT_NOT_NULL(awa);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 45.0, awa->value);
    const FieldUpdate *aws = find_field(r, FieldKind::AwsKn);
    TEST_ASSERT_NOT_NULL(aws);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 10.0, aws->value);
}

static void test_mwv_apparent_negative_wrap() {
    // 315 deg should wrap to -45.
    const char *line = "$IIMWV,315.0,R,8.0,N,A*32";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    const FieldUpdate *awa = find_field(r, FieldKind::AwaDeg);
    TEST_ASSERT_NOT_NULL(awa);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, -45.0, awa->value);
}

static void test_mwv_true_ms() {
    // True wind, 5 m/s = ~9.71944 kn
    const char *line = "$IIMWV,090.0,T,5.0,M,A*34";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    const FieldUpdate *twa = find_field(r, FieldKind::TwaDeg);
    TEST_ASSERT_NOT_NULL(twa);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 90.0, twa->value);
    const FieldUpdate *tws = find_field(r, FieldKind::TwsKn);
    TEST_ASSERT_NOT_NULL(tws);
    TEST_ASSERT_DOUBLE_WITHIN(0.05, 9.72, tws->value);
}

static void test_dpt() {
    const char *line = "$SDDPT,12.3,0.5,*4E";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    const FieldUpdate *d = find_field(r, FieldKind::DepthM);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 12.3, d->value);
}

static void test_dbt() {
    const char *line = "$SDDBT,15.5,f,4.72,M,2.58,F*39";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    const FieldUpdate *d = find_field(r, FieldKind::DepthM);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 4.72, d->value);
}

static void test_xte_steer_left() {
    // 0.50 nm, L = steer right? Spec says: indicator for which side to
    // steer to compensate. We pick a convention: L -> negative.
    const char *line = "$GPXTE,A,A,0.50,L,N*6B";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    const FieldUpdate *x = find_field(r, FieldKind::XteNm);
    TEST_ASSERT_NOT_NULL(x);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, -0.5, x->value);
}

static void test_unknown_sentence() {
    // ZDA = time/date - not in our table. Should return ok=false.
    const char *line = "$GPZDA,123456,01,01,2026,00,00*49";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_FALSE(r.ok);
}

static void test_bad_checksum() {
    const char *line = "$IIVHW,045.0,T,047.0,M,5.5,N,10.2,K*FF";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_FALSE(r.ok);
}

// --- BUG-2: HDG variation + RMC variation ---

static void test_hdg_with_variation() {
    // $HCHDG,<mag>,<dev>,<devEW>,<var>,<varEW>. 7.1 W -> -7.1 (+E convention).
    const char *line = "$HCHDG,101.1,,,7.1,W*3C";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    const FieldUpdate *hm = find_field(r, FieldKind::HeadingMagDeg);
    TEST_ASSERT_NOT_NULL(hm);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 101.1, hm->value);
    const FieldUpdate *var = find_field(r, FieldKind::MagVarDeg);
    TEST_ASSERT_NOT_NULL(var);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, -7.1, var->value);
    // HDG is magnetic-only: it must NOT emit a true heading.
    TEST_ASSERT_NULL(find_field(r, FieldKind::HeadingTrueDeg));
}

static void test_hdg_without_variation() {
    const char *line = "$HCHDG,45.0,,,,*73";
    ParseResult r = parse_sentence(line, strlen(line));
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_NOT_NULL(find_field(r, FieldKind::HeadingMagDeg));
    TEST_ASSERT_NULL(find_field(r, FieldKind::MagVarDeg));
}

static void test_rmc_variation_west_and_east() {
    // Existing RMC fixture carries 003.1,W -> -3.1.
    const char *w = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    ParseResult rw = parse_sentence(w, strlen(w));
    TEST_ASSERT_TRUE(rw.ok);
    const FieldUpdate *vw = find_field(rw, FieldKind::MagVarDeg);
    TEST_ASSERT_NOT_NULL(vw);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, -3.1, vw->value);
    // East variation stays positive.
    const char *e = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,E*78";
    ParseResult re = parse_sentence(e, strlen(e));
    TEST_ASSERT_TRUE(re.ok);
    const FieldUpdate *ve = find_field(re, FieldKind::MagVarDeg);
    TEST_ASSERT_NOT_NULL(ve);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 3.1, ve->value);
}

// --- NMEA2000 pure PGN decoders (n2k_decode.h) ---

static void test_n2k_127250_true_reference() {
    // heading 1.5000 rad, no dev/var, reference byte 0 = true.
    uint8_t d[8] = {0, 0x98, 0x3A, 0xFF, 0x7F, 0xFF, 0x7F, 0x00};  // 0x3A98 = 15000
    n2k::Heading127250 h = n2k::decode_127250(d, 8);
    TEST_ASSERT_EQUAL_UINT8(0, h.reference);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 1.5, h.heading_rad);
    TEST_ASSERT_TRUE(std::isnan(h.variation_rad));
}

static void test_n2k_127250_magnetic_with_variation() {
    // heading 1.0000 rad magnetic, variation +0.0500 rad, ref 1 = magnetic.
    uint8_t d[8] = {0, 0x10, 0x27, 0xFF, 0x7F, 0xF4, 0x01, 0x01};  // 10000, var 500
    n2k::Heading127250 h = n2k::decode_127250(d, 8);
    TEST_ASSERT_EQUAL_UINT8(1, h.reference);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 1.0, h.heading_rad);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 0.05, h.variation_rad);
}

static void test_n2k_127250_unavailable_heading() {
    uint8_t d[8] = {0, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0x7F, 0x00};
    n2k::Heading127250 h = n2k::decode_127250(d, 8);
    TEST_ASSERT_TRUE(std::isnan(h.heading_rad));
}

static void test_n2k_127251_rate_of_turn() {
    // rate = 1000000 * 3.125e-8 = 0.03125 rad/s. s32le at d+1.
    uint8_t d[8] = {0, 0x40, 0x42, 0x0F, 0x00, 0xFF, 0xFF, 0xFF};  // 0x000F4240
    double rot = n2k::decode_127251_rot_radps(d, 8);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.03125, rot);
    // Negative (port turn) and not-available sentinels.
    uint8_t neg[8] = {0, 0xC0, 0xBD, 0xF0, 0xFF, 0xFF, 0xFF, 0xFF};  // -1000000
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, -0.03125, n2k::decode_127251_rot_radps(neg, 8));
    uint8_t na[8] = {0, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_TRUE(std::isnan(n2k::decode_127251_rot_radps(na, 8)));
}

static void test_n2k_127257_attitude() {
    // yaw 0.5000, pitch 0.0349 (349), roll -0.0872 (-872) rad.
    uint8_t d[8] = {0, 0x88, 0x13, 0x5D, 0x01, 0x98, 0xFC, 0xFF};
    n2k::Attitude127257 a = n2k::decode_127257(d, 8);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 0.5, a.yaw_rad);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 0.0349, a.pitch_rad);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, -0.0872, a.roll_rad);
}

static void test_n2k_127245_rudder() {
    // instance 0, order NA, position +0.2000 rad (2000 = 0x07D0) starboard.
    uint8_t d[8] = {0, 0xFF, 0xFF, 0x7F, 0xD0, 0x07, 0xFF, 0xFF};
    n2k::Rudder127245 r = n2k::decode_127245(d, 8);
    TEST_ASSERT_EQUAL_UINT8(0, r.instance);
    TEST_ASSERT_TRUE(std::isnan(r.angle_order_rad));
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 0.2, r.position_rad);
}

static void test_n2k_127488_engine_rapid() {
    // instance 0, speed raw 7200 * 0.25 = 1800 RPM = 30 Hz.
    uint8_t d[8] = {0, 0x20, 0x1C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    n2k::EngineRapid127488 e = n2k::decode_127488(d, 8);
    TEST_ASSERT_EQUAL_UINT8(0, e.instance);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 30.0, e.rev_hz);
    uint8_t na[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_TRUE(std::isnan(n2k::decode_127488(na, 8).rev_hz));
}

static void test_n2k_127489_engine_dynamic_payload() {
    // Reassembled 26-byte payload: instance 0, oil 3500 hPa-units (350000 Pa),
    // oil temp NA, coolant 36115 * 0.01 = 361.15 K, alt NA, fuel rate 52 * 0.1
    // = 5.2 L/h, hours 7200 s.
    uint8_t p[26];
    memset(p, 0xFF, sizeof(p));
    p[0] = 0;  // instance
    p[1] = 0xAC;
    p[2] = 0x0D;  // oil pressure raw 3500 -> 350000 Pa
    p[3] = 0xFF;
    p[4] = 0xFF;  // oil temp NA
    p[5] = 0x13;
    p[6] = 0x8D;  // coolant raw 0x8D13 = 36115 -> 361.15 K
    p[7] = 0xFF;
    p[8] = 0x7F;  // alternator NA
    p[9] = 0x34;
    p[10] = 0x00;  // fuel rate raw 52 -> 5.2 L/h
    p[11] = 0x20;
    p[12] = 0x1C;
    p[13] = 0;
    p[14] = 0;  // hours 7200 s
    n2k::EngineDynamic127489 e = n2k::decode_127489(p, sizeof(p));
    TEST_ASSERT_EQUAL_UINT8(0, e.instance);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 350000.0, e.oil_pressure_pa);
    TEST_ASSERT_TRUE(std::isnan(e.oil_temp_k));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 361.15, e.coolant_temp_k);
    // 5.2 L/h = 5.2e-3 m3 / 3600 s
    TEST_ASSERT_DOUBLE_WITHIN(1e-10, 5.2e-3 / 3600.0, e.fuel_rate_m3s);
    TEST_ASSERT_DOUBLE_WITHIN(0.5, 7200.0, e.engine_hours_s);
}

static void test_n2k_fastpacket_reassembly() {
    // A 26-byte 127489 payload split into frames of 6+7+7+6 data bytes.
    uint8_t payload[26];
    for (int i = 0; i < 26; ++i)
        payload[i] = (uint8_t)i;
    n2k::FastPacket fp;
    uint8_t f0[8] = {0x40,       26,         payload[0], payload[1],
                     payload[2], payload[3], payload[4], payload[5]};  // seq 2, frame 0
    TEST_ASSERT_FALSE(n2k::fastpacket_feed(fp, f0, 8));
    uint8_t f1[8] = {0x41,       payload[6],  payload[7],  payload[8],
                     payload[9], payload[10], payload[11], payload[12]};
    TEST_ASSERT_FALSE(n2k::fastpacket_feed(fp, f1, 8));
    uint8_t f2[8] = {0x42,        payload[13], payload[14], payload[15],
                     payload[16], payload[17], payload[18], payload[19]};
    TEST_ASSERT_FALSE(n2k::fastpacket_feed(fp, f2, 8));
    uint8_t f3[8] = {0x43,        payload[20], payload[21], payload[22],
                     payload[23], payload[24], payload[25], 0xFF};
    TEST_ASSERT_TRUE(n2k::fastpacket_feed(fp, f3, 8));
    TEST_ASSERT_EQUAL_UINT8(26, fp.expect);
    TEST_ASSERT_EQUAL_MEMORY(payload, fp.buf, 26);
}

static void test_n2k_fastpacket_out_of_order_resets() {
    n2k::FastPacket fp;
    uint8_t f0[8] = {0x00, 26, 1, 2, 3, 4, 5, 6};
    TEST_ASSERT_FALSE(n2k::fastpacket_feed(fp, f0, 8));
    uint8_t f2[8] = {0x02, 9, 9, 9, 9, 9, 9, 9};  // frame 2 arrives before 1
    TEST_ASSERT_FALSE(n2k::fastpacket_feed(fp, f2, 8));
    uint8_t f1[8] = {0x01, 9, 9, 9, 9, 9, 9, 9};  // stream was reset -> ignored
    TEST_ASSERT_FALSE(n2k::fastpacket_feed(fp, f1, 8));
}

// --- BUG-3: PGN 130306 wind reference mapping ---

static void mk_wind(uint8_t *d, uint16_t speed_raw, uint16_t angle_raw, uint8_t ref) {
    d[0] = 0;
    d[1] = (uint8_t)(speed_raw & 0xFF);
    d[2] = (uint8_t)(speed_raw >> 8);
    d[3] = (uint8_t)(angle_raw & 0xFF);
    d[4] = (uint8_t)(angle_raw >> 8);
    d[5] = ref;
    d[6] = 0xFF;
    d[7] = 0xFF;
}

static void test_n2k_wind_ref2_is_apparent() {
    uint8_t d[8];
    mk_wind(d, 800, 7854, 2);  // 8.00 m/s, 0.7854 rad
    n2k::Wind130306 w = n2k::decode_130306(d, 8);
    TEST_ASSERT_EQUAL(n2k::WindTarget::Apparent, w.target);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 8.0, w.speed_mps);
    TEST_ASSERT_DOUBLE_WITHIN(1e-4, 0.7854, w.angle_rad);
}

static void test_n2k_wind_ref0_and_ref3_are_true() {
    uint8_t d[8];
    mk_wind(d, 500, 10000, 0);  // true, ground referenced
    TEST_ASSERT_EQUAL(n2k::WindTarget::True, n2k::decode_130306(d, 8).target);
    mk_wind(d, 500, 10000, 3);  // true, boat referenced
    TEST_ASSERT_EQUAL(n2k::WindTarget::True, n2k::decode_130306(d, 8).target);
}

static void test_n2k_wind_ref1_and_unknown_dropped() {
    uint8_t d[8];
    mk_wind(d, 500, 10000, 1);  // magnetic ground direction: no field -> drop
    TEST_ASSERT_EQUAL(n2k::WindTarget::Drop, n2k::decode_130306(d, 8).target);
    mk_wind(d, 500, 10000, 5);  // reserved -> drop, NEVER folded into TWA
    TEST_ASSERT_EQUAL(n2k::WindTarget::Drop, n2k::decode_130306(d, 8).target);
}

// --- Stream tests ---

struct CapturedSentence {
    char sentence[4];
    uint8_t count;
    FieldUpdate fields[8];
};

struct Cap {
    CapturedSentence list[8];
    int n;
};

static void cap_cb(const ParseResult &r, void *user) {
    Cap *c = (Cap *)user;
    if (c->n >= 8) return;
    CapturedSentence &s = c->list[c->n++];
    memcpy(s.sentence, r.sentence, 4);
    s.count = r.count;
    for (int i = 0; i < r.count; ++i)
        s.fields[i] = r.fields[i];
}

static void test_stream_two_sentences() {
    Stream s;
    Cap cap{};
    stream_init(s, cap_cb, &cap);
    const char *blob = "$IIVHW,045.0,T,047.0,M,5.5,N,10.2,K*64\r\n"
                       "$IIMWV,090.0,R,8.0,N,A*3C\r\n";
    stream_feed(s, (const uint8_t *)blob, strlen(blob));
    TEST_ASSERT_EQUAL_INT(2, cap.n);
    TEST_ASSERT_EQUAL_STRING("VHW", cap.list[0].sentence);
    TEST_ASSERT_EQUAL_STRING("MWV", cap.list[1].sentence);
}

static void test_stream_chunked_feed() {
    Stream s;
    Cap cap{};
    stream_init(s, cap_cb, &cap);
    const char *blob = "$IIVHW,045.0,T,047.0,M,5.5,N,10.2,K*64\r\n";
    // Feed one byte at a time.
    for (size_t i = 0; i < strlen(blob); ++i) {
        uint8_t b = (uint8_t)blob[i];
        stream_feed(s, &b, 1);
    }
    TEST_ASSERT_EQUAL_INT(1, cap.n);
    TEST_ASSERT_EQUAL_STRING("VHW", cap.list[0].sentence);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_checksum_valid);
    RUN_TEST(test_checksum_invalid);
    RUN_TEST(test_lat_lon_conversion);
    RUN_TEST(test_rmc);
    RUN_TEST(test_rmc_void_status_ignored);
    RUN_TEST(test_vhw);
    RUN_TEST(test_mwv_apparent_knots);
    RUN_TEST(test_mwv_apparent_negative_wrap);
    RUN_TEST(test_mwv_true_ms);
    RUN_TEST(test_dpt);
    RUN_TEST(test_dbt);
    RUN_TEST(test_xte_steer_left);
    RUN_TEST(test_unknown_sentence);
    RUN_TEST(test_bad_checksum);
    RUN_TEST(test_hdg_with_variation);
    RUN_TEST(test_hdg_without_variation);
    RUN_TEST(test_rmc_variation_west_and_east);
    RUN_TEST(test_n2k_127250_true_reference);
    RUN_TEST(test_n2k_127250_magnetic_with_variation);
    RUN_TEST(test_n2k_127250_unavailable_heading);
    RUN_TEST(test_n2k_127251_rate_of_turn);
    RUN_TEST(test_n2k_127257_attitude);
    RUN_TEST(test_n2k_127245_rudder);
    RUN_TEST(test_n2k_127488_engine_rapid);
    RUN_TEST(test_n2k_127489_engine_dynamic_payload);
    RUN_TEST(test_n2k_fastpacket_reassembly);
    RUN_TEST(test_n2k_fastpacket_out_of_order_resets);
    RUN_TEST(test_n2k_wind_ref2_is_apparent);
    RUN_TEST(test_n2k_wind_ref0_and_ref3_are_true);
    RUN_TEST(test_n2k_wind_ref1_and_unknown_dropped);
    RUN_TEST(test_stream_two_sentences);
    RUN_TEST(test_stream_chunked_feed);
    return UNITY_END();
}
