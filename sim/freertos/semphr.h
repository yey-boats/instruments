#pragma once
// Sim shadow of freertos/semphr.h — just enough for headers that declare a
// SemaphoreHandle_t member under `#ifdef ARDUINO` (e.g. include/path_store.h)
// to compile in the host harness. No locking exists (or is needed) here: the
// sim renders are single-threaded and never compile the device .cpp that
// would create/take the semaphore.

typedef void *SemaphoreHandle_t;
