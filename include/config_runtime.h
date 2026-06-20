#pragma once

// Live config owner. Holds the single mutable RuntimeConfig; render/refresh
// reads via snapshot() (mutex-guarded short critical section). Mutations
// (theme/brightness/alarm thresholds/...) come in as typed Mutation events
// from web/BLE/serial/UI handlers, are validated + clamped, then applied to
// RAM with revision bump + dirty mark. The storage worker reads
// persist_pending domains on a debounce timer.
//
// Design intent (docs/specs/08):
//   - Apply changes to RAM first; render reads from RAM
//   - Persistence asynchronous and coalesced
//   - Failed write does not roll back live state

#include "config_model.h"
#include <stdint.h>

namespace config {

enum class MutationKind : uint8_t {
    SetTheme,
    SetBrightness,
    SetPosFormat,
    SetDefaultScreen,
    SetDepthAlarm,
    SetBatteryAlarm,
    SetSignalKTarget,  // host + port
    SetSignalKToken,
};

struct Mutation {
    MutationKind kind;
    Source source = Source::Default;
    // Bag-of-fields union to keep this header-only; mutations are small
    // and host-testable.
    Theme theme = Theme::Night;
    uint8_t u8 = 0;
    uint16_t u16 = 0;
    double d = 0.0;
    PosFormat pos_format = PosFormat::DDM;
    char s[80] = {0};
};

// Boot-time: load persisted domains from NVS (and legacy keys for
// back-compat) into the RAM model. Must be called once before any
// mutate() or snapshot(). Safe to call from setup().
void setup();

// Mutex-guarded full snapshot. Cheap (a struct copy under a short lock).
// Use for screens / web /api/config / alarm_check.
void snapshot(RuntimeConfig &out);

// Per-domain typed snapshots - avoid copying the whole struct.
UiConfig ui();
AlarmConfig alarms();
// Display number-formatting (decimals + k/M scaling) per unit class. Lives in
// the Ui domain; this is a convenience accessor used by the painters.
FormatConfig format();
SignalKConfig signalk();
DomainMeta meta(Domain d);

// Apply a typed mutation. Returns true if accepted (validated + clamped,
// RAM updated, revision bumped, persistence scheduled).
bool mutate(const Mutation &m);

// Force the storage worker to flush any pending writes immediately
// (e.g., on Settings close, reboot prep). Non-blocking; returns after
// enqueueing.
void flush_pending();

// Diagnostics (RAM-only - no NVS reads).
uint32_t persist_jobs_queued();
uint32_t persist_jobs_completed();
uint32_t persist_jobs_failed();
uint32_t coalesced_writes();

}  // namespace config
