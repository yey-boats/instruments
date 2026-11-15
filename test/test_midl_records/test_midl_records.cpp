// MIDL record store (docs/midl/midl-only.md): records, override rule, screen-id
// conflicts, carousel order, owner lookup, home screen, boot fallback chain,
// the index/record codecs, the manager-config `midl` (pull) block, the
// embedded built-in records, sha256 and the text-valued local ids.
#include <unity.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "generated_midl_manifest.h"
#include "generated_midl_builtin.h"
#include "midl_local_ids.h"
#include "midl_records.h"
#include "midl_validate.h"
#include "sha256_lite.h"
#include "trip_log.h"

#include <math.h>

// Every class's manifest + fitted built-in records (wrapped so the identical
// midl_manifest:: / midl_builtin:: names do not collide). The host build's own
// class (square-480) is the global one.
namespace cls_square_480 {
namespace midl_manifest = ::midl_manifest;
namespace midl_builtin = ::midl_builtin;
}  // namespace cls_square_480
namespace cls_landscape_800x480 {
#include "midl_gen/landscape-800x480/builtin_records.h"
#include "midl_gen/landscape-800x480/manifest.h"
}  // namespace cls_landscape_800x480
namespace cls_landscape_1024x600 {
#include "midl_gen/landscape-1024x600/builtin_records.h"
#include "midl_gen/landscape-1024x600/manifest.h"
}  // namespace cls_landscape_1024x600
namespace cls_round_360 {
#include "midl_gen/round-360/builtin_records.h"
#include "midl_gen/round-360/manifest.h"
}  // namespace cls_round_360

using namespace midl::records;

void setUp() {
}
void tearDown() {
}

static Store &store() {
    static Store s;  // ~48 KB: static, never on the test stack
    return s;
}

// Build a RecordInfo from "id" + comma-separated screen ids.
static RecordInfo &rec(const char *id, const char *screens, Source src = Source::Manager) {
    static RecordInfo r[4];
    static int k = 0;
    RecordInfo &out = r[k++ & 3];
    clear(out);
    snprintf(out.id, sizeof(out.id), "%s", id);
    snprintf(out.title, sizeof(out.title), "%s", id);
    snprintf(out.hash, sizeof(out.hash), "sha256:%s", id);
    out.source = src;
    std::string s(screens);
    size_t pos = 0;
    while (pos <= s.size() && *screens) {
        size_t c = s.find(',', pos);
        std::string one = s.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        snprintf(out.screens[out.screen_count].id, SCREEN_ID_LEN, "%s", one.c_str());
        snprintf(out.screens[out.screen_count].title, SCREEN_TITLE_LEN, "%s", one.c_str());
        ++out.screen_count;
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    return out;
}

static void seed_embedded() {
    Store &s = store();
    s.reset();
    TEST_ASSERT_TRUE(s.add_embedded(rec("dashboard", "dashboard", Source::Embedded)));
    TEST_ASSERT_TRUE(s.add_embedded(rec("wind", "wind", Source::Embedded)));
    TEST_ASSERT_TRUE(s.add_embedded(rec("status", "status", Source::Embedded)));
}

static std::string carousel_ids() {
    const RecordInfo *c[MAX_RECORDS];
    size_t n = store().carousel(c, MAX_RECORDS);
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        if (i) out += ",";
        out += c[i]->id;
    }
    return out;
}

static void test_valid_id() {
    TEST_ASSERT_TRUE(valid_id("wind"));
    TEST_ASSERT_TRUE(valid_id("a-b_c9"));
    TEST_ASSERT_TRUE(valid_id("abcdefghijabcdefghijabcdefghij12"));  // 32
    TEST_ASSERT_FALSE(valid_id("abcdefghijabcdefghijabcdefghij123"));
    TEST_ASSERT_FALSE(valid_id(""));
    TEST_ASSERT_FALSE(valid_id(nullptr));
    TEST_ASSERT_FALSE(valid_id("Wind"));
    TEST_ASSERT_FALSE(valid_id("a/b"));
    TEST_ASSERT_FALSE(valid_id("../x"));
}

static void test_override_and_restore() {
    seed_embedded();
    Store &s = store();
    TEST_ASSERT_EQUAL(Source::Embedded, s.effective("wind")->source);
    TEST_ASSERT_FALSE(s.overrides_embedded("wind"));
    TEST_ASSERT_TRUE(s.put(rec("wind", "wind", Source::Manager)));
    TEST_ASSERT_EQUAL(Source::Manager, s.effective("wind")->source);
    TEST_ASSERT_TRUE(s.overrides_embedded("wind"));
    // An override keeps the embedded position in the carousel.
    TEST_ASSERT_EQUAL_STRING("dashboard,wind,status", carousel_ids().c_str());
    TEST_ASSERT_TRUE(s.remove("wind"));
    TEST_ASSERT_EQUAL(Source::Embedded, s.effective("wind")->source);
    // A bare embedded record cannot be deleted.
    TEST_ASSERT_FALSE(s.remove("wind"));
    TEST_ASSERT_FALSE(s.put(rec("x", "x", Source::Embedded)));  // put is for stored only
}

static void test_conflicts() {
    seed_embedded();
    Store &s = store();
    char other[ID_LEN], sid[SCREEN_ID_LEN];
    // New record claiming an embedded screen id -> conflict.
    TEST_ASSERT_TRUE(
        s.find_conflict(rec("mine", "a,wind"), other, sizeof(other), sid, sizeof(sid)));
    TEST_ASSERT_EQUAL_STRING("wind", other);
    TEST_ASSERT_EQUAL_STRING("wind", sid);
    // Overriding the embedded record of the same id is not a conflict.
    TEST_ASSERT_FALSE(
        s.find_conflict(rec("wind", "wind,wind2"), other, sizeof(other), sid, sizeof(sid)));
    TEST_ASSERT_TRUE(s.put(rec("race", "start,vmg")));
    TEST_ASSERT_TRUE(s.find_conflict(rec("other", "vmg"), other, sizeof(other), sid, sizeof(sid)));
    TEST_ASSERT_EQUAL_STRING("race", other);
    TEST_ASSERT_EQUAL_STRING("vmg", sid);
    // Replacing "race" itself with the same screens is fine.
    TEST_ASSERT_FALSE(
        s.find_conflict(rec("race", "start,vmg"), other, sizeof(other), sid, sizeof(sid)));
}

static void test_owner_of() {
    seed_embedded();
    Store &s = store();
    TEST_ASSERT_TRUE(s.put(rec("race", "start,vmg")));
    TEST_ASSERT_EQUAL_STRING("race", s.owner_of("vmg")->id);
    TEST_ASSERT_EQUAL_STRING("wind", s.owner_of("wind")->id);
    TEST_ASSERT_NULL(s.owner_of("nope"));
    TEST_ASSERT_NULL(s.owner_of(""));
    // An override with different screens moves ownership.
    TEST_ASSERT_TRUE(s.put(rec("wind", "wind_a,wind_b")));
    TEST_ASSERT_NULL(s.owner_of("wind"));
    TEST_ASSERT_EQUAL_STRING("wind", s.owner_of("wind_b")->id);
}

static void test_carousel_order() {
    seed_embedded();
    Store &s = store();
    TEST_ASSERT_TRUE(s.put(rec("zeta", "z1")));
    TEST_ASSERT_TRUE(s.put(rec("alpha", "a1")));
    // No index: embedded order, then stored-only by id.
    TEST_ASSERT_EQUAL_STRING("dashboard,wind,status,alpha,zeta", carousel_ids().c_str());
    Index &ix = s.index();
    clear(ix);
    snprintf(ix.order[0], ID_LEN, "zeta");
    snprintf(ix.order[1], ID_LEN, "ghost");  // unknown: skipped
    snprintf(ix.order[2], ID_LEN, "wind");
    snprintf(ix.order[3], ID_LEN, "zeta");  // duplicate: skipped
    ix.order_count = 4;
    snprintf(ix.hidden[0], ID_LEN, "status");
    ix.hidden_count = 1;
    TEST_ASSERT_EQUAL_STRING("zeta,wind,dashboard,alpha", carousel_ids().c_str());
    TEST_ASSERT_EQUAL_STRING("wind", s.next_record("zeta")->id);
    TEST_ASSERT_EQUAL_STRING("zeta", s.next_record("alpha")->id);    // wraps
    TEST_ASSERT_EQUAL_STRING("alpha", s.prev_record("zeta")->id);    // wraps
    TEST_ASSERT_EQUAL_STRING("zeta", s.next_record("status")->id);   // hidden -> first
    TEST_ASSERT_EQUAL_STRING("alpha", s.prev_record("status")->id);  // hidden -> last
}

static void test_home_screen() {
    seed_embedded();
    Store &s = store();
    char sid[SCREEN_ID_LEN], owner[ID_LEN];
    s.home_screen(sid, sizeof(sid), owner, sizeof(owner));
    TEST_ASSERT_EQUAL_STRING("dashboard", sid);
    TEST_ASSERT_EQUAL_STRING("dashboard", owner);
    TEST_ASSERT_TRUE(s.put(rec("race", "start,vmg")));
    snprintf(s.index().default_screen, SCREEN_ID_LEN, "vmg");
    s.home_screen(sid, sizeof(sid), owner, sizeof(owner));
    TEST_ASSERT_EQUAL_STRING("vmg", sid);
    TEST_ASSERT_EQUAL_STRING("race", owner);
    // Default naming a missing screen -> first screen of the first visible record.
    snprintf(s.index().default_screen, SCREEN_ID_LEN, "gone");
    snprintf(s.index().hidden[0], ID_LEN, "dashboard");
    s.index().hidden_count = 1;
    s.home_screen(sid, sizeof(sid), owner, sizeof(owner));
    TEST_ASSERT_EQUAL_STRING("wind", sid);
    s.reset();
    s.home_screen(sid, sizeof(sid));
    TEST_ASSERT_EQUAL_STRING("", sid);
}

static void test_fallback_chain() {
    seed_embedded();
    Store &s = store();
    Candidate c[4];
    // Bare embedded record: embedded copy, then the default embedded record.
    size_t n = fallback_chain(s, "wind", c);
    TEST_ASSERT_EQUAL(2, (int)n);
    TEST_ASSERT_EQUAL(Copy::Embedded, c[0].copy);
    TEST_ASSERT_EQUAL(Copy::DefaultEmbedded, c[1].copy);
    TEST_ASSERT_EQUAL_STRING("dashboard", c[1].id);
    // Override: record -> LKG -> embedded same id -> default embedded.
    TEST_ASSERT_TRUE(s.put(rec("wind", "wind")));
    n = fallback_chain(s, "wind", c);
    TEST_ASSERT_EQUAL(4, (int)n);
    TEST_ASSERT_EQUAL(Copy::Stored, c[0].copy);
    TEST_ASSERT_EQUAL(Copy::Lkg, c[1].copy);
    TEST_ASSERT_EQUAL(Copy::Embedded, c[2].copy);
    TEST_ASSERT_EQUAL(Copy::DefaultEmbedded, c[3].copy);
    // Manager-only record: record -> LKG -> default embedded.
    TEST_ASSERT_TRUE(s.put(rec("race", "start")));
    n = fallback_chain(s, "race", c);
    TEST_ASSERT_EQUAL(3, (int)n);
    TEST_ASSERT_EQUAL(Copy::DefaultEmbedded, c[2].copy);
    // The default record itself: no duplicate default entry.
    n = fallback_chain(s, "dashboard", c);
    TEST_ASSERT_EQUAL(1, (int)n);
    TEST_ASSERT_EQUAL(Copy::Embedded, c[0].copy);
    TEST_ASSERT_EQUAL_STRING("dashboard", factory_default(s)->id);
}

static void test_describe_doc() {
    static RecordInfo r;
    static Error e;
    JsonDocument d;
    deserializeJson(d, R"({"midl":"1.0.0","screens":[{"id":"a","title":"A"},{"title":"x"},
                         {"id":"b"}]})");
    TEST_ASSERT_TRUE(describe_doc(d.as<JsonVariantConst>(), r, e));
    TEST_ASSERT_EQUAL(2, r.screen_count);
    TEST_ASSERT_EQUAL_STRING("A", r.screens[0].title);
    TEST_ASSERT_EQUAL_STRING("b", r.screens[1].title);  // title defaults to the id
    deserializeJson(d, R"({"screens":[{"id":"a"},{"id":"a"}]})");
    TEST_ASSERT_FALSE(describe_doc(d.as<JsonVariantConst>(), r, e));
    TEST_ASSERT_EQUAL_STRING("duplicate_screen_id", e.code);
    deserializeJson(d, R"({"screens":[]})");
    TEST_ASSERT_FALSE(describe_doc(d.as<JsonVariantConst>(), r, e));
    TEST_ASSERT_EQUAL(422, e.http);
}

static void test_index_codec() {
    static Index ix;
    static Error e;
    JsonDocument d;
    deserializeJson(d, R"({"order":["race","wind","race"],"defaultScreen":"vmg",
                         "hidden":["status"]})");
    TEST_ASSERT_TRUE(parse_index(d.as<JsonVariantConst>(), ix, e));
    TEST_ASSERT_EQUAL(2, ix.order_count);  // duplicate dropped
    TEST_ASSERT_EQUAL_STRING("vmg", ix.default_screen);
    TEST_ASSERT_EQUAL(1, ix.hidden_count);
    JsonDocument out;
    index_to_json(ix, out.to<JsonObject>());
    std::string s;
    serializeJson(out, s);
    TEST_ASSERT_EQUAL_STRING(
        R"({"order":["race","wind"],"defaultScreen":"vmg","hidden":["status"]})", s.c_str());
    deserializeJson(d, R"({"order":["Bad Id"]})");
    TEST_ASSERT_FALSE(parse_index(d.as<JsonVariantConst>(), ix, e));
    TEST_ASSERT_EQUAL(400, e.http);
    deserializeJson(d, R"([1])");
    TEST_ASSERT_FALSE(parse_index(d.as<JsonVariantConst>(), ix, e));
    deserializeJson(d, R"({})");
    TEST_ASSERT_TRUE(parse_index(d.as<JsonVariantConst>(), ix, e));
    TEST_ASSERT_EQUAL(0, ix.order_count);
}

static void test_record_codec() {
    static RecordInfo r;
    static Error e;
    JsonDocument d;
    deserializeJson(d, R"({"id":"race","title":"Race","source":"manager","hash":"sha256:ab",
                         "doc":{"midl":"1.0.0","screens":[{"id":"start","title":"Start"}]}})");
    JsonVariantConst doc;
    TEST_ASSERT_TRUE(parse_record(d.as<JsonVariantConst>(), r, doc, e));
    TEST_ASSERT_EQUAL_STRING("race", r.id);
    TEST_ASSERT_EQUAL(Source::Manager, r.source);
    TEST_ASSERT_EQUAL_STRING("sha256:ab", r.hash);
    TEST_ASSERT_EQUAL(1, r.screen_count);
    TEST_ASSERT_EQUAL_STRING("1.0.0", doc["midl"] | "");
    deserializeJson(d, R"({"id":"race"})");
    TEST_ASSERT_FALSE(parse_record(d.as<JsonVariantConst>(), r, doc, e));
    deserializeJson(d, R"({"id":"../x","doc":{}})");
    TEST_ASSERT_FALSE(parse_record(d.as<JsonVariantConst>(), r, doc, e));
}

static void test_pull_block() {
    static PullPlan p;
    static PullActions a;
    static Error e;
    seed_embedded();
    Store &s = store();
    TEST_ASSERT_TRUE(s.put(rec("old", "o1", Source::Manager)));
    TEST_ASSERT_TRUE(s.put(rec("mine", "m1", Source::Local)));
    TEST_ASSERT_TRUE(s.put(rec("race", "r1", Source::Manager)));
    JsonDocument d;
    deserializeJson(d, R"({"layout":{"ignored":true},"midl":{
        "index":{"order":["race","dashboard"],"defaultScreen":"r1"},
        "records":[
          {"id":"race","title":"Race","hash":"sha256:race","url":"/devices/d/midl/records/race"},
          {"id":"dashboard","hash":"sha256:dashboard","url":"/u"},
          {"id":"wind","hash":"sha256:new-wind","url":"/devices/d/midl/records/wind"},
          {"id":"fresh","hash":"sha256:fresh","url":"/devices/d/midl/records/fresh"}]}})");
    TEST_ASSERT_TRUE(parse_pull(d["midl"], p, e));
    TEST_ASSERT_TRUE(p.has_index);
    TEST_ASSERT_EQUAL_STRING("r1", p.index.default_screen);
    TEST_ASSERT_EQUAL(4, p.count);
    TEST_ASSERT_EQUAL_STRING("/devices/d/midl/records/wind", p.entries[2].url);
    plan_pull(s, p, a);
    // race: same hash (already pushed) -> no-op; dashboard: embedded listed by
    // reference with its own hash -> no-op; wind (new hash) + fresh -> fetch.
    TEST_ASSERT_EQUAL(2, a.fetch_count);
    TEST_ASSERT_EQUAL_STRING("wind", p.entries[a.fetch[0]].id);
    TEST_ASSERT_EQUAL_STRING("fresh", p.entries[a.fetch[1]].id);
    // "old" (manager, unlisted) goes; "mine" (local) and embedded stay.
    TEST_ASSERT_EQUAL(1, a.remove_count);
    TEST_ASSERT_EQUAL_STRING("old", a.remove[0]);

    TEST_ASSERT_FALSE(parse_pull(d["nope"], p, e));
    TEST_ASSERT_EQUAL_STRING("absent", e.code);
    deserializeJson(d, R"({"midl":{"records":[{"id":"x"}]}})");
    TEST_ASSERT_FALSE(parse_pull(d["midl"], p, e));
    TEST_ASSERT_EQUAL(400, e.http);
    deserializeJson(d, R"({"midl":{"records":{}}})");
    TEST_ASSERT_FALSE(parse_pull(d["midl"], p, e));
}

static void test_sha256() {
    char h[HASH_LEN];
    TEST_ASSERT_TRUE(sha256_lite::hash_tagged("", 0, h, sizeof(h)));
    TEST_ASSERT_EQUAL_STRING(
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", h);
    sha256_lite::hash_tagged("abc", 3, h, sizeof(h));
    TEST_ASSERT_EQUAL_STRING(
        "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", h);
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 bytes
    sha256_lite::hash_tagged(m, strlen(m), h, sizeof(h));
    TEST_ASSERT_EQUAL_STRING(
        "sha256:248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", h);
    TEST_ASSERT_FALSE(sha256_lite::hash_tagged("abc", 3, h, 10));
}

// Every embedded record of every class: id grammar, hash matches its bytes,
// the device re-serialization is byte-identical (so device and DM hashes
// agree), screen id == record id, no screen-id conflicts, and the doc is
// admissible for its class.
struct ClassSet {
    const char *class_id;
    const char *manifest;
    const midl_builtin::Record *records;  // layout-identical across the wrapped namespaces
    size_t count;
};

#define CLASS_SET(ns)                                                                              \
    {                                                                                              \
        ns::midl_manifest::CLASS_ID, ns::midl_manifest::JSON,                                      \
            reinterpret_cast<const midl_builtin::Record *>(ns::midl_builtin::RECORDS),             \
            ns::midl_builtin::COUNT                                                                \
    }

static void check_class(const ClassSet &c, std::string &failures) {
    // Knob views on the round class, the 11 built-in screens elsewhere.
    bool round = strcmp(c.class_id, "round-360") == 0;
    TEST_ASSERT_EQUAL(round ? 4 : 11, (int)c.count);
    TEST_ASSERT_EQUAL_STRING(round ? "ap_hud" : "dashboard", c.records[0].id);
    JsonDocument m;
    TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(m, c.manifest).code());
    Store &s = store();
    s.reset();
    for (size_t i = 0; i < c.count; ++i) {
        const midl_builtin::Record &r = c.records[i];
        TEST_ASSERT_TRUE_MESSAGE(valid_id(r.id), r.id);
        TEST_ASSERT_EQUAL(strlen(r.json), r.len);
        char h[HASH_LEN];
        sha256_lite::hash_tagged(r.json, r.len, h, sizeof(h));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(r.hash, h, r.id);
        JsonDocument doc;
        TEST_ASSERT_EQUAL(DeserializationError::Ok, deserializeJson(doc, r.json, r.len).code());
        std::string again;
        serializeJson(doc, again);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(r.json, again.c_str(), r.id);
        static RecordInfo info;
        static Error e;
        clear(info);
        snprintf(info.id, sizeof(info.id), "%s", r.id);
        TEST_ASSERT_TRUE_MESSAGE(describe_doc(doc.as<JsonVariantConst>(), info, e), r.id);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(r.id, info.screens[0].id, r.id);
        char other[ID_LEN], sid[SCREEN_ID_LEN];
        TEST_ASSERT_FALSE_MESSAGE(s.find_conflict(info, other, sizeof(other), sid, sizeof(sid)),
                                  r.id);
        TEST_ASSERT_TRUE(s.add_embedded(info));
        static midl::validate::Issue issue;
        memset(&issue, 0, sizeof(issue));
        bool ok = midl::validate::validate_document(doc.as<JsonVariantConst>(),
                                                    m.as<JsonVariantConst>(), c.class_id, issue);
        // Storage limits are class-independent except tiles/depth, which the
        // host build sizes for its own class: run the full admission there.
        if (ok && strcmp(c.class_id, ::midl_manifest::CLASS_ID) == 0)
            ok = midl::validate::admit(doc.as<JsonVariantConst>(), m.as<JsonVariantConst>(),
                                       c.class_id, issue);
        if (!ok) {
            failures += "\n  ";
            failures += r.id;
            failures += " @";
            failures += c.class_id;
            failures += ": ";
            failures += issue.path;
            failures += " ";
            failures += issue.message;
        }
    }
}

static void test_builtin_records() {
    const ClassSet sets[] = {CLASS_SET(cls_square_480), CLASS_SET(cls_landscape_800x480),
                             CLASS_SET(cls_landscape_1024x600), CLASS_SET(cls_round_360)};
    std::string failures;
    for (const ClassSet &c : sets)
        check_class(c, failures);
    TEST_ASSERT_TRUE_MESSAGE(failures.empty(), failures.c_str());
}

static void test_local_ids() {
    using namespace midl::local_ids;
    Snapshot s = {};
    s.ip = "10.42.0.14";
    s.ssid = "boat";
    s.rssi = -61;
    s.sk_state = "live";
    s.sk_host = "";
    s.build = "Nov 15 2026";
    s.version = "0.9.1";
    s.uptime_s = 3 * 86400 + 4 * 3600 + 5 * 60 + 6;
    s.heap_free = 123 * 1024;
    s.psram_free = 7864320;  // 7.5 MB
    s.device_id = "yeydisp-a1b2";
    s.underway_s = 2 * 3600 + 3 * 60 + 4;
    char out[32];
    TEST_ASSERT_TRUE(is_text_id("net.ip"));
    TEST_ASSERT_FALSE(is_text_id("heap"));
    TEST_ASSERT_FALSE(is_text_id(nullptr));
    format("net.ip", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("10.42.0.14", out);
    format("net.ssid", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("boat", out);
    format("net.rssi", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("-61 dBm", out);
    format("sk.state", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("live", out);
    format("sk.host", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("--", out);
    format("sys.build", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Nov 15 2026", out);
    format("sys.version", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0.9.1", out);
    format("sys.uptime", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("3d 04:05", out);
    format("sys.heap", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("123 KB", out);
    format("sys.psram", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("7.5 MB", out);
    format("device.id", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("yeydisp-a1b2", out);
    format("trip.underway", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2:03:04", out);
    TEST_ASSERT_FALSE(format("nope", s, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("--", out);
    s.rssi = 0;
    format("net.rssi", s, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("--", out);
    format_uptime(3725, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("01:02:05", out);
    char tiny[4];
    format("device.id", s, tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_STRING("yey", tiny);  // truncated, NUL-terminated
}

static void test_trip_log() {
    trip_log::Accum a = {};
    TEST_ASSERT_FALSE(trip_log::sample(a, 2.0, 1000));  // first sample only primes
    TEST_ASSERT_FALSE(trip_log::sample(a, 2.0, 1500));  // < 1 s: ignored
    trip_log::sample(a, 2.0, 2000);                     // 1 s underway at 2 m/s
    TEST_ASSERT_EQUAL_DOUBLE(2.0, a.dist_m);
    TEST_ASSERT_EQUAL(1, (int)a.underway_s);
    TEST_ASSERT_TRUE(isnan(trip_log::avg_kn(a)));  // < 5 s underway
    trip_log::sample(a, 0.1, 3000);                // drifting: not underway
    TEST_ASSERT_EQUAL(1, (int)a.underway_s);
    trip_log::sample(a, NAN, 9000);  // no SOG: nothing
    TEST_ASSERT_EQUAL(3000, (int)a.last_ms);
    for (uint32_t t = 4000; t <= 32000; t += 1000)
        trip_log::sample(a, 1852.0 / 3600.0, t);  // 1 kn
    TEST_ASSERT_EQUAL(30, (int)a.underway_s);
    TEST_ASSERT_TRUE(trip_log::max_kn(a) > 3.88 && trip_log::max_kn(a) < 3.89);  // 2 m/s
    TEST_ASSERT_TRUE(trip_log::avg_kn(a) > 1.0);
    TEST_ASSERT_TRUE(trip_log::distance_nm(a) > 0.008 && trip_log::distance_nm(a) < 0.0093);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_id);
    RUN_TEST(test_override_and_restore);
    RUN_TEST(test_conflicts);
    RUN_TEST(test_owner_of);
    RUN_TEST(test_carousel_order);
    RUN_TEST(test_home_screen);
    RUN_TEST(test_fallback_chain);
    RUN_TEST(test_describe_doc);
    RUN_TEST(test_index_codec);
    RUN_TEST(test_record_codec);
    RUN_TEST(test_pull_block);
    RUN_TEST(test_sha256);
    RUN_TEST(test_builtin_records);
    RUN_TEST(test_local_ids);
    RUN_TEST(test_trip_log);
    return UNITY_END();
}
