#pragma once

// One-time PSRAM allocation for large, long-lived app buffers.
//
// Internal SRAM is the scarce resource on this board: with WiFi, BLE,
// LVGL and the web server running, the S3 idles at ~10-15 KiB free
// internal heap, and every byte of static .bss/.data comes out of that.
// When it ran dry during an ArduinoOTA transfer, the WiFi RX path starved
// and the update failed (instruments#41). So a big scratch struct or ring
// buffer must not be a plain `static T x;`. Use
//
//     static T &x = *yeyboats::psram_new<T>();          // one object
//     static T *xs = yeyboats::psram_new<T>(N);          // array of N
//
// The function-local static runs the allocation once, on first use, and
// C++ guarantees thread-safe initialization. Objects are value-initialized
// (zeroed POD), the same as a zero-initialized static. They are never
// freed. tools/check_dram_budget.py fails CI if internal .bss grows again.
//
// Never use this for buffers touched from an ISR or while the flash cache
// is disabled, or for DMA buffers; those must stay in internal RAM.

#include <esp_heap_caps.h>
#include <stdlib.h>

#include <new>

namespace yeyboats {

template <typename T> T *psram_new(size_t count = 1) {
    void *mem = heap_caps_calloc(count, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) mem = heap_caps_calloc(count, sizeof(T), MALLOC_CAP_8BIT);  // no PSRAM: limp on
    if (!mem) abort();  // a static could not fail; don't hand back null
    T *p = static_cast<T *>(mem);
    for (size_t i = 0; i < count; ++i)
        new (p + i) T();
    return p;
}

}  // namespace yeyboats
