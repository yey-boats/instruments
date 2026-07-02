#pragma once
// Sim shadow of the ESP-IDF FreeRTOS umbrella header. The sim envs define
// -DARDUINO=10805 (so Arduino-gated APIs are declared) which also turns on
// the `#ifdef ARDUINO` FreeRTOS includes in include/path_store.h. The host
// harness is single-threaded and never compiles src/path_store.cpp, so only
// the type names are needed; see sim/freertos/semphr.h.
