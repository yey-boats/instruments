#pragma once

// Defaults for development builds. Release PlatformIO environments override
// these with -D flags so CI release artifacts do not expose debug/test controls.

#ifndef YEYBOATS_RELEASE_BUILD
#define YEYBOATS_RELEASE_BUILD 0
#endif

#ifndef YEYBOATS_ENABLE_INPUT_TEST
#define YEYBOATS_ENABLE_INPUT_TEST 1
#endif

#ifndef YEYBOATS_ENABLE_BENCH
#define YEYBOATS_ENABLE_BENCH 1
#endif

#ifndef YEYBOATS_ENABLE_DEMO
#define YEYBOATS_ENABLE_DEMO 1
#endif

#ifndef YEYBOATS_ENABLE_STALL_TELEMETRY
#define YEYBOATS_ENABLE_STALL_TELEMETRY 1
#endif

// Touch calibration UI screen. The core calibration application
// (touch_cal::apply / set / reset in src/touch_cal.cpp) stays in all
// builds - it's what reads the saved matrix from NVS and remaps
// touch input every frame. This flag only controls the SETUP UI
// (src/ui/screen_touch_cal.cpp), which is a one-shot first-boot
// flow and isn't needed on shipped devices that have already been
// calibrated at the factory. Disabling drops ~3 KiB of Flash + the
// "touch_cal" hidden screen.
#ifndef YEYBOATS_ENABLE_TOUCH_CAL_UI
#define YEYBOATS_ENABLE_TOUCH_CAL_UI 1
#endif

// Manager provisioning token sent as `X-EspDisp-Authorization: Bearer <tok>`
// on the very first /devices/register call when the device has no saved
// device token. The plugin's default `dev-shared-token` mode accepts
// "yeyboats-dev" as a valid provisioning credential AND issues it back as
// the device token, so a fresh device can self-provision. In production
// builds override via -D MANAGER_PROVISION_TOKEN=\"...\" with whatever the
// admin configured as the plugin's auth.devToken (or batch provision
// token). Set to "" to opt out of auto-provisioning entirely.
#ifndef MANAGER_PROVISION_TOKEN
#define MANAGER_PROVISION_TOKEN "yeyboats-dev"
#endif
