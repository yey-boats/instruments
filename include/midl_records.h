#pragma once

// MIDL record store: the pure (host-testable) half.
//
// The display renders MIDL screens only (docs/midl/midl-only.md). Screens live
// in RECORDS: one MIDL document + metadata ({id,title,source,hash,doc}). The
// firmware embeds one record per built-in screen (source "embedded"); Display
// Manager pushes or the device pulls "manager" records; a web-authenticated
// write stores a "local" record. A manager/local record with the id of an
// embedded record overrides it; deleting the override restores the embedded
// one. Screen ids are unique across all (effective) records.
//
// This header holds everything that does not need LittleFS or LVGL: record
// metadata, the index, the override rule, screen-id conflict detection, the
// carousel order, screen -> owning-record lookup, the boot fallback chain, the
// record/index JSON codecs and the manager-config `midl` block (pull) parser.
// The device half (persistence, activation, navigation) is midl_store.h.
//
// MEMORY: Store is ~45 KB. Never put one on a task stack or in .bss on the
// device; midl_store.cpp allocates it in PSRAM (yeyboats::psram_new).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace midl::records {

constexpr size_t ID_LEN = 33;         // [a-z0-9_-]{1,32} + NUL
constexpr size_t TITLE_LEN = 48;      // record title (truncated, never rejected)
constexpr size_t HASH_LEN = 72;       // "sha256:" + 64 hex + NUL
constexpr size_t SCREEN_ID_LEN = 32;  // = midl::FirmwareLimits::str_len
constexpr size_t SCREEN_TITLE_LEN = 32;
constexpr size_t MAX_SCREENS_PER_RECORD = 16;  // = midl::validate::MAX_SCREENS
constexpr size_t MAX_EMBEDDED = 16;
constexpr size_t MAX_STORED = 24;  // manager + local records on flash
constexpr size_t MAX_RECORDS = MAX_EMBEDDED + MAX_STORED;
constexpr size_t MAX_DOC_BYTES = 48 * 1024;  // same cap as POST /api/midl/config
constexpr size_t URL_LEN = 160;

enum class Source : uint8_t { Embedded = 0, Manager = 1, Local = 2 };
const char *source_name(Source s);
bool parse_source(const char *s, Source &out);

// Record id grammar: [a-z0-9_-]{1,32}.
bool valid_id(const char *id);

struct ScreenRef {
    char id[SCREEN_ID_LEN];
    char title[SCREEN_TITLE_LEN];
};

struct RecordInfo {
    char id[ID_LEN];
    char title[TITLE_LEN];
    Source source;
    char hash[HASH_LEN];
    uint8_t screen_count;
    ScreenRef screens[MAX_SCREENS_PER_RECORD];
};

struct Index {
    char order[MAX_RECORDS][ID_LEN];
    uint8_t order_count;
    char default_screen[SCREEN_ID_LEN];
    char hidden[MAX_RECORDS][ID_LEN];
    uint8_t hidden_count;
};

struct Error {
    int http;  // suggested HTTP status (400/404/409/413/422/507)
    char code[32];
    char message[96];
};

void clear(Index &ix);
void clear(RecordInfo &r);

// Fill info.screens from doc["screens"] (array of {id,title}). Screens without
// an id are skipped (the renderer synthesizes ids for those, they cannot be
// addressed). Returns false (err 422 no_screens / screen_id_too_long /
// duplicate_screen_id) when the doc names no addressable screen, a screen id
// does not fit, or the doc repeats a screen id.
bool describe_doc(JsonVariantConst doc, RecordInfo &info, Error &err);

// Index codec. parse_index accepts {order:[...], defaultScreen:"..",
// hidden:[...]}; every key optional, unknown ids are kept (they may name a
// record that arrives later). Returns false (err 400) for a non-object or a
// bad record id in order/hidden.
bool parse_index(JsonVariantConst v, Index &out, Error &err);
void index_to_json(const Index &ix, JsonObject out);

class Store {
  public:
    void reset();  // forget everything (embedded + stored + index)

    // Embedded records register at boot, in carousel order (that order is the
    // "embedded order" the index falls back to).
    bool add_embedded(const RecordInfo &info);
    // Insert/replace a manager/local record (no conflict check: call
    // find_conflict first). Returns false when the stored table is full.
    bool put(const RecordInfo &info);
    // Remove a stored (manager/local) record. Returns false when none exists
    // (a bare embedded record cannot be deleted).
    bool remove(const char *id);

    const RecordInfo *effective(const char *id) const;  // stored override, else embedded
    const RecordInfo *embedded(const char *id) const;
    const RecordInfo *stored(const char *id) const;
    bool overrides_embedded(const char *id) const { return stored(id) && embedded(id); }

    // Effective records, embedded order first, then stored-only ones in id
    // order. out[] receives up to cap pointers; returns the count.
    size_t all(const RecordInfo **out, size_t cap) const;

    // Would `cand` (replacing any record with the same id) give a screen id
    // that another effective record already owns? On conflict, other_record /
    // screen_id get the colliding names and true is returned.
    bool find_conflict(const RecordInfo &cand, char *other_record, size_t other_cap,
                       char *screen_id, size_t screen_cap) const;

    // The effective record that owns screen `screen_id`, or nullptr.
    const RecordInfo *owner_of(const char *screen_id) const;

    // Carousel: visible effective records. index.order first (existing,
    // non-hidden, de-duplicated), then the rest in embedded-then-id order.
    size_t carousel(const RecordInfo **out, size_t cap) const;
    // Neighbours in the carousel (wraps). nullptr when the carousel is empty.
    // A record that is not in the carousel (hidden) steps to the first/last.
    const RecordInfo *next_record(const char *cur) const;
    const RecordInfo *prev_record(const char *cur) const;

    // Home screen: index.defaultScreen when an effective record owns it, else
    // the first screen of the first visible record, else of any record. "" when
    // the store is empty. Writes the owning record id when owner_out != null.
    void home_screen(char *out, size_t cap, char *owner_out = nullptr, size_t owner_cap = 0) const;

    Index &index() { return index_; }
    const Index &index() const { return index_; }
    bool is_hidden(const char *id) const;

    size_t embedded_count() const { return n_embedded_; }
    size_t stored_count() const { return n_stored_; }
    const RecordInfo &embedded_at(size_t i) const { return embedded_[i]; }

  private:
    int embedded_pos(const char *id) const;
    RecordInfo embedded_[MAX_EMBEDDED];
    RecordInfo stored_[MAX_STORED];
    uint8_t n_embedded_ = 0;
    uint8_t n_stored_ = 0;
    Index index_;
};

// ---- boot / activation fallback chain ---------------------------------
// record -> its LKG copy -> the embedded record of the same id -> the default
// embedded record (the owner of the factory home screen). Stored/Lkg appear
// only when a stored record exists; the device skips an Lkg it cannot find.
enum class Copy : uint8_t { Stored, Lkg, Embedded, DefaultEmbedded };
const char *copy_name(Copy c);
struct Candidate {
    Copy copy;
    char id[ID_LEN];
};
size_t fallback_chain(const Store &s, const char *record_id, Candidate out[4]);

// Factory default: the embedded record owning the first embedded screen.
const RecordInfo *factory_default(const Store &s);

// ---- record files: {id,title,source,hash,doc} -------------------------
// Parse a stored record object. Fills info (screens from the doc) and points
// doc_out at the "doc" member. Returns false (err) on a malformed record.
bool parse_record(JsonVariantConst rec, RecordInfo &info, JsonVariantConst &doc_out, Error &err);

// ---- pull: manager config `midl` block ---------------------------------
struct PullEntry {
    char id[ID_LEN];
    char title[TITLE_LEN];
    char hash[HASH_LEN];
    char url[URL_LEN];
};
struct PullPlan {
    bool has_index;
    Index index;
    uint8_t count;
    PullEntry entries[MAX_STORED + MAX_EMBEDDED];
};
// Parse cfg["midl"] = {index:{...}, records:[{id,title,hash,url}]}. Returns
// false when the block is absent / not an object (err.code "absent") or
// malformed. Entries with a bad id or missing hash are rejected (400).
bool parse_pull(JsonVariantConst midl_block, PullPlan &out, Error &err);

struct PullActions {
    uint8_t fetch[MAX_STORED + MAX_EMBEDDED];  // indexes into PullPlan.entries
    uint8_t fetch_count;
    char remove[MAX_STORED][ID_LEN];  // stored manager records not listed
    uint8_t remove_count;
};
// Diff a pull plan against the store: fetch entries whose hash differs from
// the effective record (an embedded record listed by reference with its own
// hash is a no-op); remove "manager" records the plan no longer lists. Local
// and embedded records are never removed.
void plan_pull(const Store &s, const PullPlan &p, PullActions &out);

}  // namespace midl::records
