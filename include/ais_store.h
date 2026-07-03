#pragma once

// AIS target store (MFD overhaul phase 5 data layer). Fixed pool of nearby
// vessels keyed by MMSI, fed by the SignalK delta parser (context
// "vessels.urn:mrn:imo:mmsi:<n>") and the NMEA2000 adapter (PGNs
// 129038/129039 position, 129794 static). Consumer is the later-phase AIS
// plot screen, via snapshot().
//
// Header-inline pure C++ (same build-filter reasoning as notifications.h /
// n2k_decode.h) so the parser TU and host Unity suites use it without a
// platformio change. The device singleton is PSRAM-allocated in
// src/ais_store.cpp (~3.6 KB - never .bss, never a stack local per
// CLAUDE.md). Mutex behind #ifdef ARDUINO like PathStore: SK task + N2K
// task write, UI task snapshots.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace ais {

enum class VesselClass : uint8_t {
    Unknown = 0,  // SignalK targets (class not carried on the 3 nav paths)
    ClassA,       // PGN 129038 / 129794
    ClassB,       // PGN 129039
};

struct Target {
    uint32_t mmsi = 0;
    double lat_deg = NAN;
    double lon_deg = NAN;
    float sog_mps = NAN;
    float cog_rad = NAN;
    float heading_rad = NAN;  // NaN ok (Class B often omits)
    char name[24] = {0};
    VesselClass cls = VesselClass::Unknown;
    uint32_t last_seen_ms = 0;
};

class Store {
  public:
    static constexpr int CAP = 48;
    // Targets not heard from for this long age out (SOLAS Class A reports
    // every 2-10 s underway, every 3 min at anchor; 6 min = safely dead).
    static constexpr uint32_t MAX_AGE_MS = 6u * 60u * 1000u;

    Store() {
#ifdef ARDUINO
        mtx_ = xSemaphoreCreateMutex();
#endif
    }

    void clear() {
        lock_();
        count_ = 0;
        unlock_();
    }

    // Merge a position-ish report for `mmsi`. NaN arguments leave the stored
    // field untouched (SignalK delivers lat/lon, SOG and COG as separate
    // deltas; N2K delivers them together) - so per-path callers just pass
    // NaN for what this message didn't carry. lat/lon only land as a pair.
    // cls == Unknown never downgrades a known class. When the pool is full a
    // new MMSI evicts the stalest target. Returns index or -1 (bad mmsi).
    int upsert_position(uint32_t mmsi, double lat_deg, double lon_deg, float sog_mps, float cog_rad,
                        float heading_rad, VesselClass cls, uint32_t now_ms) {
        if (mmsi == 0) return -1;
        lock_();
        int i = find_or_alloc_(mmsi, now_ms);
        if (i < 0) {
            unlock_();
            return -1;
        }
        Target &t = targets_[i];
        if (!isnan(lat_deg) && !isnan(lon_deg)) {
            t.lat_deg = lat_deg;
            t.lon_deg = lon_deg;
        }
        if (!isnan(sog_mps)) t.sog_mps = sog_mps;
        if (!isnan(cog_rad)) t.cog_rad = cog_rad;
        if (!isnan(heading_rad)) t.heading_rad = heading_rad;
        if (cls != VesselClass::Unknown) t.cls = cls;
        t.last_seen_ms = now_ms;
        unlock_();
        return i;
    }

    // Merge static data (vessel name). Empty/NULL name keeps the stored one.
    int upsert_static(uint32_t mmsi, const char *name, VesselClass cls, uint32_t now_ms) {
        if (mmsi == 0) return -1;
        lock_();
        int i = find_or_alloc_(mmsi, now_ms);
        if (i < 0) {
            unlock_();
            return -1;
        }
        Target &t = targets_[i];
        if (name && name[0]) {
            strncpy(t.name, name, sizeof(t.name) - 1);
            t.name[sizeof(t.name) - 1] = 0;
        }
        if (cls != VesselClass::Unknown) t.cls = cls;
        t.last_seen_ms = now_ms;
        unlock_();
        return i;
    }

    // UI-facing: copy up to `max` live (non-aged) targets under the lock so
    // the plot renders from a stable local copy. Returns #copied. Does not
    // mutate the pool (age_out() reclaims slots).
    int snapshot(Target *out, int max, uint32_t now_ms) const {
        if (!out || max <= 0) return 0;
        lock_();
        int n = 0;
        for (int i = 0; i < count_ && n < max; ++i) {
            if ((uint32_t)(now_ms - targets_[i].last_seen_ms) > MAX_AGE_MS) continue;
            out[n++] = targets_[i];
        }
        unlock_();
        return n;
    }

    // Drop targets older than MAX_AGE_MS. Returns #removed.
    int age_out(uint32_t now_ms) {
        lock_();
        int removed = 0;
        for (int i = count_ - 1; i >= 0; --i) {
            if ((uint32_t)(now_ms - targets_[i].last_seen_ms) > MAX_AGE_MS) {
                targets_[i] = targets_[count_ - 1];  // order not meaningful
                --count_;
                ++removed;
            }
        }
        unlock_();
        return removed;
    }

    int count() const {
        lock_();
        int n = count_;
        unlock_();
        return n;
    }

    bool get(int i, Target &out) const {
        lock_();
        if (i < 0 || i >= count_) {
            unlock_();
            return false;
        }
        out = targets_[i];
        unlock_();
        return true;
    }

    // Lookup by MMSI (tests / debugging). Returns index or -1.
    int find(uint32_t mmsi) const {
        lock_();
        int i = find_(mmsi);
        unlock_();
        return i;
    }

  private:
    Target targets_[CAP];
    int count_ = 0;
#ifdef ARDUINO
    SemaphoreHandle_t mtx_ = nullptr;
    void lock_() const {
        if (mtx_) xSemaphoreTake(mtx_, portMAX_DELAY);
    }
    void unlock_() const {
        if (mtx_) xSemaphoreGive(mtx_);
    }
#else
    void lock_() const {}
    void unlock_() const {}
#endif

    int find_(uint32_t mmsi) const {
        for (int i = 0; i < count_; ++i)
            if (targets_[i].mmsi == mmsi) return i;
        return -1;
    }
    // Existing slot for mmsi, else a free slot, else evict the stalest
    // target (oldest last_seen_ms). Caller holds the lock.
    int find_or_alloc_(uint32_t mmsi, uint32_t now_ms) {
        int i = find_(mmsi);
        if (i >= 0) return i;
        if (count_ < CAP) {
            i = count_++;
        } else {
            i = 0;
            for (int j = 1; j < CAP; ++j)
                if ((int32_t)(targets_[j].last_seen_ms - targets_[i].last_seen_ms) < 0) i = j;
        }
        targets_[i] = Target{};
        targets_[i].mmsi = mmsi;
        targets_[i].last_seen_ms = now_ms;
        return i;
    }
};

// Global device singleton (PSRAM-allocated; see src/ais_store.cpp). Host
// tests instantiate their own local Store instead.
Store &store();

}  // namespace ais
