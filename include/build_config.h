#pragma once

// Defaults for development builds. Release PlatformIO environments override
// these with -D flags so CI release artifacts do not expose debug/test controls.

#ifndef ESPDISP_RELEASE_BUILD
#define ESPDISP_RELEASE_BUILD 0
#endif

#ifndef ESPDISP_ENABLE_INPUT_TEST
#define ESPDISP_ENABLE_INPUT_TEST 1
#endif

#ifndef ESPDISP_ENABLE_BENCH
#define ESPDISP_ENABLE_BENCH 1
#endif

#ifndef ESPDISP_ENABLE_DEMO
#define ESPDISP_ENABLE_DEMO 1
#endif

#ifndef ESPDISP_ENABLE_STALL_TELEMETRY
#define ESPDISP_ENABLE_STALL_TELEMETRY 1
#endif
