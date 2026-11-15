// MIDL record store, device half: persistence + store operations + reports.
// No LVGL here (see midl_store_ui.cpp for activation/navigation). See
// include/midl_store.h and docs/midl/midl-only.md.
//
// MEMORY: everything over a few bytes lives in one PSRAM State block
// (psram_new); doc/record buffers are PSRAM heap_caps allocations; JSON trees
// use the shared PSRAM allocator. Callers run on small task stacks (web task,
// manager worker), so no RecordInfo/PullPlan/Store is ever a stack local.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_store.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "app_events.h"
#include "generated_midl_builtin.h"
#include "generated_midl_manifest.h"
#include "midl_gen/midl_commit.h"
#include "midl_validate.h"
#include "net.h"
#include "psram_json.h"
#include "psram_static.h"
#include "sha256_lite.h"
#include "ui_screens.h"

namespace midl::store {

using namespace midl::records;

namespace {

constexpr const char *DIR_ROOT = "/midl";
constexpr const char *DIR_RECORDS = "/midl/records";
constexpr const char *INDEX_PATH = "/midl/index.json";

struct State {
    Store store;
    RecordInfo scratch;  // candidate record under construction (lock held)
    char lkg_id[MAX_STORED][ID_LEN];
    char lkg_hash[MAX_STORED][HASH_LEN];
    uint8_t lkg_n;
    char active[ID_LEN];
    char manifest_hash[HASH_LEN];
    bool fs_ok;
};

State &st() {
    static State &s = *yeyboats::psram_new<State>();
    return s;
}

SemaphoreHandle_t s_mtx = nullptr;

struct Lock {
    Lock() {
        if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex();
        xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
    }
    ~Lock() { xSemaphoreGiveRecursive(s_mtx); }
};

void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

void fail(Result &r, int http, const char *code, const char *msg) {
    r.http = http;
    copy_str(r.code, sizeof(r.code), code);
    copy_str(r.message, sizeof(r.message), msg);
}

void record_path(char *out, size_t cap, const char *id, bool lkg) {
    snprintf(out, cap, "%s/%s%s", DIR_RECORDS, id, lkg ? ".lkg.json" : ".json");
}

// Read a whole file into a NUL-terminated PSRAM buffer (caller frees).
char *read_file(const char *path, size_t *len) {
    *len = 0;
    if (!st().fs_ok || !LittleFS.exists(path)) return nullptr;
    File f = LittleFS.open(path, "r");
    if (!f) return nullptr;
    size_t sz = f.size();
    if (sz == 0 || sz > MAX_DOC_BYTES + 4096) {
        f.close();
        return nullptr;
    }
    char *buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        f.close();
        return nullptr;
    }
    size_t got = f.read((uint8_t *)buf, sz);
    f.close();
    if (got != sz) {
        heap_caps_free(buf);
        return nullptr;
    }
    buf[sz] = 0;
    *len = sz;
    return buf;
}

// Write via <path>.tmp + rename so a power cut never leaves a torn record.
bool write_file(const char *path, const char *a, size_t alen, const char *b = nullptr,
                size_t blen = 0, const char *c = nullptr, size_t clen = 0) {
    if (!st().fs_ok) return false;
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;
    size_t want = alen + blen + clen;
    size_t wrote = f.write((const uint8_t *)a, alen);
    if (b) wrote += f.write((const uint8_t *)b, blen);
    if (c) wrote += f.write((const uint8_t *)c, clen);
    f.close();
    if (wrote != want) {
        LittleFS.remove(tmp);
        return false;
    }
    if (LittleFS.exists(path)) LittleFS.remove(path);
    return LittleFS.rename(tmp, path);
}

const midl_builtin::Record *builtin(const char *id) {
    for (size_t i = 0; i < midl_builtin::COUNT; ++i)
        if (strcmp(midl_builtin::RECORDS[i].id, id) == 0) return &midl_builtin::RECORDS[i];
    return nullptr;
}

const char *lkg_hash_of(const char *id) {
    State &s = st();
    for (uint8_t i = 0; i < s.lkg_n; ++i)
        if (strcmp(s.lkg_id[i], id) == 0) return s.lkg_hash[i];
    return "";
}

void set_lkg_hash(const char *id, const char *hash) {
    State &s = st();
    for (uint8_t i = 0; i < s.lkg_n; ++i) {
        if (strcmp(s.lkg_id[i], id) != 0) continue;
        if (hash) {
            copy_str(s.lkg_hash[i], HASH_LEN, hash);
        } else {
            for (uint8_t j = i + 1; j < s.lkg_n; ++j) {
                memcpy(s.lkg_id[j - 1], s.lkg_id[j], ID_LEN);
                memcpy(s.lkg_hash[j - 1], s.lkg_hash[j], HASH_LEN);
            }
            --s.lkg_n;
        }
        return;
    }
    if (!hash || s.lkg_n >= MAX_STORED) return;
    copy_str(s.lkg_id[s.lkg_n], ID_LEN, id);
    copy_str(s.lkg_hash[s.lkg_n], HASH_LEN, hash);
    ++s.lkg_n;
}

bool save_index() {
    JsonDocument d(&yeyboats::psram_json);
    index_to_json(st().store.index(), d.to<JsonObject>());
    String out;
    serializeJson(d, out);
    return write_file(INDEX_PATH, out.c_str(), out.length());
}

void index_hash(char *out, size_t cap) {
    JsonDocument d(&yeyboats::psram_json);
    index_to_json(st().store.index(), d.to<JsonObject>());
    String s;
    serializeJson(d, s);
    sha256_lite::hash_tagged(s.c_str(), s.length(), out, cap);
}

void load_embedded() {
    State &s = st();
    for (size_t i = 0; i < midl_builtin::COUNT; ++i) {
        const midl_builtin::Record &b = midl_builtin::RECORDS[i];
        JsonDocument d(&yeyboats::psram_json);
        if (deserializeJson(d, b.json, b.len)) {
            net::logf("[midl-store] embedded %s: parse failed", b.id);
            continue;
        }
        static Error e;
        clear(s.scratch);
        copy_str(s.scratch.id, ID_LEN, b.id);
        copy_str(s.scratch.title, TITLE_LEN, b.title);
        copy_str(s.scratch.hash, HASH_LEN, b.hash);
        s.scratch.source = Source::Embedded;
        if (!describe_doc(d.as<JsonVariantConst>(), s.scratch, e) ||
            !s.store.add_embedded(s.scratch))
            net::logf("[midl-store] embedded %s rejected: %s", b.id, e.message);
    }
}

void load_flash() {
    State &s = st();
    size_t len = 0;
    char *buf = read_file(INDEX_PATH, &len);
    if (buf) {
        JsonDocument d(&yeyboats::psram_json);
        static Error e;
        if (deserializeJson(d, buf, len) ||
            !parse_index(d.as<JsonVariantConst>(), s.store.index(), e))
            net::logf("[midl-store] index unreadable; using defaults");
        heap_caps_free(buf);
    }
    File dir = LittleFS.open(DIR_RECORDS);
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        char name[64];
        copy_str(name, sizeof(name), f.name());
        f.close();
        // f.name() is the basename on arduino-esp32 2.x; strip any path.
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        size_t n = strlen(base);
        bool lkg = n > 9 && strcmp(base + n - 9, ".lkg.json") == 0;
        bool rec = !lkg && n > 5 && strcmp(base + n - 5, ".json") == 0;
        if (!lkg && !rec) continue;
        char id[ID_LEN];
        size_t idlen = n - (lkg ? 9 : 5);
        if (idlen >= ID_LEN) continue;
        memcpy(id, base, idlen);
        id[idlen] = 0;
        if (!valid_id(id)) continue;
        char path[80];
        record_path(path, sizeof(path), id, lkg);
        char *body = read_file(path, &len);
        if (!body) continue;
        JsonDocument d(&yeyboats::psram_json);
        DeserializationError err = deserializeJson(d, body, len);
        heap_caps_free(body);
        if (err) {
            net::logf("[midl-store] %s: parse failed (%s)", path, err.c_str());
            continue;
        }
        if (lkg) {
            set_lkg_hash(id, d["hash"] | "");
            continue;
        }
        static Error e;
        JsonVariantConst doc;
        if (!parse_record(d.as<JsonVariantConst>(), s.scratch, doc, e) ||
            strcmp(s.scratch.id, id) != 0 || s.scratch.source == Source::Embedded ||
            !s.store.put(s.scratch)) {
            net::logf("[midl-store] %s: skipped (%s)", path, e.message);
            continue;
        }
    }
}

void remove_files(const char *id) {
    char path[80];
    record_path(path, sizeof(path), id, false);
    if (LittleFS.exists(path)) LittleFS.remove(path);
    record_path(path, sizeof(path), id, true);
    if (LittleFS.exists(path)) LittleFS.remove(path);
    set_lkg_hash(id, nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------

JsonVariantConst manifest() {
    static JsonDocument *m = nullptr;
    Lock l;
    if (!m) {
        m = new JsonDocument(&yeyboats::psram_json);
        if (deserializeJson(*m, midl_manifest::JSON)) m->clear();
    }
    return m->as<JsonVariantConst>();
}

void setup() {
    Lock l;
    State &s = st();
    s.store.reset();
    s.lkg_n = 0;
    s.active[0] = 0;
    sha256_lite::hash_tagged(midl_manifest::JSON, strlen(midl_manifest::JSON), s.manifest_hash,
                             sizeof(s.manifest_hash));
    // format-on-fail, same as the manager's applied-config persistence.
    s.fs_ok = LittleFS.begin(true);
    if (s.fs_ok) {
        if (!LittleFS.exists(DIR_ROOT)) LittleFS.mkdir(DIR_ROOT);
        if (!LittleFS.exists(DIR_RECORDS)) LittleFS.mkdir(DIR_RECORDS);
    } else {
        net::logf("[midl-store] LittleFS mount failed: embedded records only");
    }
    load_embedded();
    if (s.fs_ok) load_flash();
    net::logf("[midl-store] %u embedded + %u stored record(s), class %s",
              (unsigned)s.store.embedded_count(), (unsigned)s.store.stored_count(),
              midl_builtin::CLASS_ID);
}

bool put_record(const char *id, const char *title, JsonVariantConst doc, Source src, Result &r,
                const char *hash) {
    memset(&r, 0, sizeof(r));
    if (!valid_id(id)) {
        fail(r, 400, "bad_id", "record id must match [a-z0-9_-]{1,32}");
        return false;
    }
    if (src == Source::Embedded) {
        fail(r, 400, "bad_source", "embedded records are built in");
        return false;
    }
    if (hash && hash[0] && strncmp(hash, "sha256:", 7) != 0) {
        fail(r, 400, "bad_hash", "hash must be sha256:<hex>");
        return false;
    }
    if (!doc.is<JsonObjectConst>()) {
        fail(r, 400, "bad_doc", "body needs a 'doc' object");
        return false;
    }
    size_t doc_len = measureJson(doc);
    if (doc_len > MAX_DOC_BYTES) {
        fail(r, 413, "too_large", "midl doc too large (48 KB max)");
        return false;
    }
    {
        static midl::validate::Issue issue;  // serialized by the store lock below
        Lock l;
        memset(&issue, 0, sizeof(issue));
        if (!midl::validate::admit(doc, manifest(), midl_manifest::CLASS_ID, issue)) {
            fail(r, 422, "invalid_doc", issue.message);
            copy_str(r.path, sizeof(r.path), issue.path);
            return false;
        }
    }
    char *bytes = (char *)heap_caps_malloc(doc_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bytes) {
        fail(r, 503, "no_memory", "doc buffer alloc failed");
        return false;
    }
    serializeJson(doc, bytes, doc_len + 1);

    bool ok = false;
    bool was_active = false;
    {
        Lock l;
        State &s = st();
        static Error e;
        RecordInfo &cand = s.scratch;
        clear(cand);
        copy_str(cand.id, ID_LEN, id);
        cand.source = src;
        if (!describe_doc(doc, cand, e)) {
            fail(r, e.http, e.code, e.message);
        } else if (s.store.find_conflict(cand, r.other, sizeof(r.other), r.screen,
                                         sizeof(r.screen))) {
            char m[160];
            snprintf(m, sizeof(m), "screen '%s' already belongs to record '%s'", r.screen, r.other);
            fail(r, 409, "screen_id_conflict", m);
        } else if (!s.store.stored(id) && s.store.stored_count() >= MAX_STORED) {
            fail(r, 507, "store_full", "too many stored records");
        } else {
            copy_str(cand.title, TITLE_LEN, (title && title[0]) ? title : cand.screens[0].title);
            bool supplied = src == Source::Manager && hash && strncmp(hash, "sha256:", 7) == 0 &&
                            strlen(hash) < sizeof(cand.hash);
            if (supplied)
                copy_str(cand.hash, sizeof(cand.hash), hash);  // DM's JSON.stringify hash
            else
                sha256_lite::hash_tagged(bytes, doc_len, cand.hash, sizeof(cand.hash));
            JsonDocument head(&yeyboats::psram_json);
            head["id"] = cand.id;
            head["title"] = cand.title;
            head["source"] = source_name(cand.source);
            head["hash"] = cand.hash;
            String prefix;
            serializeJson(head, prefix);
            prefix.remove(prefix.length() - 1);  // drop '}' -> append the doc
            prefix += ",\"doc\":";
            char path[80];
            record_path(path, sizeof(path), id, false);
            if (!write_file(path, prefix.c_str(), prefix.length(), bytes, doc_len, "}", 1)) {
                fail(r, 500, "write_failed", "flash write failed");
            } else if (!s.store.put(cand)) {
                fail(r, 507, "store_full", "too many stored records");
            } else {
                ok = true;
                r.http = 200;
                copy_str(r.hash, sizeof(r.hash), cand.hash);
                was_active = strcmp(s.active, id) == 0;
                net::logf("[midl-store] stored %s record '%s' (%u bytes, %u screen(s))",
                          source_name(src), id, (unsigned)doc_len, (unsigned)cand.screen_count);
            }
        }
    }
    heap_caps_free(bytes);
    if (ok && was_active) request_activate(id, nullptr);  // re-render the live record
    return ok;
}

bool delete_record(const char *id, Result &r) {
    memset(&r, 0, sizeof(r));
    bool was_active = false;
    {
        Lock l;
        State &s = st();
        if (!valid_id(id) || !s.store.stored(id)) {
            bool emb = valid_id(id) && s.store.embedded(id);
            fail(r, 404, emb ? "embedded_record" : "not_found",
                 emb ? "built-in record (nothing to delete)" : "no such record");
            return false;
        }
        remove_files(id);
        s.store.remove(id);
        was_active = strcmp(s.active, id) == 0;
        r.http = 200;
        net::logf("[midl-store] deleted record '%s'%s", id,
                  s.store.embedded(id) ? " (embedded restored)" : "");
        if (was_active && !s.store.embedded(id)) s.active[0] = 0;
    }
    if (was_active) {
        char act[ID_LEN];
        active(act, sizeof(act));
        request_activate(act[0] ? act : "", nullptr);  // embedded restore, or home
    }
    return true;
}

bool put_index(JsonVariantConst v, Result &r) {
    memset(&r, 0, sizeof(r));
    Lock l;
    static Index ix;
    static Error e;
    if (!parse_index(v, ix, e)) {
        fail(r, e.http, e.code, e.message);
        return false;
    }
    memcpy(&st().store.index(), &ix, sizeof(Index));
    if (!save_index()) {
        fail(r, 500, "write_failed", "flash write failed");
        return false;
    }
    r.http = 200;
    index_hash(r.hash, sizeof(r.hash));
    return true;
}

void factory_reset() {
    {
        Lock l;
        State &s = st();
        if (s.fs_ok) {
            File dir = LittleFS.open(DIR_RECORDS);
            constexpr size_t MAX_FILES = MAX_STORED * 2 + 8;  // record + lkg + strays
            struct Name {
                char s[64];
            };
            static Name *names = yeyboats::psram_new<Name>(MAX_FILES);
            size_t n = 0;
            if (dir && dir.isDirectory()) {
                for (File f = dir.openNextFile(); f && n < MAX_FILES; f = dir.openNextFile()) {
                    const char *nm = f.name();
                    const char *base = strrchr(nm, '/');
                    snprintf(names[n++].s, sizeof(Name::s), "%s/%s", DIR_RECORDS,
                             base ? base + 1 : nm);
                    f.close();
                }
            }
            for (size_t i = 0; i < n; ++i)
                LittleFS.remove(names[i].s);
            if (LittleFS.exists(INDEX_PATH)) LittleFS.remove(INDEX_PATH);
        }
        // Keep embedded records; drop stored ones + the index.
        const RecordInfo *recs[MAX_RECORDS];
        size_t n = s.store.all(recs, MAX_RECORDS);
        struct Id {
            char s[ID_LEN];
        };
        static Id *ids = yeyboats::psram_new<Id>(MAX_RECORDS);
        size_t m = 0;
        for (size_t i = 0; i < n; ++i)
            if (s.store.stored(recs[i]->id)) copy_str(ids[m++].s, ID_LEN, recs[i]->id);
        for (size_t i = 0; i < m; ++i)
            s.store.remove(ids[i].s);
        clear(s.store.index());
        s.lkg_n = 0;
        s.active[0] = 0;
        net::logf("[midl-store] factory reset: %u stored record(s) removed", (unsigned)m);
    }
    request_activate("", "");
}

bool record_json(const char *id, String &out) {
    Lock l;
    State &s = st();
    if (!valid_id(id)) return false;
    if (s.store.stored(id)) {
        char path[80];
        record_path(path, sizeof(path), id, false);
        size_t len = 0;
        char *buf = read_file(path, &len);
        if (!buf) return false;
        out = buf;
        heap_caps_free(buf);
        return true;
    }
    const midl_builtin::Record *b = builtin(id);
    if (!b) return false;
    JsonDocument head(&yeyboats::psram_json);
    head["id"] = b->id;
    head["title"] = b->title;
    head["source"] = "embedded";
    head["hash"] = b->hash;
    serializeJson(head, out);
    out.remove(out.length() - 1);
    out += ",\"doc\":";
    out.concat(b->json, b->len);
    out += "}";
    return true;
}

void list_json(JsonObject out) {
    Lock l;
    State &s = st();
    out["active"] = s.active;
    out["screen"] = ui::current_id();
    out["midlCommit"] = midl_gen::MIDL_COMMIT;
    index_to_json(s.store.index(), out["index"].to<JsonObject>());
    JsonArray arr = out["records"].to<JsonArray>();
    const RecordInfo *recs[MAX_RECORDS];
    size_t n = s.store.all(recs, MAX_RECORDS);
    for (size_t i = 0; i < n; ++i) {
        const RecordInfo &r = *recs[i];
        JsonObject o = arr.add<JsonObject>();
        o["id"] = r.id;
        o["title"] = r.title;
        o["source"] = source_name(r.source);
        o["hash"] = r.hash;
        o["overridesEmbedded"] = s.store.overrides_embedded(r.id);
        JsonArray sc = o["screens"].to<JsonArray>();
        for (uint8_t k = 0; k < r.screen_count; ++k) {
            JsonObject so = sc.add<JsonObject>();
            so["id"] = r.screens[k].id;
            so["title"] = r.screens[k].title;
        }
    }
}

void heartbeat_ui(JsonObject ui_o) {
    Lock l;
    State &s = st();
    ui_o["record"] = s.active;
    JsonArray arr = ui_o["screens"].to<JsonArray>();
    const RecordInfo *recs[MAX_RECORDS];
    size_t n = s.store.carousel(recs, MAX_RECORDS);
    for (size_t i = 0; i < n; ++i) {
        for (uint8_t k = 0; k < recs[i]->screen_count; ++k) {
            JsonObject so = arr.add<JsonObject>();
            so["id"] = recs[i]->screens[k].id;
            so["title"] = recs[i]->screens[k].title;
            so["record"] = recs[i]->id;
        }
    }
}

void heartbeat_midl(JsonObject m) {
    Lock l;
    State &s = st();
    m["version"] = manifest()["midl"] | "1.0.0";
    m["class"] = midl_manifest::CLASS_ID;
    m["commit"] = midl_gen::MIDL_COMMIT;
    char h[HASH_LEN];
    index_hash(h, sizeof(h));
    m["indexHash"] = h;
    JsonArray arr = m["records"].to<JsonArray>();
    const RecordInfo *recs[MAX_RECORDS];
    size_t n = s.store.all(recs, MAX_RECORDS);
    for (size_t i = 0; i < n; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = recs[i]->id;
        o["hash"] = recs[i]->hash;
        o["source"] = source_name(recs[i]->source);
    }
}

void identity_caps(JsonObject m) {
    Lock l;
    m["version"] = manifest()["midl"] | "1.0.0";
    m["class"] = midl_manifest::CLASS_ID;
    m["manifestHash"] = st().manifest_hash;
    m["commit"] = midl_gen::MIDL_COMMIT;
    m["editor"] = "/editor";  // device-hosted MIDL editor page (web.cpp)
    m["push"] = true;
    m["pull"] = true;
    // Trend history capability (points/maxSeries/maxWindowS/windows/backfill),
    // the manifest's own object so the two can never disagree.
    JsonVariantConst hist = manifest()["history"];
    if (hist.is<JsonObjectConst>()) m["history"] = hist;
}

bool plan_pull(JsonVariantConst blk, PullPlan &plan, PullActions &acts, Error &err) {
    Lock l;
    if (!parse_pull(blk, plan, err)) return false;
    records::plan_pull(st().store, plan, acts);
    return true;
}

bool apply_pull_index(const PullPlan &plan) {
    if (!plan.has_index) return true;
    Lock l;
    memcpy(&st().store.index(), &plan.index, sizeof(Index));
    return save_index();
}

bool request_activate(const char *record_id, const char *screen_id) {
    app::Command c;
    c.type = app::CommandType::MidlActivate;
    copy_str(c.a, sizeof(c.a), record_id);
    copy_str(c.b, sizeof(c.b), screen_id);
    c.i = screen_id ? 0 : 1;  // 1 = keep the current screen when still present
    bool ok = app::post(c, 50);
    if (!ok) net::logf("[midl-store] activate '%s' not queued (ui queue full)", c.a);
    return ok;
}

JsonVariantConst load_doc(const Candidate &c, JsonDocument &holder) {
    holder.clear();
    if (c.copy == Copy::Stored || c.copy == Copy::Lkg) {
        char path[80];
        record_path(path, sizeof(path), c.id, c.copy == Copy::Lkg);
        size_t len = 0;
        char *buf;
        {
            Lock l;
            buf = read_file(path, &len);
        }
        if (!buf) return JsonVariantConst();
        DeserializationError err = deserializeJson(holder, buf, len);
        heap_caps_free(buf);
        if (err) return JsonVariantConst();
        return holder["doc"];
    }
    const midl_builtin::Record *b = builtin(c.id);
    if (!b || deserializeJson(holder, b->json, b->len)) return JsonVariantConst();
    return holder.as<JsonVariantConst>();
}

size_t fallback_chain(const char *record_id, Candidate out[4]) {
    Lock l;
    return records::fallback_chain(st().store, record_id, out);
}

void promote_lkg(const char *id) {
    Lock l;
    State &s = st();
    const RecordInfo *r = s.store.stored(id);
    if (!r || strcmp(lkg_hash_of(id), r->hash) == 0) return;
    char path[80], lkg[80];
    record_path(path, sizeof(path), id, false);
    record_path(lkg, sizeof(lkg), id, true);
    size_t len = 0;
    char *buf = read_file(path, &len);
    if (!buf) return;
    if (write_file(lkg, buf, len)) {
        set_lkg_hash(id, r->hash);
        net::logf("[midl-store] '%s' rendered: promoted to last-known-good", id);
    }
    heap_caps_free(buf);
}

bool owner_of(const char *screen_id, char *out, size_t cap) {
    Lock l;
    const RecordInfo *r = st().store.owner_of(screen_id);
    copy_str(out, cap, r ? r->id : "");
    return r != nullptr;
}

void home(char *screen_out, size_t screen_cap, char *owner_out, size_t owner_cap) {
    Lock l;
    st().store.home_screen(screen_out, screen_cap, owner_out, owner_cap);
}

bool neighbour(const char *record_id, bool forward, char *rec_out, size_t rec_cap, char *screen_out,
               size_t screen_cap) {
    Lock l;
    Store &s = st().store;
    const RecordInfo *r = forward ? s.next_record(record_id) : s.prev_record(record_id);
    if (!r || !r->screen_count) return false;
    copy_str(rec_out, rec_cap, r->id);
    copy_str(screen_out, screen_cap, r->screens[forward ? 0 : r->screen_count - 1].id);
    return true;
}

size_t visible_count() {
    Lock l;
    const RecordInfo *recs[MAX_RECORDS];
    return st().store.carousel(recs, MAX_RECORDS);
}

void set_active(const char *id) {
    Lock l;
    copy_str(st().active, ID_LEN, id);
}

void active(char *out, size_t cap) {
    Lock l;
    copy_str(out, cap, st().active);
}

}  // namespace midl::store
