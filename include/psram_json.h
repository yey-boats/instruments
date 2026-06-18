#pragma once

// Shared PSRAM-backed ArduinoJson allocator.
//
// The ESP32-S3 idles around 10-15 KiB free internal heap once LVGL +
// WiFi + BLE + SK + WebServer + NimBLE pools are all running; allocating
// a multi-KiB JsonDocument on internal heap is a documented cause of
// random reboots (see docs/manager-psram-benchmark.md). PSRAM has
// ~7.5 MiB free.
//
// Any module that builds or parses JsonDocuments on a hot path - manager
// HTTP handlers, the web server's /api/state - should pass &psram_json
// to its JsonDocument so the tree lives in PSRAM instead.
//
// Falls back to internal heap when PSRAM is exhausted (effectively never,
// but the fallback keeps the device limping instead of crashing if it
// ever gets fragmented).

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

namespace yeyboats {

class PsramJsonAllocator : public ArduinoJson::Allocator {
  public:
    void *allocate(size_t size) override {
        void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ptr) ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
        return ptr;
    }
    void deallocate(void *ptr) override { heap_caps_free(ptr); }
    void *reallocate(void *ptr, size_t new_size) override {
        void *out = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) out = heap_caps_realloc(ptr, new_size, MALLOC_CAP_8BIT);
        return out;
    }
};

// Shared singleton. Define in exactly one TU via PSRAM_JSON_DEFINE_SHARED
// before #include of this header. Other TUs see only the extern.
extern PsramJsonAllocator psram_json;

}  // namespace yeyboats

#ifdef PSRAM_JSON_DEFINE_SHARED
namespace yeyboats {
PsramJsonAllocator psram_json;
}  // namespace yeyboats
#endif
