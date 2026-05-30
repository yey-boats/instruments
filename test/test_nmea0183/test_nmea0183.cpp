#include <unity.h>
#include <cmath>
#include <cstring>

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
    RUN_TEST(test_stream_two_sentences);
    RUN_TEST(test_stream_chunked_feed);
    return UNITY_END();
}
