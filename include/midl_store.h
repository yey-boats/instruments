#pragma once

// MIDL record store: the device half (docs/midl/midl-only.md).
//
// Owns the record store (embedded records + manager/local records on
// LittleFS), the index, the active record, and the HTTP/manager-facing
// operations. Two TUs:
//   - midl_store.cpp     persistence + store operations + JSON reports. No
//                        LVGL: safe from the web task, the manager worker and
//                        the UI task (one recursive mutex guards the store and
//                        the /midl files). Linked by the headless harness too.
//   - midl_store_ui.cpp  activation + navigation. LVGL: UI task ONLY (the app
//                        pump). Other tasks call request_activate(), which
//                        queues app::CommandType::MidlActivate.
//
// Flash layout: /midl/index.json, /midl/records/<id>.json (record JSON
// {id,title,source,hash,doc}), /midl/records/<id>.lkg.json (last known good,
// promoted after the record renders). Factory = the embedded records (reset
// deletes /midl/).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <Arduino.h>
#include <ArduinoJson.h>

#include "midl_records.h"

namespace midl::store {

using records::Candidate;
using records::Index;
using records::PullActions;
using records::PullPlan;
using records::Source;

struct Result {
    int http;           // HTTP status to answer with
    char code[32];      // machine code: screen_id_conflict, invalid_doc, ...
    char message[160];  // human-readable
    char path[128];     // JSON pointer of the first validation problem (422)
    char hash[records::HASH_LEN];
    char other[records::ID_LEN];          // 409: the record that owns the screen
    char screen[records::SCREEN_ID_LEN];  // 409: the colliding screen id
};

// Boot (setup(), before the UI renders): mount LittleFS, register the
// embedded records for this build's display class, load the index and the
// stored records' metadata.
void setup();

// ---- writes (web task / manager worker) ---------------------------------
// Validate `doc` against the embedded manifest (midl::validate::admit), check
// the 48 KB cap and screen-id conflicts, store it as record `id` (source
// manager|local) and, when `id` is the active record, queue a re-render.
// `hash`: for a MANAGER write, the hash Display Manager computed
// ("sha256:" + hex of JSON.stringify(doc)) is stored and reported verbatim,
// because ArduinoJson may format floats differently from JS. Null/empty (and
// every local write) = the device hashes its own compact serialization.
bool put_record(const char *id, const char *title, JsonVariantConst doc, Source src, Result &r,
                const char *hash = nullptr);
// Remove a manager/local record or override (404 for a bare embedded record
// or an unknown id). Re-renders when it was active.
bool delete_record(const char *id, Result &r);
bool put_index(JsonVariantConst index, Result &r);
// Delete /midl/ entirely; the embedded records + an empty index remain.
void factory_reset();

// ---- reads ----------------------------------------------------------------
// Full record JSON ({id,title,source,hash,doc}); false for an unknown id.
bool record_json(const char *id, String &out);
// GET /api/midl/records body: {active, screen, index, records:[...]}.
void list_json(JsonObject out);
// Heartbeat: ui.record + ui.screens[{id,title,record}] (visible records,
// carousel order) and the `midl` block {version,class,indexHash,records}.
void heartbeat_ui(JsonObject ui);
void heartbeat_midl(JsonObject midl);
// Identity capabilities.midl = {version,class,manifestHash,push,pull}.
void identity_caps(JsonObject midl);
// The embedded capability manifest (parsed once, PSRAM).
JsonVariantConst manifest();

// ---- pull (manager worker) ------------------------------------------------
// Parse cfg["midl"] and diff it against the store. False when there is no
// usable block (err.code "absent" when simply missing).
bool plan_pull(JsonVariantConst midl_block, PullPlan &plan, PullActions &acts, records::Error &err);
// Store the pulled index (if the plan carried one).
bool apply_pull_index(const PullPlan &plan);

// ---- activation support ---------------------------------------------------
// Queue (MidlActivate on the UI task) a render of `record_id` ("" / null = the
// owner of the home screen) and then show `screen_id` (null = keep the current
// screen when still present, else the record's default).
bool request_activate(const char *record_id, const char *screen_id);

// Load a fallback-chain candidate's doc into `holder` (PSRAM). Returns the
// doc variant (null on a missing/corrupt copy).
JsonVariantConst load_doc(const Candidate &c, JsonDocument &holder);
size_t fallback_chain(const char *record_id, Candidate out[4]);
// After a stored record rendered cleanly: copy it to its LKG file (only when
// the LKG differs).
void promote_lkg(const char *record_id);

// Store queries for navigation (copied out under the lock).
bool owner_of(const char *screen_id, char *out, size_t cap);
void home(char *screen_out, size_t screen_cap, char *owner_out, size_t owner_cap);
// Neighbour record in the carousel and its first / last screen.
bool neighbour(const char *record_id, bool forward, char *rec_out, size_t rec_cap, char *screen_out,
               size_t screen_cap);
// Number of records in the carousel (visible).
size_t visible_count();

void set_active(const char *record_id);
void active(char *out, size_t cap);

}  // namespace midl::store

// UI-task half (midl_store_ui.cpp). All of these touch LVGL.
namespace midl::store_ui {

// Boot: render the record owning the home screen (with the fallback chain).
void boot();
// Render `record_id` (fallback chain) and show `screen_id` (see
// request_activate). Returns true when some candidate rendered.
bool activate(const char *record_id, const char *screen_id);
// Show a screen id: registered (active record / system) -> show; else load the
// owning record first. False when no record owns it.
bool show(const char *screen_id);
// Swipe navigation across record boundaries.
void next();
void prev();
// Index defaultScreen (swipe down, Select, HUD HOME).
void home();
// Live theme switch: rebuild the active record (and the system screens).
void rebuild();

}  // namespace midl::store_ui
