// MIDL record store, UI half: activation (with the fallback chain) and
// record-aware navigation. LVGL: runs on the UI task ONLY (setup() before the
// LVGL task starts, then the app pump). See include/midl_store.h.
//
// One record is active: its screens are the registered MIDL screens (next to
// the system screens, which ui::reset_screens() keeps). Navigating to a screen
// of another record loads that record first; swiping past the last (first)
// screen of the active record loads the next (previous) visible record.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "midl_store.h"

#include <string.h>

#include "apply_blank.h"
#include "midl_render.h"
#include "net.h"
#include "psram_json.h"
#include "ui_screens.h"

namespace midl::store_ui {

using midl::records::Candidate;
using midl::records::Copy;
using midl::records::ID_LEN;
using midl::records::SCREEN_ID_LEN;

namespace {

void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

bool try_apply(const Candidate &c) {
    JsonDocument holder(&yeyboats::psram_json);  // tree in PSRAM, never the task stack
    JsonVariantConst doc = midl::store::load_doc(c, holder);
    if (doc.isNull()) return false;
    return midl::render::apply_all(doc) > 0;
}

// Registry indexes of the active record's swipeable screens.
size_t midl_screens(int *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < ui::screen_count() && n < cap; ++i)
        if (!ui::is_system((int)i) && !ui::is_hidden((int)i)) out[n++] = (int)i;
    return n;
}

void step(bool forward) {
    int idx[ui::MAX_SCREENS];
    size_t n = midl_screens(idx, ui::MAX_SCREENS);
    int cur = ui::current_index();
    int pos = -1;
    for (size_t i = 0; i < n; ++i)
        if (idx[i] == cur) pos = (int)i;
    if (pos < 0 || n == 0) {
        // On a system/zoom screen (or nothing registered): plain cycle.
        forward ? ui::next() : ui::prev();
        return;
    }
    bool at_edge = forward ? pos == (int)n - 1 : pos == 0;
    if (!at_edge) {
        ui::show(idx[forward ? pos + 1 : pos - 1]);
        return;
    }
    char act[ID_LEN], rec[ID_LEN], sid[SCREEN_ID_LEN];
    midl::store::active(act, sizeof(act));
    if (midl::store::neighbour(act, forward, rec, sizeof(rec), sid, sizeof(sid)) &&
        strcmp(rec, act) != 0) {
        activate(rec, sid);
        return;
    }
    // Single visible record: wrap inside it.
    ui::show(idx[forward ? 0 : n - 1]);
}

}  // namespace

bool activate(const char *record_id, const char *screen_id) {
    bool keep = screen_id == nullptr;
    char keep_id[SCREEN_ID_LEN] = "";
    if (keep) copy_str(keep_id, sizeof(keep_id), ui::current_id());
    char rec[ID_LEN], target[SCREEN_ID_LEN];
    copy_str(rec, sizeof(rec), record_id);
    copy_str(target, sizeof(target), screen_id);
    if (!rec[0]) {
        // Home: the record owning index.defaultScreen.
        midl::store::home(target, sizeof(target), rec, sizeof(rec));
        keep = false;
    }
    Candidate chain[4];
    size_t n = midl::store::fallback_chain(rec, chain);
    int used = -1;
    for (size_t i = 0; i < n; ++i) {
        if (try_apply(chain[i])) {
            used = (int)i;
            break;
        }
        net::logf("[midl-store] '%s' %s copy did not render; falling back", chain[i].id,
                  midl::records::copy_name(chain[i].copy));
    }
    if (used < 0) {
        net::logf("[midl-store] activate '%s': nothing rendered", rec);
        apply_blank::rendered(false);  // nothing new to flush: release the dark window
        return false;
    }
    const Candidate &c = chain[used];
    midl::store::set_active(c.id);
    if (c.copy == Copy::Stored) midl::store::promote_lkg(c.id);
    if (c.copy == Copy::DefaultEmbedded) target[0] = 0;  // the asked-for screen is gone
    bool shown = false;
    if (target[0]) shown = ui::show_by_id(target);
    if (!shown && keep && keep_id[0]) shown = ui::show_by_id(keep_id);
    net::logf("[midl-store] active record '%s' (%s) on '%s'", c.id,
              midl::records::copy_name(c.copy), ui::current_id());
    // A blanked apply (#106) stays dark until this rebuild's first full flush.
    apply_blank::rendered(true);
    return true;
}

bool show(const char *screen_id) {
    if (!screen_id || !screen_id[0]) return false;
    if (ui::show_by_id(screen_id)) return true;  // active record or system screen
    char owner[ID_LEN], act[ID_LEN];
    if (!midl::store::owner_of(screen_id, owner, sizeof(owner))) {
        net::logf("[midl-store] no record owns screen '%s'", screen_id);
        return false;
    }
    midl::store::active(act, sizeof(act));
    if (strcmp(owner, act) == 0) return false;  // owned but did not build
    return activate(owner, screen_id);
}

void next() {
    step(true);
}

void prev() {
    step(false);
}

void home() {
    char sid[SCREEN_ID_LEN], owner[ID_LEN];
    midl::store::home(sid, sizeof(sid), owner, sizeof(owner));
    if (!sid[0] || !show(sid)) activate("", "");
}

void rebuild() {
    char cur[SCREEN_ID_LEN], act[ID_LEN];
    copy_str(cur, sizeof(cur), ui::current_id());
    midl::store::active(act, sizeof(act));
    ui::reset_screens(true);  // system screens rebuild lazily with the new palette
    activate(act, cur[0] ? cur : nullptr);
}

void boot() {
    if (!activate("", "")) net::logf("[midl-store] boot: no record rendered");
}

}  // namespace midl::store_ui
