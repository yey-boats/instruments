# Architecture Refactor Plan

Goal: improve responsiveness and stability by making LVGL single-owner, moving
slow I/O off the UI loop, and using FreeRTOS queues/events for cross-task work.

## Context

The project already uses the right core frameworks:

- LVGL for UI
- FreeRTOS tasks, queues, semaphores, and event groups
- Arduino `WebServer` / `DNSServer`
- NimBLE
- ArduinoOTA
- PlatformIO
- Signal K over WebSocket / REST

Keep those frameworks. The refactor should formalize ownership boundaries rather
than introduce a new runtime architecture.

## Target Architecture

```text
Core 1: LVGL/UI owner
  - lv_timer_handler
  - touch
  - screen changes
  - screenshots
  - widget/layout apply

Core 0: I/O services
  - WebServer + DNSServer
  - WiFi manager
  - Signal K WebSocket/HTTP writes
  - BLE callbacks

Shared boundary
  - FreeRTOS queues for commands
  - FreeRTOS event groups for status
  - mutex only for small read-only snapshots
```

Hard rule: only the LVGL/UI owner task may call LVGL APIs or functions that
mutate LVGL-backed UI state.

## Phase 1: App Event Queue

Add `include/app_events.h` and `src/app_events.cpp`.

Define a small command queue:

```cpp
namespace app {

enum class CommandType {
    ShowScreen,
    ApplyLayout,
    SetTheme,
    SetBrightness,
    SignalKPut,
    SaveWifi,
    Reboot,
};

struct Command {
    CommandType type;
    char a[96];
    char b[256];
    int32_t i = 0;
};

void setup();
bool post(const Command &cmd, uint32_t timeout_ms = 0);
void pump();  // called only from the main LVGL loop

}  // namespace app
```

Rules:

- `app::pump()` is the only place allowed to call LVGL APIs, `ui::show*()`, and
  `layout::apply_json()`.
- Web/BLE/network callbacks only call `app::post()`.
- Keep payloads fixed-size initially to avoid heap/lifetime bugs.
- If a command needs a larger payload later, use an explicit owner/free contract
  or a PSRAM-backed staging buffer with a mutex.

Acceptance:

- Queue initializes during boot.
- Main loop or `ui_refresh()` calls `app::pump()`.
- Existing behavior is unchanged when commands are posted from the UI task.

## Phase 2: Remove Web to LVGL Direct Calls

Change `src/web.cpp`:

- `/api/screen/<id>` posts `ShowScreen` instead of calling `ui::show()` or
  `ui::show_by_id()` directly.
- `/api/layout PUT` posts `ApplyLayout` instead of calling
  `layout::apply_json()` directly.
- `/api/cmd` should not directly execute UI commands that touch LVGL. Either
  keep it for read/status commands only, or route mutating commands through
  `app::post()`.

Change `src/main.cpp`:

- Call `app::setup()` during setup.
- Call `app::pump()` inside `loop()` or `ui_refresh()`.
- Keep screenshot serving in the LVGL task.

Acceptance:

- No `ui::show`, `layout::apply_json`, or LVGL calls from `web.cpp`.
- `/api/screen/<id>` still changes screens.
- `/api/layout PUT` still returns a useful success/failure response.

## Phase 3: BLE Callback Safety

Change `src/ble_config.cpp` and the NUS command path in `src/net.cpp`:

- BLE writes that change screen/layout/theme/brightness post app commands.
- WiFi and Signal K target changes can remain in network modules if they reboot,
  but prefer queued `SaveWifi` / `Reboot` for consistency.
- Keep BLE callbacks short.
- Avoid slow work or large parsing inside NimBLE callbacks beyond small JSON
  validation.

Acceptance:

- BLE callbacks do not perform LVGL work.
- BLE callbacks do not perform long blocking HTTP/WiFi work.
- Existing BLE console/config behavior remains available.

## Phase 4: Async WiFi Manager

Refactor `src/net.cpp`:

- `net::setup()` should initialize prefs/device id, start BLE, then start a WiFi
  manager task.
- The WiFi manager task tries saved networks, starts AP fallback, and starts
  OTA/mDNS when STA connects.
- Add status enum:

```cpp
namespace net {

enum class WifiState {
    Idle,
    Connecting,
    StaUp,
    ApSetup,
    Failed,
};

WifiState wifiState();

}  // namespace net
```

- Keep existing wrappers: `net::wifiUp()`, `net::ipString()`, `net::rssi()`.
- Prefer Arduino WiFi events where practical.
- UI should start immediately, before STA attempts finish.

Acceptance:

- Cold boot with bad saved networks shows UI quickly.
- AP fallback still starts with captive portal.
- OTA still starts after STA success.
- Existing web and BLE state APIs report useful intermediate states.

## Phase 5: Async Signal K Writes

Refactor autopilot and other Signal K writes:

- Do not run synchronous `HTTPClient::PUT()` from LVGL event handlers.
- Add a Signal K outbound queue, or reuse `app::CommandType::SignalKPut` while
  executing the actual HTTP request from a network-owned task.
- `ui::autopilot` button handlers enqueue desired state/heading.
- Store last PUT result in a small status struct read by UI refresh.

Acceptance:

- Pressing autopilot buttons cannot block touch/rendering during slow HTTP.
- Failed Signal K PUTs are surfaced through logs/status without freezing UI.

## Phase 6: Shared State Guards

Add synchronization where shared state crosses tasks:

- `layout_loader.cpp`: protect `s_current`, `s_last_json`, and lengths with a
  mutex, or make all mutation happen via the UI/app task and reads copy under a
  mutex.
- `sk::data`: add a mutex or double-buffer snapshot.
  Recommended approach: parser writes `s_data_back`, swaps to `s_data_front`
  under a short critical section; UI reads a copied snapshot.
- `wifi_store`: guard saved network list if web/BLE can modify it while the WiFi
  manager reads it.

Acceptance:

- No free/replace of shared buffers while another task can read them.
- UI can copy a coherent Signal K data snapshot.
- Web/BLE can read layout JSON without racing an apply.

## Phase 7: Metrics and Watchdog Diagnostics

Add a lightweight diagnostics module or extend existing status reporting with:

- last LVGL loop timestamp
- max `lv_timer_handler()` duration
- app queue depth / high-water mark
- WiFi state
- Signal K state
- heap and PSRAM free / low-water marks
- last OTA result
- last Signal K PUT result

Expose via:

- `bench`
- `/api/state`
- BLE connection JSON

Acceptance:

- UI stalls can be diagnosed without attaching a serial debugger.
- `/api/state` remains compact enough for frequent polling.

## Phase 8: Tests

Extend native tests where practical:

- Command routing parser tests if command parsing is extracted.
- Layout apply invalid/valid behavior.
- WiFi store edge cases:
  - duplicate SSID
  - max length SSID/password
  - corrupt JSON
  - remove while list is full
- Signal K parser tests remain as-is.

Manual hardware tests:

1. Boot with no saved WiFi: UI appears quickly, AP captive portal works.
2. Boot with bad saved WiFi: UI appears quickly, AP fallback starts.
3. Boot with good WiFi: dashboard appears, OTA starts.
4. Hit `/api/screen/wind` repeatedly while swiping: no crash.
5. PUT layout while refreshing web UI: no crash.
6. Autopilot PUT with Signal K unplugged: UI remains responsive.
7. Screenshot endpoint still works.

## Priority Order

Implement in this order:

1. Phase 1: App Event Queue
2. Phase 2: Remove Web to LVGL Direct Calls
3. Phase 5: Async Signal K Writes
4. Phase 6: Shared State Guards
5. Phase 3: BLE Callback Safety
6. Phase 4: Async WiFi Manager
7. Phase 7: Metrics and Watchdog Diagnostics
8. Phase 8: Tests

Phases 1, 2, 5, and 6 address the highest-risk responsiveness and crash issues.
Phase 4 is larger and should happen after the queue boundary is stable.

## Implementation Constraints

- Use existing frameworks: LVGL, FreeRTOS, Arduino `WebServer`, NimBLE, and
  PlatformIO.
- Preserve current behavior and public endpoints unless this plan explicitly
  changes routing internals.
- LVGL must be single-owner.
- Web/BLE callbacks must post commands to queues instead of doing UI work or
  long blocking work.
- Keep edits incremental.
- Run `make test` and `make build` after each major phase.
- Do not introduce dynamic heap ownership across queues unless lifetime is
  explicit and safe.
- Do not rewrite UI design or screen modules except where necessary for queue
  integration.

## Suggested Claude Code Prompt

```text
Implement the responsiveness/stability refactor described in
ARCHITECTURE_REFACTOR_PLAN.md.

Constraints:
- Use existing frameworks: LVGL, FreeRTOS, Arduino WebServer, NimBLE,
  PlatformIO.
- Preserve current behavior and public endpoints unless the plan explicitly
  changes routing internals.
- LVGL must be single-owner: only the main/LVGL task may call LVGL APIs or
  ui::show/layout apply functions that touch UI state.
- Web/BLE callbacks must post commands to queues instead of doing UI or long
  blocking work.
- Keep edits incremental and run make test and make build after each major
  phase.
- Do not introduce dynamic heap ownership across queues unless lifetime is
  explicit and safe.
- Do not rewrite the UI design or screen modules except where necessary for
  queue integration.
```
