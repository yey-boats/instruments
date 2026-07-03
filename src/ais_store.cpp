// Device-side glue for the AIS target store: the PSRAM-allocated global
// singleton. All store logic is header-inline in include/ais_store.h
// (host-testable without a platformio build-filter change).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "ais_store.h"

#include <new>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

namespace ais {

Store &store() {
    static Store *s = nullptr;
    if (!s) {
#ifdef ARDUINO
        // ~3.6 KB: PSRAM per the CLAUDE.md memory traps (never .bss, never a
        // task stack). Same pattern as sk::dynamicStore().
        void *mem = heap_caps_calloc(1, sizeof(Store), MALLOC_CAP_SPIRAM);
        s = mem ? new (mem) Store() : new Store();  // heap fallback
#else
        static Store host;
        s = &host;
#endif
    }
    return *s;
}

}  // namespace ais
