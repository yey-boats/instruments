// MIDL record store, pure half. See include/midl_records.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_records.h"

#include <stdio.h>
#include <string.h>

namespace midl::records {

namespace {

void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void set_err(Error &e, int http, const char *code, const char *msg) {
    e.http = http;
    copy_str(e.code, sizeof(e.code), code);
    copy_str(e.message, sizeof(e.message), msg);
}

bool eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

bool has_screen(const RecordInfo &r, const char *sid) {
    for (uint8_t i = 0; i < r.screen_count; ++i)
        if (eq(r.screens[i].id, sid)) return true;
    return false;
}

bool parse_id_list(JsonVariantConst v, char (*out)[ID_LEN], uint8_t &count, Error &err,
                   const char *what) {
    count = 0;
    if (v.isNull()) return true;
    JsonArrayConst a = v.as<JsonArrayConst>();
    if (a.isNull()) {
        char m[64];
        snprintf(m, sizeof(m), "index.%s must be an array", what);
        set_err(err, 400, "bad_index", m);
        return false;
    }
    for (JsonVariantConst e : a) {
        const char *id = e.as<const char *>();
        if (!valid_id(id)) {
            char m[64];
            snprintf(m, sizeof(m), "index.%s: bad record id", what);
            set_err(err, 400, "bad_index", m);
            return false;
        }
        bool dup = false;
        for (uint8_t i = 0; i < count; ++i)
            if (eq(out[i], id)) dup = true;
        if (dup) continue;
        if (count >= MAX_RECORDS) break;
        copy_str(out[count++], ID_LEN, id);
    }
    return true;
}

}  // namespace

const char *source_name(Source s) {
    switch (s) {
    case Source::Embedded:
        return "embedded";
    case Source::Manager:
        return "manager";
    case Source::Local:
        return "local";
    }
    return "local";
}

bool parse_source(const char *s, Source &out) {
    if (eq(s, "embedded")) {
        out = Source::Embedded;
    } else if (eq(s, "manager")) {
        out = Source::Manager;
    } else if (eq(s, "local")) {
        out = Source::Local;
    } else {
        return false;
    }
    return true;
}

bool valid_id(const char *id) {
    if (!id || !id[0]) return false;
    size_t n = 0;
    for (const char *p = id; *p; ++p, ++n) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= ID_LEN - 1) return false;
    }
    return true;
}

void clear(Index &ix) {
    memset(&ix, 0, sizeof(ix));
}

void clear(RecordInfo &r) {
    memset(&r, 0, sizeof(r));
}

bool describe_doc(JsonVariantConst doc, RecordInfo &info, Error &err) {
    info.screen_count = 0;
    JsonArrayConst screens = doc["screens"].as<JsonArrayConst>();
    if (screens.isNull()) {
        set_err(err, 422, "no_screens", "doc has no 'screens' array");
        return false;
    }
    for (JsonVariantConst s : screens) {
        const char *sid = s["id"] | (const char *)nullptr;
        if (!sid || !sid[0]) continue;
        if (strlen(sid) >= SCREEN_ID_LEN) {
            set_err(err, 422, "screen_id_too_long", sid);
            return false;
        }
        if (has_screen(info, sid)) {
            set_err(err, 422, "duplicate_screen_id", sid);
            return false;
        }
        if (info.screen_count >= MAX_SCREENS_PER_RECORD) break;
        ScreenRef &r = info.screens[info.screen_count++];
        copy_str(r.id, sizeof(r.id), sid);
        copy_str(r.title, sizeof(r.title), s["title"] | sid);
    }
    if (info.screen_count == 0) {
        set_err(err, 422, "no_screens", "doc has no screen with an id");
        return false;
    }
    return true;
}

bool parse_index(JsonVariantConst v, Index &out, Error &err) {
    clear(out);
    if (!v.is<JsonObjectConst>()) {
        set_err(err, 400, "bad_index", "index must be an object");
        return false;
    }
    if (!parse_id_list(v["order"], out.order, out.order_count, err, "order")) return false;
    if (!parse_id_list(v["hidden"], out.hidden, out.hidden_count, err, "hidden")) return false;
    const char *def = v["defaultScreen"] | "";
    if (strlen(def) >= SCREEN_ID_LEN) {
        set_err(err, 400, "bad_index", "index.defaultScreen too long");
        return false;
    }
    copy_str(out.default_screen, sizeof(out.default_screen), def);
    return true;
}

void index_to_json(const Index &ix, JsonObject out) {
    JsonArray o = out["order"].to<JsonArray>();
    for (uint8_t i = 0; i < ix.order_count; ++i)
        o.add(ix.order[i]);
    out["defaultScreen"] = ix.default_screen;
    JsonArray h = out["hidden"].to<JsonArray>();
    for (uint8_t i = 0; i < ix.hidden_count; ++i)
        h.add(ix.hidden[i]);
}

// ---- Store --------------------------------------------------------------

void Store::reset() {
    n_embedded_ = 0;
    n_stored_ = 0;
    clear(index_);
}

bool Store::add_embedded(const RecordInfo &info) {
    if (n_embedded_ >= MAX_EMBEDDED || !valid_id(info.id)) return false;
    if (embedded(info.id)) return false;
    memcpy(&embedded_[n_embedded_], &info, sizeof(RecordInfo));
    embedded_[n_embedded_].source = Source::Embedded;
    ++n_embedded_;
    return true;
}

bool Store::put(const RecordInfo &info) {
    if (!valid_id(info.id) || info.source == Source::Embedded) return false;
    for (uint8_t i = 0; i < n_stored_; ++i) {
        if (eq(stored_[i].id, info.id)) {
            memcpy(&stored_[i], &info, sizeof(RecordInfo));
            return true;
        }
    }
    if (n_stored_ >= MAX_STORED) return false;
    memcpy(&stored_[n_stored_++], &info, sizeof(RecordInfo));
    return true;
}

bool Store::remove(const char *id) {
    for (uint8_t i = 0; i < n_stored_; ++i) {
        if (!eq(stored_[i].id, id)) continue;
        for (uint8_t j = i + 1; j < n_stored_; ++j)
            memcpy(&stored_[j - 1], &stored_[j], sizeof(RecordInfo));
        --n_stored_;
        return true;
    }
    return false;
}

const RecordInfo *Store::stored(const char *id) const {
    for (uint8_t i = 0; i < n_stored_; ++i)
        if (eq(stored_[i].id, id)) return &stored_[i];
    return nullptr;
}

const RecordInfo *Store::embedded(const char *id) const {
    int p = embedded_pos(id);
    return p < 0 ? nullptr : &embedded_[p];
}

int Store::embedded_pos(const char *id) const {
    for (uint8_t i = 0; i < n_embedded_; ++i)
        if (eq(embedded_[i].id, id)) return i;
    return -1;
}

const RecordInfo *Store::effective(const char *id) const {
    const RecordInfo *s = stored(id);
    return s ? s : embedded(id);
}

size_t Store::all(const RecordInfo **out, size_t cap) const {
    size_t n = 0;
    for (uint8_t i = 0; i < n_embedded_ && n < cap; ++i)
        out[n++] = effective(embedded_[i].id);
    // Stored-only records, by id (insertion sort into the tail).
    size_t tail = n;
    for (uint8_t i = 0; i < n_stored_ && n < cap; ++i) {
        if (embedded_pos(stored_[i].id) >= 0) continue;
        size_t at = n;
        while (at > tail && strcmp(out[at - 1]->id, stored_[i].id) > 0) {
            out[at] = out[at - 1];
            --at;
        }
        out[at] = &stored_[i];
        ++n;
    }
    return n;
}

bool Store::is_hidden(const char *id) const {
    for (uint8_t i = 0; i < index_.hidden_count; ++i)
        if (eq(index_.hidden[i], id)) return true;
    return false;
}

bool Store::find_conflict(const RecordInfo &cand, char *other_record, size_t other_cap,
                          char *screen_id, size_t screen_cap) const {
    const RecordInfo *recs[MAX_RECORDS];
    size_t n = all(recs, MAX_RECORDS);
    for (size_t r = 0; r < n; ++r) {
        if (eq(recs[r]->id, cand.id)) continue;  // the record being replaced
        for (uint8_t i = 0; i < cand.screen_count; ++i) {
            if (!has_screen(*recs[r], cand.screens[i].id)) continue;
            copy_str(other_record, other_cap, recs[r]->id);
            copy_str(screen_id, screen_cap, cand.screens[i].id);
            return true;
        }
    }
    return false;
}

const RecordInfo *Store::owner_of(const char *screen_id) const {
    if (!screen_id || !screen_id[0]) return nullptr;
    const RecordInfo *recs[MAX_RECORDS];
    size_t n = all(recs, MAX_RECORDS);
    for (size_t r = 0; r < n; ++r)
        if (has_screen(*recs[r], screen_id)) return recs[r];
    return nullptr;
}

size_t Store::carousel(const RecordInfo **out, size_t cap) const {
    size_t n = 0;
    auto listed = [&](const char *id) {
        for (size_t i = 0; i < n; ++i)
            if (eq(out[i]->id, id)) return true;
        return false;
    };
    for (uint8_t i = 0; i < index_.order_count && n < cap; ++i) {
        const RecordInfo *r = effective(index_.order[i]);
        if (!r || is_hidden(r->id) || listed(r->id)) continue;
        out[n++] = r;
    }
    const RecordInfo *recs[MAX_RECORDS];
    size_t m = all(recs, MAX_RECORDS);
    for (size_t i = 0; i < m && n < cap; ++i) {
        if (is_hidden(recs[i]->id) || listed(recs[i]->id)) continue;
        out[n++] = recs[i];
    }
    return n;
}

const RecordInfo *Store::next_record(const char *cur) const {
    const RecordInfo *c[MAX_RECORDS];
    size_t n = carousel(c, MAX_RECORDS);
    if (!n) return nullptr;
    for (size_t i = 0; i < n; ++i)
        if (eq(c[i]->id, cur)) return c[(i + 1) % n];
    return c[0];
}

const RecordInfo *Store::prev_record(const char *cur) const {
    const RecordInfo *c[MAX_RECORDS];
    size_t n = carousel(c, MAX_RECORDS);
    if (!n) return nullptr;
    for (size_t i = 0; i < n; ++i)
        if (eq(c[i]->id, cur)) return c[(i + n - 1) % n];
    return c[n - 1];
}

void Store::home_screen(char *out, size_t cap, char *owner_out, size_t owner_cap) const {
    copy_str(out, cap, "");
    if (owner_out) copy_str(owner_out, owner_cap, "");
    const RecordInfo *owner = owner_of(index_.default_screen);
    const char *sid = owner ? index_.default_screen : nullptr;
    if (!owner) {
        const RecordInfo *c[MAX_RECORDS];
        size_t n = carousel(c, MAX_RECORDS);
        if (!n) n = all(c, MAX_RECORDS);
        for (size_t i = 0; i < n && !owner; ++i) {
            if (c[i]->screen_count) {
                owner = c[i];
                sid = c[i]->screens[0].id;
            }
        }
    }
    if (!owner) return;
    copy_str(out, cap, sid);
    if (owner_out) copy_str(owner_out, owner_cap, owner->id);
}

// ---- fallback chain -----------------------------------------------------

const char *copy_name(Copy c) {
    switch (c) {
    case Copy::Stored:
        return "record";
    case Copy::Lkg:
        return "lkg";
    case Copy::Embedded:
        return "embedded";
    case Copy::DefaultEmbedded:
        return "default-embedded";
    }
    return "?";
}

const RecordInfo *factory_default(const Store &s) {
    for (size_t i = 0; i < s.embedded_count(); ++i)
        if (s.embedded_at(i).screen_count) return &s.embedded_at(i);
    return nullptr;
}

size_t fallback_chain(const Store &s, const char *record_id, Candidate out[4]) {
    size_t n = 0;
    auto add = [&](Copy c, const char *id) {
        out[n].copy = c;
        copy_str(out[n].id, ID_LEN, id);
        ++n;
    };
    if (record_id && s.stored(record_id)) {
        add(Copy::Stored, record_id);
        add(Copy::Lkg, record_id);
    }
    if (record_id && s.embedded(record_id)) add(Copy::Embedded, record_id);
    const RecordInfo *def = factory_default(s);
    if (def && !eq(def->id, record_id)) add(Copy::DefaultEmbedded, def->id);
    return n;
}

// ---- record codec -------------------------------------------------------

bool parse_record(JsonVariantConst rec, RecordInfo &info, JsonVariantConst &doc_out, Error &err) {
    clear(info);
    if (!rec.is<JsonObjectConst>()) {
        set_err(err, 400, "bad_record", "record must be an object");
        return false;
    }
    const char *id = rec["id"] | "";
    if (!valid_id(id)) {
        set_err(err, 400, "bad_record", "bad record id");
        return false;
    }
    copy_str(info.id, sizeof(info.id), id);
    copy_str(info.title, sizeof(info.title), rec["title"] | id);
    if (!parse_source(rec["source"] | "local", info.source)) info.source = Source::Local;
    copy_str(info.hash, sizeof(info.hash), rec["hash"] | "");
    doc_out = rec["doc"];
    if (!doc_out.is<JsonObjectConst>()) {
        set_err(err, 400, "bad_record", "record has no 'doc' object");
        return false;
    }
    return describe_doc(doc_out, info, err);
}

// ---- pull ---------------------------------------------------------------

bool parse_pull(JsonVariantConst blk, PullPlan &out, Error &err) {
    memset(&out, 0, sizeof(out));
    if (!blk.is<JsonObjectConst>()) {
        set_err(err, 0, "absent", "no midl block");
        return false;
    }
    if (!blk["index"].isNull()) {
        if (!parse_index(blk["index"], out.index, err)) return false;
        out.has_index = true;
    }
    JsonArrayConst recs = blk["records"].as<JsonArrayConst>();
    if (!blk["records"].isNull() && recs.isNull()) {
        set_err(err, 400, "bad_midl_block", "midl.records must be an array");
        return false;
    }
    for (JsonVariantConst r : recs) {
        const char *id = r["id"] | "";
        const char *hash = r["hash"] | "";
        if (!valid_id(id) || !hash[0] || strlen(hash) >= HASH_LEN) {
            set_err(err, 400, "bad_midl_block", "midl.records[]: bad id or hash");
            return false;
        }
        if (out.count >= sizeof(out.entries) / sizeof(out.entries[0])) break;
        PullEntry &e = out.entries[out.count++];
        copy_str(e.id, sizeof(e.id), id);
        copy_str(e.title, sizeof(e.title), r["title"] | id);
        copy_str(e.hash, sizeof(e.hash), hash);
        copy_str(e.url, sizeof(e.url), r["url"] | "");
    }
    return true;
}

void plan_pull(const Store &s, const PullPlan &p, PullActions &out) {
    memset(&out, 0, sizeof(out));
    for (uint8_t i = 0; i < p.count; ++i) {
        const RecordInfo *have = s.effective(p.entries[i].id);
        if (have && eq(have->hash, p.entries[i].hash)) continue;
        out.fetch[out.fetch_count++] = i;
    }
    const RecordInfo *recs[MAX_RECORDS];
    size_t n = s.all(recs, MAX_RECORDS);
    for (size_t r = 0; r < n; ++r) {
        const RecordInfo *st = s.stored(recs[r]->id);
        if (!st || st->source != Source::Manager) continue;
        bool listed = false;
        for (uint8_t i = 0; i < p.count; ++i)
            if (eq(p.entries[i].id, st->id)) listed = true;
        if (!listed && out.remove_count < MAX_STORED)
            copy_str(out.remove[out.remove_count++], ID_LEN, st->id);
    }
}

}  // namespace midl::records
