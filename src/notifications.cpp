// Device-side glue for the notifications/alarms store: the PSRAM-allocated
// global singleton + the `sk-status` debug line. All store logic is
// header-inline in include/notifications.h (host-testable without a
// platformio build-filter change).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "notifications.h"

#include <new>
#include <stdio.h>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

namespace notif {

Store &store() {
    static Store *s = nullptr;
    if (!s) {
#ifdef ARDUINO
        // ~4.3 KB: PSRAM per the CLAUDE.md memory traps (never .bss, never a
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

int status_line(char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    Store &st = store();
    int n = st.count();
    int unacked = 0;
    Entry e;
    for (int i = 0; i < n; ++i)
        if (st.get(i, e) && !e.acknowledged) ++unacked;
    return snprintf(buf, cap, "notif=%d highest=%s unacked=%d", n,
                    state_name(st.highest_severity()), unacked);
}

}  // namespace notif
