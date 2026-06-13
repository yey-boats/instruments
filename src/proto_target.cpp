#include "proto_target.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "app_events.h"
#include "board.h"
#include "net.h"
#include "storage.h"
#include "ui_screens.h"

namespace proto_target {

namespace {

constexpr const char *NS = "proto";

// Single device-wide session table, guarded by s_mtx. The table and
// currentView are mutated only under the lock; LVGL/app::post calls happen
// AFTER the lock is released (mutex never held across post/ui::*).
proto::SessionTable s_table;
SemaphoreHandle_t s_mtx = nullptr;

// Shared key (NVS "proto"/"key"). Empty => open (control unauthenticated).
char s_key[48] = {0};

// Last view a controller switched us to. Seeds from ui::current_id() lazily;
// kept in sync on every accepted switch. Guarded by s_mtx.
char s_current_view[48] = {0};

void lock() {
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
}
void unlock() {
    if (s_mtx) xSemaphoreGive(s_mtx);
}

// Best-effort "currentView" for outbound records: prefer what the UI is
// actually showing; fall back to the last commanded view.
const char *live_current_view() {
    const char *id = ui::current_id();
    if (id && id[0]) return id;
    return s_current_view;
}

}  // namespace

void setup() {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    s_table.clear();
    {
        storage::Namespace p(NS, true);
        std::string k = p.get_string("key", "");
        strncpy(s_key, k.c_str(), sizeof(s_key) - 1);
        s_key[sizeof(s_key) - 1] = 0;
    }
    const char *cur = ui::current_id();
    if (cur && cur[0]) {
        strncpy(s_current_view, cur, sizeof(s_current_view) - 1);
    }
}

void set_key(const char *key) {
    lock();
    if (key) {
        strncpy(s_key, key, sizeof(s_key) - 1);
        s_key[sizeof(s_key) - 1] = 0;
    } else {
        s_key[0] = 0;
    }
    unlock();
}

void fill_device_record(proto::DeviceRecord &r) {
    // Fill in place (r is the caller's buffer); no large stack temporaries.
    strncpy(r.v, "1.0", sizeof(r.v) - 1);
    strncpy(r.deviceId, net::deviceId().c_str(), sizeof(r.deviceId) - 1);
    strncpy(r.name, net::deviceId().c_str(), sizeof(r.name) - 1);
    strncpy(r.role, "both", sizeof(r.role) - 1);
    strncpy(r.board, board::id(), sizeof(r.board) - 1);

    board::Geometry g = board::geometry();
    snprintf(r.display, sizeof(r.display), "%ux%u", (unsigned)g.width_px, (unsigned)g.height_px);

    strncpy(r.currentView, live_current_view(), sizeof(r.currentView) - 1);

    r.views_count = 0;
    size_t n = ui::screen_count();
    for (size_t i = 0; i < n && r.views_count < 16; ++i) {
        const char *id = "?";
        const char *title = "?";
        bool hidden = false;
        ui::screen_info((int)i, &id, &title, &hidden);
        if (hidden) continue;
        strncpy(r.views[r.views_count].id, id ? id : "", sizeof(r.views[r.views_count].id) - 1);
        strncpy(r.views[r.views_count].title, title ? title : "",
                sizeof(r.views[r.views_count].title) - 1);
        r.views_count++;
    }

    r.transports_count = 0;
    strncpy(r.transports[r.transports_count++], "ip", 23);
    strncpy(r.transports[r.transports_count++], "ble", 23);

    r.authRequired = (s_key[0] != 0);
}

bool handle_attach(const proto::Attach &a, proto::AttachAck &ack) {
    strncpy(ack.v, "1.0", sizeof(ack.v) - 1);

    if (!proto::version_compatible(a.v)) {
        ack.accepted = false;
        strncpy(ack.reason, "incompatible_version", sizeof(ack.reason) - 1);
        return false;
    }
    // Snapshot the shared key under the lock so a concurrent `ctl key` cannot
    // race the auth check (set_key writes s_key under s_mtx).
    char key_snapshot[sizeof(s_key)];
    lock();
    strncpy(key_snapshot, s_key, sizeof(key_snapshot) - 1);
    key_snapshot[sizeof(key_snapshot) - 1] = '\0';
    unlock();
    if (!proto::auth_ok(key_snapshot, a.key)) {
        ack.accepted = false;
        strncpy(ack.reason, "unauthorized", sizeof(ack.reason) - 1);
        return false;
    }

    char sid[16] = {0};
    int idx;
    lock();
    idx = s_table.attach(a, (long)millis(), sid, sizeof(sid));
    unlock();

    if (idx < 0) {
        ack.accepted = false;
        strncpy(ack.reason, "session_table_full", sizeof(ack.reason) - 1);
        return false;
    }

    ack.accepted = true;
    strncpy(ack.sessionId, sid, sizeof(ack.sessionId) - 1);
    ack.ttlMs = proto::kDefaultTtlMs;
    fill_device_record(ack.device);  // no lock held; reads UI/board state
    return true;
}

bool handle_switch(const proto::Switch &s, proto::SwitchAck &ack) {
    strncpy(ack.v, "1.0", sizeof(ack.v) - 1);

    // Validate + refresh the session using heartbeat() semantics: it returns
    // false for an unknown sessionId and refreshes lastSeen for a known one.
    bool known;
    lock();
    known = s_table.heartbeat(s.sessionId, (long)millis());
    if (known) {
        strncpy(s_current_view, s.viewId, sizeof(s_current_view) - 1);
    }
    unlock();

    if (!known) {
        ack.ok = false;
        strncpy(ack.reason, "no_session", sizeof(ack.reason) - 1);
        return false;
    }

    // Drive the view change via the UI-task command queue (never lv_obj_* /
    // ui::* from here). Mirrors web.cpp handle_screen_set: a full UI queue is a
    // dropped switch, so NAK it ("busy") instead of acking ok=true.
    app::Command cmd;
    cmd.type = app::CommandType::ShowScreen;
    strncpy(cmd.a, s.viewId, sizeof(cmd.a) - 1);
    if (!app::post(cmd, 50)) {
        ack.ok = false;
        strncpy(ack.reason, "busy", sizeof(ack.reason) - 1);
        return false;
    }

    ack.ok = true;
    strncpy(ack.currentView, s.viewId, sizeof(ack.currentView) - 1);
    return true;
}

bool handle_heartbeat(const char *sid) {
    bool ok;
    lock();
    ok = s_table.heartbeat(sid ? sid : "", (long)millis());
    unlock();
    return ok;
}

bool handle_detach(const char *sid) {
    bool ok;
    lock();
    ok = s_table.detach(sid ? sid : "");
    unlock();
    return ok;
}

void fill_state(proto::ControlState &cs) {
    lock();
    s_table.to_control_state(cs, live_current_view());
    unlock();
}

void tick(long now_ms) {
    int reaped;
    lock();
    reaped = s_table.reap(now_ms, proto::kDefaultTtlMs);
    unlock();
    // The colored-frame overlay (Phase 3.2) refreshes from
    // active_session_snapshot() on the UI task; nothing to post here yet.
    (void)reaped;
}

int active_session_snapshot(proto::Session *out, int cap) {
    if (!out || cap <= 0) return 0;
    int n = 0;
    lock();
    for (int i = 0; i < proto::kMaxSessions && n < cap; ++i) {
        if (s_table.used[i]) out[n++] = s_table.sessions[i];
    }
    unlock();
    return n;
}

}  // namespace proto_target
