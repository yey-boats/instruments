#include "knob_remote.h"

#include <string.h>

#include "ui_screens.h"

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "device_identity.h"
#include "manager.h"
#include "proto/proto.h"
#include "proto_ble.h"
#include "proto_discovery.h"
#include "proto_ip.h"
#include "storage.h"
#endif

namespace knob_remote {

namespace {
DisplayEntry s_displays[kMaxDisplays];
int s_count = 0;
// Ingest scratch: the count being rebuilt between begin_ingest/commit_ingest.
// Defined unconditionally so the ingest functions compile on non-knob boards
// (where they are effectively no-ops).
int s_ingest_count = 0;

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// Guards s_displays / s_count. The manager worker rewrites entries 1..N while
// the LVGL task reads them via copy_entry(); without the lock the reader could
// observe a half-written DisplayEntry. Entry 0 (the local knob) is immutable
// after setup() so reads of it are safe regardless.
SemaphoreHandle_t s_mtx = nullptr;
// UI-task -> worker request: which display index needs its views fetched
// (-1 = none). Written by the UI task in request_views_fetch(), drained by the
// worker. A plain volatile int is sufficient (single producer/consumer, word
// aligned); the heavier registry mutation is serialized by s_mtx.
volatile int s_pending_views_idx = -1;

// UI-task -> worker request: a remote view-switch to perform via a blocking
// manager screen.set POST. switch_view() resolves the target device id + view
// id under s_mtx and stows them here; the worker drains it on its next tick so
// the HTTP never runs on the LVGL task. Guarded by s_mtx (multi-field struct,
// unlike the single-word views index).
struct PendingSwitch {
    bool pending;
    char dev_id[40];
    char view_id[24];
    Transport transport;
    char base_url[40];
};
PendingSwitch s_pending_switch = {false, {0}, {0}, Transport::Manager, {0}};

void lock() {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
void unlock() {
    if (s_mtx) xSemaphoreGive(s_mtx);
}
#else
void lock() {
}
void unlock() {
}
#endif

void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

void add_local() {
    DisplayEntry &e = s_displays[0];
    memset(&e, 0, sizeof(e));
    copy_str(e.id, sizeof(e.id), "");
    copy_str(e.name, sizeof(e.name), "This knob");
    e.is_local = true;
    const char *vids[4] = {"ap_hud", "knob_compass", "knob_wind", "knob_big"};
    const char *vtitles[4] = {"Autopilot", "Compass", "Wind", "Big number"};
    for (int i = 0; i < 4; ++i) {
        copy_str(e.view_id[i], sizeof(e.view_id[i]), vids[i]);
        copy_str(e.view_title[i], sizeof(e.view_title[i]), vtitles[i]);
    }
    e.view_count = 4;
    e.current_view = 0;
    e.transport = Transport::Local;
    s_count = 1;
}

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
// Find a remote entry [1..s_count) by device id; -1 if absent. Caller holds
// the lock.
int find_by_id_locked(const char *id) {
    if (!id || !*id) return -1;
    for (int i = 1; i < s_count; ++i) {
        if (strncmp(s_displays[i].id, id, sizeof(s_displays[i].id)) == 0) return i;
    }
    return -1;
}
#endif
}  // namespace

void setup() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
#endif
    lock();
    add_local();
    unlock();
}

int display_count() {
    lock();
    int n = s_count;
    unlock();
    return n;
}

bool copy_entry(int idx, DisplayEntry &out) {
    lock();
    bool ok = (idx >= 0 && idx < s_count);
    if (ok) out = s_displays[idx];
    unlock();
    return ok;
}

bool switch_view(int dev_idx, int view_idx) {
    // Snapshot the entry under the lock, then act on the copy.
    DisplayEntry e;
    if (!copy_entry(dev_idx, e)) return false;
    if (view_idx < 0 || view_idx >= e.view_count) return false;
    if (e.is_local) {
        lock();
        if (dev_idx >= 0 && dev_idx < s_count) s_displays[dev_idx].current_view = view_idx;
        unlock();
        return ui::show_by_id(e.view_id[view_idx]);
    }
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // Remote: queue the manager screen.set for the worker. The blocking POST
    // (/devices/:id/command) must NOT run here on the LVGL task, so we only
    // resolve the target id+view under the lock, stow them in the pending slot,
    // and return true (queued). drain_pending_switch() does the HTTP on the
    // manager worker; the target executes screen.set and applies via configPush.
    lock();
    s_pending_switch.pending = true;
    copy_str(s_pending_switch.dev_id, sizeof(s_pending_switch.dev_id), e.id);
    copy_str(s_pending_switch.view_id, sizeof(s_pending_switch.view_id), e.view_id[view_idx]);
    // Carry the transport so the worker drains it via the right path: an IP peer
    // (mDNS-discovered) is driven with proto_ip attach/switch/detach; a BLE-only
    // peer with proto_ble on-demand central; a manager-only peer falls back to
    // the manager screen.set POST. The pending slot's `base_url` carries the
    // reach address for whichever transport: the HTTP base for IP, the BLE
    // address for BLE.
    s_pending_switch.transport = e.transport;
    copy_str(s_pending_switch.base_url, sizeof(s_pending_switch.base_url),
             e.transport == Transport::BLE ? e.ble_addr : e.base_url);
    // Optimistically reflect the requested view in the registry for the menu;
    // the actual device state is reconciled by the next summary refresh.
    if (dev_idx >= 0 && dev_idx < s_count) s_displays[dev_idx].current_view = view_idx;
    unlock();
    return true;
#else
    return false;
#endif
}

// --- Ingest API (called by the manager worker via manager.cpp) -------------

void begin_ingest() {
    lock();
    s_ingest_count = 1;  // entry 0 is the local knob, preserved
}

void ingest_display(const char *id, const char *name, const char *current_screen) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    if (s_ingest_count >= (int)kMaxDisplays) return;
    if (!id || !*id) return;
    // Preserve a previously-fetched view list for this id across refreshes so a
    // slow summary refresh doesn't wipe views fetched on drill-in.
    DisplayEntry prev;
    bool had_prev = false;
    int old = find_by_id_locked(id);
    if (old >= 0) {
        prev = s_displays[old];
        had_prev = true;
    }
    DisplayEntry &e = s_displays[s_ingest_count];
    memset(&e, 0, sizeof(e));
    copy_str(e.id, sizeof(e.id), id);
    copy_str(e.name, sizeof(e.name), name && *name ? name : id);
    e.is_local = false;
    e.current_view = -1;
    e.transport = Transport::Manager;
    // Preserve an IP transport already discovered for this id across a manager
    // summary refresh — IP is preferred and must not be downgraded to Manager
    // just because the slow summary GET re-ran.
    if (had_prev && prev.transport == Transport::IP) {
        e.transport = Transport::IP;
        copy_str(e.base_url, sizeof(e.base_url), prev.base_url);
    }
    if (had_prev) {
        e.view_count = prev.view_count;
        for (int v = 0; v < prev.view_count && v < (int)kMaxViews; ++v) {
            memcpy(e.view_id[v], prev.view_id[v], sizeof(e.view_id[v]));
            memcpy(e.view_title[v], prev.view_title[v], sizeof(e.view_title[v]));
        }
    }
    // Resolve current view from the reported screen id if known.
    if (current_screen && *current_screen) {
        for (int v = 0; v < e.view_count; ++v) {
            if (strncmp(e.view_id[v], current_screen, sizeof(e.view_id[v])) == 0) {
                e.current_view = v;
                break;
            }
        }
    }
    s_ingest_count++;
#else
    (void)id;
    (void)name;
    (void)current_screen;
#endif
}

void commit_ingest() {
    s_count = s_ingest_count;
    unlock();
}

void copy_device_id(int dev_idx, char *out, size_t out_cap) {
    if (!out || !out_cap) return;
    out[0] = '\0';
    lock();
    if (dev_idx >= 0 && dev_idx < s_count) copy_str(out, out_cap, s_displays[dev_idx].id);
    unlock();
}

void set_views(int dev_idx, const char *const *view_ids, const char *const *view_titles, int count,
               const char *current) {
    lock();
    if (dev_idx > 0 && dev_idx < s_count) {
        DisplayEntry &e = s_displays[dev_idx];
        if (count < 0) count = 0;
        if (count > (int)kMaxViews) count = (int)kMaxViews;
        e.view_count = count;
        e.current_view = -1;
        for (int v = 0; v < count; ++v) {
            copy_str(e.view_id[v], sizeof(e.view_id[v]), view_ids ? view_ids[v] : "");
            copy_str(e.view_title[v], sizeof(e.view_title[v]),
                     view_titles && view_titles[v] ? view_titles[v] : e.view_id[v]);
            if (current && *current && strncmp(e.view_id[v], current, sizeof(e.view_id[v])) == 0) {
                e.current_view = v;
            }
        }
    }
    unlock();
}

// --- Worker-driven fetch entry points --------------------------------------

bool manager_available() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    return manager::is_provisioned();
#else
    return false;
#endif
}

int refresh_from_manager() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    if (!manager_available()) return -1;
    return manager::knob_refresh_displays();
#else
    return -1;
#endif
}

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
namespace {
// mDNS browse callback: merge each discovered display into the registry. Counts
// how many entries were added/upgraded via the ctx int.
void on_ip_peer(const proto_discovery::Peer &p, void *ctx) {
    if (ingest_ip_peer(p.device_id, p.device_id, p.base_url, p.board, p.display)) {
        if (ctx) (*static_cast<int *>(ctx))++;
    }
}
}  // namespace
#endif

int refresh_ip_peers() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    int changed = 0;
    int found = proto_discovery::browse(on_ip_peer, &changed);
    if (found < 0) return -1;
    return changed;
#else
    return -1;
#endif
}

bool fetch_views_for(int dev_idx) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // Resolve the transport + base URL under the lock, then do the blocking
    // fetch with the mutex released.
    Transport transport = Transport::Manager;
    char base_url[40] = {0};
    char ble_addr[24] = {0};
    lock();
    if (dev_idx > 0 && dev_idx < s_count) {
        transport = s_displays[dev_idx].transport;
        copy_str(base_url, sizeof(base_url), s_displays[dev_idx].base_url);
        copy_str(ble_addr, sizeof(ble_addr), s_displays[dev_idx].ble_addr);
    }
    unlock();

    // Either transport that speaks the control protocol fills views from a
    // DeviceRecord: IP reads GET /api/p2p/device; BLE reads the DEVICE
    // characteristic via an on-demand connect (proto_ble::get_device_on_peer).
    if (transport == Transport::IP || transport == Transport::BLE) {
        // DeviceRecord is ~1.5 KB (transports[16][24] + 7x char[48]); keep it off
        // the worker-task stack per the CLAUDE.md large-struct trap. knob_remote
        // fetch/switch run single-flight on the worker task, so a function-static
        // scratch is race-free.
        static proto::DeviceRecord rec;
        memset(&rec, 0, sizeof(rec));
        bool got = false;
        if (transport == Transport::IP) {
            if (!base_url[0]) return false;
            got = proto_ip::get_device(String(base_url), rec);
        } else {
            if (!ble_addr[0]) return false;
            got = proto_ble::get_device_on_peer(ble_addr, rec);
        }
        if (!got) return false;
        const char *ids[kMaxViews] = {nullptr};
        const char *titles[kMaxViews] = {nullptr};
        int count = rec.views_count;
        if (count > (int)kMaxViews) count = (int)kMaxViews;
        for (int i = 0; i < count; ++i) {
            ids[i] = rec.views[i].id;
            titles[i] = rec.views[i].title;
        }
        set_views(dev_idx, ids, titles, count, rec.currentView);
        return true;
    }

    if (!manager_available()) return false;
    return manager::knob_fetch_views(dev_idx);
#else
    (void)dev_idx;
    return false;
#endif
}

void request_views_fetch(int dev_idx) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    s_pending_views_idx = dev_idx;
#else
    (void)dev_idx;
#endif
}

bool drain_pending_views_fetch() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    int idx = s_pending_views_idx;
    if (idx < 0) return false;
    s_pending_views_idx = -1;
    return fetch_views_for(idx);
#else
    return false;
#endif
}

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
namespace {

// Default controller color (design §6 makes this configurable; a fixed cyan is
// fine for 3.3). The shared key, if any, is read from NVS namespace "proto",
// key "key" (open/no-auth when unset).
constexpr const char *kDefaultControllerColor = "#00bcd4";

void read_controller_color(char *out, size_t cap) {
    // Phase 6 will persist an operator-chosen color in NVS proto/color; until
    // then use the default. Honor it early if already set so 3.3 + 6 compose.
    storage::Namespace ns("proto", true);
    std::string c = ns.get_string("color", kDefaultControllerColor);
    copy_str(out, cap, c.empty() ? kDefaultControllerColor : c.c_str());
}

void read_shared_key(char *out, size_t cap) {
    storage::Namespace ns("proto", true);
    std::string k = ns.get_string("key", "");
    copy_str(out, cap, k.c_str());
}

// Build the controller's Attach (id/name/color/key/ttl) from device identity +
// the NVS-persisted proto color/key. Shared by the IP and BLE switch paths.
void build_attach(proto::Attach &a) {
    strncpy(a.v, "1.0", sizeof(a.v) - 1);
    strncpy(a.controllerId, device_identity::get().device_id, sizeof(a.controllerId) - 1);
    strncpy(a.name, device_identity::get().device_id, sizeof(a.name) - 1);
    char color[16];
    read_controller_color(color, sizeof(color));
    strncpy(a.color, color, sizeof(a.color) - 1);
    char key[40];
    read_shared_key(key, sizeof(key));
    strncpy(a.key, key, sizeof(a.key) - 1);
    a.ttlMs = proto::kDefaultTtlMs;
}

// Drive a BLE-only peer for one view switch via the on-demand central. Blocking
// (scan-free connect -> attach -> switch -> detach -> disconnect+deleteClient,
// all inside proto_ble); worker task only. Returns true if the switch ack
// reported ok.
bool ble_switch(const char *ble_addr, const char *view_id) {
    if (!ble_addr || !*ble_addr) return false;
    proto::Attach a{};
    build_attach(a);
    proto::Switch sw{};
    strncpy(sw.v, "1.0", sizeof(sw.v) - 1);
    strncpy(sw.viewId, view_id ? view_id : "", sizeof(sw.viewId) - 1);
    return proto_ble::switch_on_peer(ble_addr, a, sw);
}

// Drive an IP peer for one view switch over the control protocol. Blocking
// (attach -> switch -> detach); runs on the worker task only. Returns true if
// the switch ack reported the target moved to the requested view.
bool ip_switch(const char *base_url, const char *view_id) {
    if (!base_url || !*base_url) return false;
    String base(base_url);

    proto::Attach a{};
    build_attach(a);

    // AttachAck embeds a DeviceRecord (~1.5 KB) — keep it off the worker-task
    // stack per the CLAUDE.md large-struct trap. Switch drains single-flight on
    // the worker task, so a function-static scratch is race-free.
    static proto::AttachAck ack;
    memset(&ack, 0, sizeof(ack));
    if (!proto_ip::attach(base, a, ack) || !ack.accepted) return false;

    proto::Switch sw{};
    strncpy(sw.v, "1.0", sizeof(sw.v) - 1);
    strncpy(sw.sessionId, ack.sessionId, sizeof(sw.sessionId) - 1);
    strncpy(sw.viewId, view_id ? view_id : "", sizeof(sw.viewId) - 1);
    proto::SwitchAck sa{};
    bool ok = proto_ip::do_switch(base, sw, sa) && sa.ok;

    // Release the session — the knob does not hold a live attachment between
    // turns (per the on-demand control model). The colored frame on the target
    // clears on detach / TTL.
    proto_ip::detach(base, ack.sessionId);
    return ok;
}

}  // namespace
#endif

bool drain_pending_switch() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    // Copy the request out under the lock and clear it, then release the lock
    // before the blocking I/O so the mutex is never held across HTTP.
    char dev_id[40];
    char view_id[24];
    char base_url[40];
    Transport transport = Transport::Manager;
    lock();
    bool pending = s_pending_switch.pending;
    if (pending) {
        s_pending_switch.pending = false;
        copy_str(dev_id, sizeof(dev_id), s_pending_switch.dev_id);
        copy_str(view_id, sizeof(view_id), s_pending_switch.view_id);
        copy_str(base_url, sizeof(base_url), s_pending_switch.base_url);
        transport = s_pending_switch.transport;
    }
    unlock();
    if (!pending) return false;
    if (transport == Transport::IP) return ip_switch(base_url, view_id);
    // base_url carries the BLE address for a BLE-transport pending switch.
    if (transport == Transport::BLE) return ble_switch(base_url, view_id);
    return manager::knob_post_screen_set(dev_id, view_id);
#else
    return false;
#endif
}

bool ingest_ip_peer(const char *id, const char *name, const char *base_url, const char *board,
                    const char *display) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    (void)board;
    (void)display;
    if (!id || !*id || !base_url || !*base_url) return false;
    // Skip our own advertisement — entry 0 is already the local knob.
    const char *own = device_identity::get().device_id;
    if (own && *own && strcmp(own, id) == 0) return false;

    bool changed = false;
    lock();
    int idx = find_by_id_locked(id);
    if (idx >= 0) {
        // Existing entry (likely from the manager source): upgrade it to IP,
        // which is preferred because it needs no manager. View list, name and
        // current_view are preserved.
        DisplayEntry &e = s_displays[idx];
        if (e.transport != Transport::IP ||
            strncmp(e.base_url, base_url, sizeof(e.base_url)) != 0) {
            e.transport = Transport::IP;
            copy_str(e.base_url, sizeof(e.base_url), base_url);
            if ((!e.name[0]) && name && *name) copy_str(e.name, sizeof(e.name), name);
            changed = true;
        }
    } else if (s_count < (int)kMaxDisplays) {
        DisplayEntry &e = s_displays[s_count];
        memset(&e, 0, sizeof(e));
        copy_str(e.id, sizeof(e.id), id);
        copy_str(e.name, sizeof(e.name), name && *name ? name : id);
        e.is_local = false;
        e.current_view = -1;
        e.view_count = 0;  // filled lazily on drill-in via fetch_views_for()
        e.transport = Transport::IP;
        copy_str(e.base_url, sizeof(e.base_url), base_url);
        s_count++;
        changed = true;
    }
    unlock();
    return changed;
#else
    (void)id;
    (void)name;
    (void)base_url;
    (void)board;
    (void)display;
    return false;
#endif
}

bool ingest_ble_peer(const char *id, const char *name, const char *ble_addr) {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    if (!id || !*id || !ble_addr || !*ble_addr) return false;
    // Skip our own advertisement — entry 0 is already the local knob.
    const char *own = device_identity::get().device_id;
    if (own && *own && strcmp(own, id) == 0) return false;

    bool changed = false;
    lock();
    int idx = find_by_id_locked(id);
    if (idx >= 0) {
        // The device already exists via IP or the manager. BLE is the fallback
        // transport (IP-preferred): do NOT downgrade the entry's transport.
        // Just record the BLE address so the fallback is available without a
        // re-scan if the IP path later goes away. The transport stays as-is.
        DisplayEntry &e = s_displays[idx];
        if (strncmp(e.ble_addr, ble_addr, sizeof(e.ble_addr)) != 0) {
            copy_str(e.ble_addr, sizeof(e.ble_addr), ble_addr);
            changed = true;
        }
    } else if (s_count < (int)kMaxDisplays) {
        // BLE-only peer (no IP/manager entry): create a Transport::BLE entry.
        DisplayEntry &e = s_displays[s_count];
        memset(&e, 0, sizeof(e));
        copy_str(e.id, sizeof(e.id), id);
        copy_str(e.name, sizeof(e.name), name && *name ? name : id);
        e.is_local = false;
        e.current_view = -1;
        e.view_count = 0;  // filled lazily on drill-in via fetch_views_for()
        e.transport = Transport::BLE;
        copy_str(e.ble_addr, sizeof(e.ble_addr), ble_addr);
        s_count++;
        changed = true;
    }
    unlock();
    return changed;
#else
    (void)id;
    (void)name;
    (void)ble_addr;
    return false;
#endif
}

#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
namespace {
// BLE scan callback: merge each Control-service peer into the registry as a BLE
// peer (IP-preferred dedup in ingest_ble_peer). Counts changes via the ctx int.
void on_ble_peer(const proto_ble::Peer &p, void *ctx) {
    if (ingest_ble_peer(p.device_id, p.device_id, p.addr)) {
        if (ctx) (*static_cast<int *>(ctx))++;
    }
}
}  // namespace
#endif

int refresh_ble_peers() {
#if defined(BOARD_ID_WAVESHARE_KNOB_1_8)
    int changed = 0;
    // ~3 s active scan window. BLE is the fallback transport, scanned on a slow
    // cadence by the worker (see manager.cpp) to bound NimBLE radio/RAM pressure.
    int found = proto_ble::scan(3000, on_ble_peer, &changed);
    if (found < 0) return -1;
    return changed;
#else
    return -1;
#endif
}

}  // namespace knob_remote
