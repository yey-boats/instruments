#pragma once
// Sim shadow of freertos/semphr.h — just enough for headers that declare (or
// header-inline use) a SemaphoreHandle_t under `#ifdef ARDUINO` — e.g.
// include/path_store.h (member only) and include/ais_store.h /
// include/notifications.h (header-inline create/take/give) — to compile in
// the host harness. No locking exists (or is needed) here: the sim renders
// are single-threaded, so the stubs are no-ops that always "succeed".

typedef void *SemaphoreHandle_t;

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xffffffffUL
#endif

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)1;  // non-null so `if (mtx_)` guards take/give
}
static inline int xSemaphoreTake(SemaphoreHandle_t /*sem*/, unsigned long /*ticks*/) {
    return 1;  // pdTRUE
}
static inline int xSemaphoreGive(SemaphoreHandle_t /*sem*/) {
    return 1;  // pdTRUE
}
