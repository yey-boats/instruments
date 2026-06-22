# Arduino → ESP-IDF Migration

> **Target platform.** Today the firmware is **arduino-esp32 2.x on a
> precompiled esp-idf 4.4.7 core** (`platform = espressif32@^6.7.0`,
> `framework = arduino`). The migration target is **esp-idf 5.x via the
> pioarduino hybrid framework** (`framework = arduino, espidf`,
> `platform = pioarduino .../53.03.13`). The hybrid **rebuilds IDF from
> source**, which is what makes `sdkconfig.defaults` actually take effect
> — the single fact the whole migration turns on.
>
> **Status (2026-06-21).** The hybrid env (`[env:esp32-4848s040-idf5]`)
> **links and builds a full `firmware.bin`**. The two blockers that were on
> the critical path are both addressed: the `ssl_client` link error (§A.1)
> was an empty-object problem fixed by enabling mbedTLS PSK in
> `sdkconfig.defaults` (the "duplicate-library" theory was wrong — see
> §A.1), and the **`Arduino_RGB_Display` gap on arduino-3.x** (§H) is
> resolved by the esp_lcd RGB path (`display_db_init`), now running
> `num_fbs=2` DIRECT double-buffer (bounce buffers deferred). **Not yet
> hardware-validated** — needs a USB flash + screen walk + flicker check.
> Everything below the line is the dependency-ordered module work.

## Why now — three production drivers

This is no longer a flash-savings / tidiness refactor. Three concrete,
recent pain points each point at the same root cause — **no control over
`sdkconfig` / the bootloader because the Arduino core is precompiled** —
and each is only properly fixable on IDF5/hybrid.

### 1. OTA reliability hang (production-severity, this week)

A manager-pull OTA **hung the device mid-flash and required a physical
power-cycle** — unacceptable for a deployed marine display. Root cause is
a heap + flash-cache double-bind:

- **Heap starvation.** Internal-SRAM low-water is already ~22 KB. The pull
  path runs, concurrently: an HTTPS download, `Update.write` (2 MB flash
  erase/write), a streaming sha256, and progress POSTs back to the
  manager. Together they exhaust the internal heap.
- **Flash-cache vs. live panel DMA.** A sustained flash write disables the
  flash cache; meanwhile flash-resident code (the LVGL task) keeps running
  against the RGB panel DMA + PSRAM framebuffer. The write races the cache
  against live rendering.

Interim 4.4 mitigation **already shipped**: pause the LVGL pump during OTA
(`app_pause_ui`, in `src/main.cpp` / `src/manager.cpp`). That reduces the
contention but does not remove it — the real fix is an **IRAM-safe
esp_lcd** path plus **heap headroom**, both of which require `sdkconfig`
control, i.e. the hybrid/IDF5 build.

### 2. No OTA rollback / auto-recovery

A bad OTA cannot auto-recover. Confirm-after-boot
(`ESP_OTA_IMG_PENDING_VERIFY` → `esp_ota_mark_app_valid`) **cannot fire**
because the precompiled Arduino bootloader was built **without**
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` — there is no pending-verify state
to confirm. So a firmware that boots but is broken (or never reaches the
"valid" point) bricks the device to a power-cycle + USB re-flash.

IDF5/hybrid lets us **rebuild the bootloader + sdkconfig with rollback
enabled**. An interim 4.4 workaround is an **NVS confirm-flag** (write a
"booted OK" flag on first healthy heartbeat; a watchdog/boot-counter
forces last-known-good if it is missing) — a software emulation of what
rollback gives for free once the bootloader can be rebuilt.

### 3. Flicker-free + fast render needs esp_lcd double-buffer (≥ IDF 5.0)

Tear-free, high-FPS rendering needs **esp_lcd RGB `num_fbs = 2` +
vsync-driven page-flip**, which is **esp-idf ≥ 5.0 only**. On 4.4 we hit a
hard ceiling:

- Shipped on 4.4 (the ceiling): quantize-and-cache rotations, a
  **dedicated LVGL task**, fit-to-width and value scaling (spec 09). This
  is single-buffer; it reduces redraw cost but **cannot give tear-free
  double-buffering**.
- `LV_DISPLAY_RENDER_MODE_DIRECT` on the single framebuffer **flickers**
  (tearing on the live scan-out) — confirmed on the bench.
- The RGB + PSRAM + flash combination triggers a **"Cache disabled but
  cached memory region accessed" panic** unless the RGB refill ISR is
  IRAM-safe (`CONFIG_LCD_RGB_ISR_IRAM_SAFE=y` + bounce buffers). The
  **precompiled Arduino esp_lcd is not IRAM-safe**, so this is unreachable
  on 4.4 — only the hybrid (which regenerates `sdkconfig`) can set it.

Net: double-buffer + IRAM-safe ISR are the same `sdkconfig` lever, and the
same lever that fixes driver #1's flash-cache race.

### 4 (cross-cutting). Heap / BLE headroom needs sdkconfig

Internal-SRAM low-water swings to **~1–22 KB** with BLE (NimBLE 2.x) +
esp_lcd + the LVGL task all live. NimBLE memory tuning (host task stack,
ACL buffer counts, GATT cache) is `sdkconfig`-only — unreachable on the
precompiled core. LVGL allocations already go to PSRAM (good); the
pressure is internal SRAM, exactly the resource `sdkconfig` lets us trim
BLE/Wi-Fi/driver buffers to free.

### 5 (forward-looking). MIDL runtime rendering raises the stakes

The device is gaining **runtime dashboard rendering from YB-MIDL** (spec:
`docs/superpowers/specs/2026-06-19-generic-dashboard-runtime-design.md`;
`midl/` git submodule; active branch `feat/midl-firmware-render`). MIDL
builds LVGL widget trees **at runtime from a config document** instead of
the current compile-time hand-built `screen_*.cpp` HUDs. That means **more
heap/PSRAM churn and more dynamic LVGL object construction/teardown** on
config apply. Two consequences for this migration, both pulling it
*earlier*:

- it makes the ~22 KB internal-heap ceiling (driver #1, #4) **more
  dangerous** — a runtime-built tree is exactly when you least want to be
  one allocation from the floor;
- smooth dynamic dashboards **benefit strongly from tear-free
  double-buffering** (driver #3) — rebuilt screens flipping in on vsync
  rather than tearing.

So the migration should be **sequenced with the MIDL firmware work, not
after it** (see §M).

## Goal

Move to **esp-idf 5.x via the pioarduino hybrid framework**, then drop the
Arduino-ESP32 wrapper module-by-module until `framework = espidf` alone.
The hybrid is the vehicle; the production payoff is the four drivers
above (rollback-safe OTA, IRAM-safe esp_lcd, heap headroom, double-buffer)
— flash savings (~120 KB) and one-fewer-release-notes are now secondary.

The firmware already runs on top of ESP-IDF — Arduino-ESP32 is a layer of
C++ classes plus a handful of helpers (`Serial`, `String`,
`Preferences`, `WiFi`, `HTTPClient`, `ESPmDNS`, `ArduinoOTA`) wrapping
ESP-IDF and FreeRTOS calls.

## What IDF5/hybrid unlocks (mapped to the drivers)

| Unlock (sdkconfig / bootloader / API) | Fixes driver | Reachable on 4.4? |
|---|---|---|
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + `ESP_OTA_IMG_PENDING_VERIFY` | #2 rollback | No (precompiled bootloader) — NVS-flag emulation only |
| `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y` + bounce buffers | #1 flash-cache race, #3 panic | No (precompiled esp_lcd not IRAM-safe) |
| esp_lcd RGB `num_fbs=2` + vsync page-flip | #3 tear-free, #5 MIDL smoothness | No (needs IDF ≥ 5.0) |
| NimBLE/Wi-Fi/driver buffer trims via `sdkconfig` | #4 heap headroom, #1 OTA heap | No (baked sdkconfig) |
| Streaming `esp_http_client` + `esp_ota_*` | #1 lower-overhead OTA path | Partly (Arduino exposes some) |

The recurring word is **`sdkconfig`**: the hybrid rebuilds IDF from
source, so `sdkconfig.defaults` finally takes effect. That is the entire
reason the hybrid is the migration vehicle rather than a straight port.

## Non-goals

- Rewriting LVGL (it's framework-agnostic C; already linked directly).
- Rewriting ArduinoJson (also framework-agnostic; host tests already
  link it without Arduino).
- Rewriting the SignalK plugin or any spec 17/18/19/20 contracts.
- Re-deriving the MIDL config grammar — that is the runtime-rendering
  spec's job; this spec only sequences the platform move around it.
- Changing PCB / pin maps. The verified ST7701 init table in
  `board_pins.h` and the GT911 quirks must round-trip unchanged.

## Migration model

Hybrid PlatformIO framework (`framework = arduino, espidf`) used as a
transition vehicle. Both stay linked, so each module migrates in a
self-contained commit and the bench stays usable throughout. The
final cutover removes `arduino` from the list.

Migration is sequenced by dependency (storage and WiFi must be alive
before HTTP can move; HTTP must be alive before OTA can move) and by
risk (display + BLE are the biggest rewrites; schedule them after the
cheap mechanical work has shaken out the build system).

---

## A. Hybrid framework + IDF 5 bump (the critical path)

### Scope

Stand up `[env:esp32-4848s040-idf5]` on the **pioarduino** platform with
`framework = arduino, espidf`, driven by `sdkconfig.defaults`, and get it
to **boot on the bench**. This is no longer a "switch one line in
`platformio.ini`" change — the precompiled `espressif32@^6.x` core never
rebuilds IDF, so it can't apply `sdkconfig`; only the source-rebuilding
pioarduino hybrid can. The env and defaults already exist; the remaining
work is the **link blocker (§A.1)** and the **display port (§H)**.

### The working hybrid recipe (already in-tree)

`platformio.ini`, `[env:esp32-4848s040-idf5]`:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13/platform-espressif32.zip
framework = arduino, espidf
custom_component_remove =
    espressif/esp_rainmaker
    espressif/rmaker_common
    espressif/esp_insights
    espressif/esp_diagnostics
    espressif/esp-sr
    espressif/esp-zboss-lib
    espressif/esp-zigbee-lib
    espressif/esp-dsp
    espressif/esp-modbus
    espressif/esp_modem
    espressif/network_provisioning
    chmorgan/esp-libhelix-mp3
```

Two non-obvious pieces of the recipe, both load-bearing:

1. **`custom_component_remove`, not `EXCLUDE_COMPONENTS`.** Arduino's
   `idf_component.yml` pulls a large IoT stack (RainMaker, Insights,
   esp-sr, Zigbee, DSP, modbus, modem, …) we use none of.
   `EXCLUDE_COMPONENTS` only excludes *after* fetch, so
   `esp_rainmaker`'s `https_server.crt` embed still ran and tripped a
   **PlatformIO doubled-embed-path bug**. `custom_component_remove` is the
   pioarduino mechanism that **deletes** the deps from the component
   manifest so the manager never fetches them. Keep `mdns`, `qrcode`,
   `libsodium`, `littlefs`.

2. **`sdkconfig.defaults` (in-tree) carries the unlocks** — because the
   hybrid rebuilds IDF from source, these finally apply:

   ```ini
   CONFIG_FREERTOS_HZ=1000
   CONFIG_SPIRAM_MODE_OCT=y
   CONFIG_SPIRAM_SPEED_80M=y
   CONFIG_ESP32S3_SPIRAM_SUPPORT=y
   CONFIG_SPIRAM=y
   CONFIG_LCD_RGB_ISR_IRAM_SAFE=y      ; driver #1/#3 — RGB refill ISR in IRAM
   CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y     ; pairs with bounce buffers
   CONFIG_BT_ENABLED=y
   CONFIG_BT_BLUEDROID_ENABLED=n
   CONFIG_BT_NIMBLE_ENABLED=y          ; NimBLE host (Bluedroid off)
   CONFIG_AUTOSTART_ARDUINO=y          ; Arduino provides app_main() → setup()/loop()
   ```

   The SPIRAM octal block is the same class of trap as `qio_opi` in
   CLAUDE.md: omit it and the octal-PSRAM cache config mismatches and the
   app faults **at entry** on every boot (silent reboot, no panic text).
   `CONFIG_AUTOSTART_ARDUINO=y` is what gives the hybrid an `app_main`
   (Arduino's loopTask) — without it the link fails on `undefined
   reference to app_main`. **To enable rollback (driver #2)** add
   `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` here once the env links — it
   only takes effect under the source-rebuilding hybrid.

### A.1 LINK blocker — `ssl_client` empty object (RESOLVED 2026-06-21)

The hybrid build reached the **LINK stage** and failed with `undefined
reference to start_ssl_client / ssl_init / send_ssl_data / ...` from
`NetworkClientSecure.cpp.o` (pulled in transitively by `manager.cpp`'s
`#include <HTTPClient.h>` — arduino-esp32 3.x's `HTTPClient` drags in
`NetworkClientSecure` for `https` support even though we use no TLS).

**The earlier "pioarduino duplicate-library / archive-order" diagnosis was
wrong.** The real cause: arduino-esp32's
`libraries/NetworkClientSecure/src/ssl_client.cpp` wraps its **entire
body** (every `start_ssl_client`, `ssl_init`, `send_ssl_data`, … definition)
in:

```c
#if !defined(MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED)
  #warning "...Enable pre-shared-key ciphersuites..."
#else
  ...all function definitions...
#endif
```

The stock **precompiled** Arduino sdkconfig enables PSK ciphersuites, so
that umbrella macro is defined and the bodies compile. Our hybrid
**regenerates sdkconfig from `sdkconfig.defaults`**, and without PSK the
macro is undefined → `ssl_client.cpp.o` compiles to an **empty object**
(0 symbols, verified with `nm`) → the `NetworkClientSecure` references are
unresolved at link. `lib_archive`/archive-order knobs do nothing because
there is no symbol in *any* copy to resolve.

**Fix (shipped):** enable PSK key exchange in `sdkconfig.defaults`:

```ini
CONFIG_MBEDTLS_PSK_MODES=y
CONFIG_MBEDTLS_KEY_EXCHANGE_PSK=y
```

`MBEDTLS_KEY_EXCHANGE_PSK` depends on `MBEDTLS_PSK_MODES` (mbedtls Kconfig);
enabling the child defines the `MBEDTLS_KEY_EXCHANGE_SOME_PSK_ENABLED`
umbrella that gates the file. After the change, `ssl_client.cpp.o` has its
14 symbols and `-idf5` **links to a full `firmware.bin`**.

**Gotcha that cost a rebuild:** IDF only applies `sdkconfig.defaults` when
it *generates* `sdkconfig.<env>` the first time; once that file exists it is
reused and `.defaults` edits are ignored. Delete the stale generated
`sdkconfig.esp32-4848s040-idf5` (it is gitignored via `sdkconfig.*`) to
force regeneration after editing `.defaults`.

With A.1 resolved, the remaining critical-path item for §A is the §H
display port (now also landed in first form — `num_fbs=2` double-buffer,
see §H).

### A.2 BLE temporarily disabled in the spike slice

The current `-idf5` env disables BLE (`-DYEYBOATS_DISABLE_BLE`, NimBLE
dropped from `lib_deps`) to shrink the first booting slice. **NimBLE-Arduino
1.4 does not build on IDF 5** (`NimBLEDevice.h` → `esp_bt.h` moved out of
the global include path); **NimBLE-Arduino 2.x** builds against the
Arduino-3.x API the pioarduino platform ships and against IDF5, so the
sequence is: land the booting display slice with BLE off, then re-enable
BLE on NimBLE-Arduino 2.x, then do the §J C-API rewrite. So **§J is the
effective long-pole for full feature parity**, but it no longer *blocks*
the first IDF5 boot.

### Arduino-3.x API breakage to expect on first compile

- `ledcSetup` / `ledcAttachPin` → new `ledcAttach` 4-arg form
  (`board::set_backlight`).
- `WiFi.onEvent` callback shape changed.
- `attachInterrupt` arg cast tightened.
- `~36` `%lu`/`%ld` format-string sites become **errors** (`uint32_t` is
  `unsigned int` on xtensa) — cast `(unsigned long)x` or switch to
  `%u`/`%d`. Mechanical, ~1–2 h; belongs in §B and can land on 4.4 first
  to decouple it from A.
- One pre-existing `-Wmisleading-indentation` in
  `src/boards/board_cli.cpp:30` surfaces under the stricter flags — fix
  in §B.
- Stale global platform forks (e.g. a Tasmota `espressif32@2023.6.2`) can
  win SemVer resolution — `pio platform uninstall espressif32 -y` before
  building the pinned pioarduino URL.

### Test plan

`pio run -e esp32-4848s040-idf5` links → USB flash → **boots** (no entry
fault / silent reboot) → walk all 14 screens → host suite → Lane A smoke
(SK live, manager-status hb=200). Internal-SRAM low-water logged; confirm
headroom vs. the 4.4 baseline.

### Estimate

L — 5-7 days **remaining** (the blocker in A.1 + the §H display port
dominate; the env/defaults/recipe are done). Confidence: medium — A.1 is
the unknown.

---

## B. Mechanical IDF call replacements

### Scope

Sed-pass replacements that don't need their own module structure:

- `Serial.printf(...)` → `printf(...)` (or `ESP_LOGI(TAG, ...)`)
- `millis()` → `(uint32_t)(esp_timer_get_time() / 1000)`
- `delay(n)` → `vTaskDelay(pdMS_TO_TICKS(n))` (most are already done)
- `pinMode` / `digitalWrite` / `digitalRead` → `gpio_config_t` +
  `gpio_set_level` / `gpio_get_level`
- `ledcSetup` / `ledcAttachPin` / `ledcWrite` → `ledc_timer_config` +
  `ledc_channel_config` + `ledc_set_duty` + `ledc_update_duty`
- `attachInterrupt(digitalPinToInterrupt(pin), fn, mode)` →
  `gpio_install_isr_service` + `gpio_isr_handler_add`

### Design

Where the IDF call has 3+ steps (LEDC, GPIO config) wrap in a tiny
helper at the point of use. No new module abstractions.

### Risks

- GPIO ISR service is process-wide; install exactly once.
- LEDC clock source choice (`LEDC_USE_APB_CLK` vs `LEDC_USE_RTC8M_CLK`)
  affects PWM frequency stability for the backlight.

### Test plan

Backlight set/get round-trip + touch IRQ ISR count grows.

### Estimate

S — 3 days, can run in parallel with C.

---

## C. Preferences → storage::Namespace (NVS direct)

### Scope

Replace ~24 call sites of `Preferences p; p.begin(NS); p.getString/...`
with a thin C++ wrapper over `nvs_handle_t`. Existing NVS data (same
namespaces, same keys, same types) stays compatible.

### Design

```cpp
// include/storage.h
namespace storage {

class Namespace {
 public:
    Namespace(const char *name, bool readonly);
    ~Namespace();  // commits on RW, closes always

    std::string get_string(const char *key, const char *default_ = "");
    void        put_string(const char *key, const char *value);
    uint8_t     get_u8 (const char *key, uint8_t  default_ = 0);
    void        put_u8 (const char *key, uint8_t  value);
    uint32_t    get_u32(const char *key, uint32_t default_ = 0);
    void        put_u32(const char *key, uint32_t value);
    int8_t      get_i8 (const char *key, int8_t   default_ = 0);
    void        put_i8 (const char *key, int8_t   value);
    void        remove(const char *key);
    bool        ok() const;
 private:
    nvs_handle_t handle_;
    bool readonly_;
    bool ok_;
};

}  // namespace storage
```

Each existing `load_prefs`/`save_prefs` becomes a `storage::Namespace
p("ns", ro);` + identical key list. Per-module commits.

### Risks

- NVS namespace names are limited to 15 chars in IDF. Audit the existing
  names: `mgr`, `n0183w`, `n2k`, `ap`, `ui`, `web`, `sk`, `wifi-store`
  — all ≤ 15. ✓
- Existing values get written by the Arduino `Preferences` wrapper today.
  Verify the binary layout for u8/u32/string is identical between the
  two (it should be; `Preferences` IS a thin wrapper over `nvs_*`).

### Test plan

Round-trip every key. Reboot persistence verified by `make flash` then
checking `/api/state` reports the last-saved value.

### Estimate

M — 2 days. Confidence: high.

---

## D. WiFi → esp_wifi + esp_netif

### Scope

Rewrite `src/net.cpp` STA + AP path. Replace `WiFi.begin`,
`WiFi.softAP`, `WiFi.status`, `WiFi.localIP`, `WiFi.RSSI`,
`WiFi.scanNetworks`, `WiFi.onEvent`, `DNSServer` (captive portal) with
ESP-IDF equivalents.

### Design

Event-driven state machine on the IDF default event loop:

```text
WIFI_EVENT_STA_START          → esp_wifi_connect()
WIFI_EVENT_STA_DISCONNECTED   → backoff + reconnect or rotate to next
                                stored network
IP_EVENT_STA_GOT_IP           → set s_wifi_state = Up; net::wifiUp() true
WIFI_EVENT_AP_STACONNECTED    → log
WIFI_EVENT_SCAN_DONE          → publish results to /api/wifi/scan
```

Multi-network store keeps the same shape (post-Spec C). Captive portal
gets a 50-line custom DNS server (UDP/53, respond to all queries with
device IP). Drop Arduino's `DNSServer` dependency.

Public surface stays:
- `net::wifiUp()`, `net::wifiStateName()`, `net::ipString()`,
  `net::rssi()`, `net::deviceId()`, `net::handleSerialCommand("wifi ...")`

### Risks

- Auto-reconnect timing differs from Arduino's defaults.
- `WiFi.scanNetworks(false, true)` async behavior maps to
  `esp_wifi_scan_start(NULL, false)` + `WIFI_EVENT_SCAN_DONE` — make
  sure the BLE-side `scan` removal from 00-findings doesn't regress.
- AP captive portal regression risk; need the custom DNS responder to
  be byte-for-byte compatible with iOS / Android captive portal probes
  (`/hotspot-detect.html`, `/generate_204`).

### Test plan

`make flash` → AP-mode QR provisioning → STA reconnect after Wi-Fi
flap → captive portal redirects iOS to setup page. Lane A live SK
connect.

### Estimate

L — 5-7 days. Confidence: medium. The captive portal is the chewy
part; the basic STA/AP swap is well-trodden.

---

## E. mDNS → IDF mdns component

### Scope

Replace `ESPmDNS.h` with the IDF `mdns` component. Sites:

- `MDNS.begin(s_device_id.c_str())` in `net.cpp`
- `MDNS.addService("arduino", "tcp", 3232)`,
  `MDNS.addService("espdisp", "tcp", 80)`
- `MDNS.queryService("espdisp-mgmt", "tcp")` +
  `MDNS.hostname(i)`/`MDNS.port(i)`/`MDNS.IP(i)` in `manager-discover`

### Design

Thin wrapper to keep call sites identical:

```cpp
namespace net::mdns {
bool begin(const char *hostname);
void add_service(const char *instance, const char *proto, uint16_t port);
struct Hit { std::string hostname; ip4_addr_t ip; uint16_t port; };
std::vector<Hit> query_service(const char *service, const char *proto,
                                uint32_t timeout_ms);
}  // namespace net::mdns
```

`query_ptr` + `query_srv` + `query_a` chain wrapped behind the single
`query_service` call.

### Risks

- IDF mDNS has occasional race issues during STA re-association.
  Already documented in `manager-discover` comment that iOS hotspot
  filters mDNS; behavior on real LAN stays.

### Test plan

`manager-discover` → 1+ hits when the plugin is up.
`avahi-browse -r _espdisp._tcp` (or `dns-sd -B`) from a peer sees the
device's two services.

### Estimate

S — 1-2 days. Confidence: high.

---

## F. HTTPClient → esp_http_client

### Scope

Replace 7 sites in `src/manager.cpp` (register, heartbeat, fetch_config,
poll_commands, ack_command, post_ota_progress, post_firmware_confirm)
and the OTA download loop. `http.begin/setConnectTimeout/setTimeout/
addHeader/GET/POST/getString/getSize/end` all need new homes.

### Design

```cpp
// include/net/http.h
namespace net::http {
struct Response {
    int    code;
    std::string body;        // empty for streamed bodies
    int    content_length;   // -1 for chunked
};

class Request {
 public:
    explicit Request(const char *url);
    void set_method(esp_http_client_method_t m);
    void set_header(const char *name, const char *value);
    void set_body(const std::string &body);  // POST/PUT
    void set_timeout_ms(int connect, int read);

    // Read-into-string. Refuses bodies larger than `cap` via a
    // header-callback check on Content-Length (drop-in for
    // resp_within_cap()).
    Response perform(int response_cap_bytes);

    // Streaming variant for OTA: each chunk handed to `sink` as it
    // arrives; sink returns false to abort.
    int perform_streaming(int response_cap_bytes,
                          std::function<bool(const uint8_t*, size_t)> sink);
};
}  // namespace net::http
```

The new module respects the four existing caps
(`MAX_DISCOVERY_BYTES`, `MAX_HEARTBEAT_RESP_BYTES`, `MAX_CONFIG_BYTES`,
`MAX_COMMANDS_BYTES`) via a header-event check.

### Risks

- TLS support is currently unused but free with `esp_http_client`. Keep
  the `embed_txtfiles` cert bundle slot ready for when SK moves to wss://
- Chunked encoding handling differs; verify the SK plugin doesn't send
  chunked.

### Test plan

Lane A: live SK register/heartbeat 200. Lane B: every Lane B test that
hits `firmware.update` / `screen.set` / etc. (all 20 in
`test_manager_commands.py`) since they trigger an ack POST.

### Estimate

M — 4 days. Confidence: high.

---

## G. OTA: Update.h → esp_ota_* + drop ArduinoOTA

### Scope

- Replace `Update.begin/write/end` in the manager OTA task with
  `esp_ota_*` calls direct (we already use `esp_ota_mark_app_valid_*`).
- Remove `ArduinoOTA` (port 3232 push-OTA). `make ota` deprecated; pull-
  OTA via the `firmware.update` v1 command is the only path.

### Design

Pull-OTA path (after Spec F lands):

```cpp
esp_ota_handle_t h;
const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
esp_ota_begin(next, OTA_SIZE_UNKNOWN, &h);
net::http::Request req(url);
req.set_header("Authorization", auth);
req.perform_streaming(MAX_OTA_BYTES, [&](const uint8_t *chunk, size_t n) {
    if (esp_ota_write(h, chunk, n) != ESP_OK) return false;
    mbedtls_sha256_update(&sha_ctx, chunk, n);
    return true;
});
esp_ota_end(h);
// sha verify, esp_ota_set_boot_partition, esp_restart
```

The existing policy gates (`s_ota_enabled` / `s_ota_max_size` /
`s_ota_require_sha`) stay in `execute_command`.

Drop `ArduinoOTA.handle()` from the main loop; remove port 3232
listener. Update `docs/specs/17 §10`.

### Risks

- Loss of `make ota` removes a path the dev workflow has used a lot
  this session. Document the `firmware.update` command-line equivalent
  via `curl` to the plugin.
- **Rollback safety (driver #2).** The `PENDING_VERIFY` →
  `mark_app_valid` sequence on first successful heartbeat only does
  anything if the **bootloader was built with**
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` — impossible on the precompiled
  4.4 core. On IDF5/hybrid, set it in `sdkconfig.defaults` so a never-
  confirmed image auto-reverts to the previous partition on the next boot.
  Interim 4.4 emulation: an **NVS confirm-flag** + boot-counter that
  forces last-known-good when the flag is missing (software stand-in for
  the bootloader feature).
- **OTA heap/cache stability (driver #1).** Pair this step with the
  IRAM-safe esp_lcd (§H) and the `app_pause_ui` pump-pause; the streaming
  `esp_ota_write` path here removes the `Update.write` + duplicate-buffer
  overhead that contributed to the mid-flash hang.

### Test plan

Cycle: bench → plugin `firmware.update` push → device downloads,
verifies, reboots → first heartbeat marks partition valid → `/devices`
shows new build_time.

### Estimate

S — 2 days. Confidence: high.

---

## H. Display: Arduino_GFX + custom ST7701 → esp_lcd_panel_rgb

> **This step is forced, not optional, on the IDF5 env.** `Arduino_GFX`'s
> `Arduino_RGB_Display` only exists in its `ESP_ARDUINO_VERSION_MAJOR < 3`
> branch. The pioarduino hybrid ships **Arduino-3.x**, so the RGB panel
> **must move fully onto esp_lcd** — the `-idf5` env will not *link*
> against `Arduino_RGB_Display` at all. (The `Arduino_SWSPI` bus used only
> for the **ST7701 boot init** *does* survive on 3.x, so the init-table
> handoff can stay on SWSPI initially and the RGB transfer alone moves to
> esp_lcd, or both move — see options below.) This is why `[env:...-idf5]`
> is marked `KNOWN-INCOMPLETE` today: it links everything but the panel.
>
> This step is also where drivers **#1 (flash-cache race)**, **#3
> (tear-free)** and **#5 (MIDL smoothness)** are actually delivered:
> `num_fbs = 2` + vsync flip + the IRAM-safe ISR (`sdkconfig`) +
> `bounce_buffer_size_px` together give double-buffering with no
> "Cache disabled but cached memory region accessed" panic.

### Scope

The biggest single rewrite. Replace `Arduino_SWSPI` + `Arduino_RGB_Display`
+ the embedded ST7701 init table in `board_pins.h` with:

1. `esp_lcd_panel_io_3wire_spi` for boot SPI commands (or keep
   `Arduino_SWSPI` for the init table — it survives on 3.x)
2. `esp_lcd_new_rgb_panel` for the RGB parallel transfer (**mandatory** on
   arduino-3.x — `Arduino_RGB_Display` is gone)
3. LVGL `lv_display_set_flush_cb` → `esp_lcd_panel_draw_bitmap`, **or**
   the `num_fbs=2` page-flip path for tear-free double-buffer

### Design

Maps onto **spec 13 step 3 (display extraction)**. Per-board
implementation under `board::display_begin()`:

```cpp
bool board::display_begin() {
    esp_lcd_panel_io_3wire_spi_config_t io_cfg = {
        .line_config = { .cs_io_type=...,.cs_gpio_num=ST7701_CS, ... },
        .expect_clk_speed = 1 * 1000 * 1000,
        // ...
    };
    esp_lcd_new_panel_io_3wire_spi(&io_cfg, &s_panel_io);
    apply_st7701_init_table(s_panel_io);   // verified bytes verbatim

    esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = ..., .h_res=480, .v_res=480,
            .hsync_pulse_width=..., .hsync_back_porch=..., /* etc */
        },
        .data_width = 16,
        .psram_trans_align = 64,
        .num_fbs = 2,           // double-buffer in PSRAM
        .bounce_buffer_size_px = 0,
        .hsync_gpio_num = RGB_HSYNC, .vsync_gpio_num = RGB_VSYNC,
        .de_gpio_num = RGB_DE,  .pclk_gpio_num = RGB_PCLK,
        .data_gpio_nums = { RGB_B0, RGB_B1, RGB_B2, RGB_B3, RGB_B4,
                            RGB_G0, RGB_G1, RGB_G2, RGB_G3, RGB_G4, RGB_G5,
                            RGB_R0, RGB_R1, RGB_R2, RGB_R3, RGB_R4 },
        .flags = { .fb_in_psram = 1, .double_fb = 1 },
    };
    esp_lcd_new_rgb_panel(&rgb_cfg, &s_panel);
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    return true;
}
```

The verified init table (`// clang-format off` section in
`board_pins.h`) ports byte-for-byte into `apply_st7701_init_table`. Do
**not** rederive timings from web sources — the verified ones go in
verbatim.

LVGL hookup:

```cpp
static void flush_cb(lv_display_t *d, const lv_area_t *area,
                     uint8_t *px) {
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px);
    lv_display_flush_ready(d);
}
```

Or in `LV_DISPLAY_RENDER_MODE_FULL` mode, swap framebuffers via
`esp_lcd_rgb_panel_get_frame_buffer` + `esp_lcd_rgb_panel_set_pep_bb_invalidate_cache`.

### Risks

- **The trickiest migration.** RGB timing is unforgiving — wrong porch
  values give jitter, color shifts, or no signal. Mitigation: preserve
  the exact timings in `board_pins.h` and copy them straight into the
  IDF struct.
- ST7701 init table — keep `clang-format off` wrap; copy each
  `WRITE_COMMAND_8` / `WRITE_BYTES` / `WRITE_C8_D16` sequence into the
  equivalent `esp_lcd_panel_io_tx_param` / `esp_lcd_panel_io_tx_color`
  call. One-for-one. Camera color test (white/red/green/blue gradients
  visible on screen with no banding) is the acceptance gate.
- LVGL flush cadence and dirty-rect handling — the existing 4.4 perf work
  (quantize-and-cache, dedicated LVGL task, fit-to-width; spec 09) must not
  regress, and the `DIRECT`-mode flicker seen on 4.4 single-buffer must be
  resolved by the `num_fbs=2` flip, not reintroduced.
- **IRAM-safe ISR + bounce buffers are mandatory, not optional.** Set
  `bounce_buffer_size_px` (non-zero) in `esp_lcd_rgb_panel_config_t` and
  rely on `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y` + `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y`
  (already in `sdkconfig.defaults`). Without them, the first sustained
  flash write *during live RGB scan-out* (e.g. an OTA, driver #1)
  re-triggers the cache panic. The acceptance gate for this step includes
  "an OTA completes while a dashboard is rendering, no panic, no hang."

### Test plan

USB flash, walk every screen, camera color test, frame-rate check
(target ≥ 50 Hz on dashboard), **and an OTA-while-rendering soak** (ties
the display port to driver #1's acceptance). Stay on
`framework = arduino, espidf` hybrid during this step so a bad rev can be
reverted with one commit.

### Estimate

L — 7 days. Confidence: low-medium. Allow extra contingency.

---

## I. Touch: GT911-Arduino → esp_lcd_touch_gt911

### Scope

Replace TAMCTec GT911 library with the IDF `esp_lcd_touch_gt911`
component. Remove the manual big-endian decode workaround from
`main.cpp` (`((uint16_t)hi << 8) | lo` reads).

### Design

Maps onto **spec 13 step 4 (touch into `input::` module)**. Per-board
`board::touch_begin()`:

```cpp
bool board::touch_begin() {
    esp_lcd_panel_io_i2c_config_t io_cfg =
        ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &s_touch_io);

    esp_lcd_touch_config_t cfg = {
        .x_max = 480, .y_max = 480,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = (gpio_num_t)TOUCH_INT,
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };
    esp_lcd_touch_new_i2c_gt911(s_touch_io, &cfg, &s_touch);
    return true;
}
```

`input::poll()` calls `esp_lcd_touch_read_data` +
`esp_lcd_touch_get_coordinates`. IRQ path (`touch.mode = "irq"`) keeps
its current shape — `esp_lcd_touch` doesn't manage the ISR itself, we
keep `gpio_isr_handler_add` from Spec B and notify the polling task.

### Risks

- The big-endian quirk for this panel — verified in tests: the official
  IDF driver implements the standard GT911 protocol and reads the
  registers in the documented order, so the workaround should not be
  needed. Confirm with a tap on (10,10) reading as (10,10) not
  (2560,2560).

### Test plan

`touch_grid` + `touch_cal` screens still calibrate. Lane B touch
injection paths (`tap`, `swipe`, `gesture`) work via BLE/serial console.

### Estimate

S — 2-3 days. Confidence: high.

---

## J. NimBLE-Arduino → esp_nimble (Apache NimBLE direct)

### Scope

Rewrite `src/ble_config.cpp` (~600 lines) to use Apache NimBLE's C API
directly. GAP advertising + GATT service definition + characteristic
read/write callbacks all move.

### Design

GATT service definition becomes the IDF-style struct array:

```cpp
static const struct ble_gatt_chr_def s_nus_chars[] = {
    { .uuid = (ble_uuid_t*)&NUS_TX_UUID,
      .access_cb = nus_tx_read_cb,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &s_nus_tx_handle },
    { .uuid = (ble_uuid_t*)&NUS_RX_UUID,
      .access_cb = nus_rx_write_cb,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
      .val_handle = &s_nus_rx_handle },
    { 0 },
};
static const struct ble_gatt_svc_def s_services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY,
      .uuid = (ble_uuid_t*)&NUS_SERVICE_UUID,
      .characteristics = s_nus_chars },
    /* CONNECTION + CONFIGURATION services ... */
    { 0 },
};
```

C function pointers replace C++ method-callback objects; per-service
`user_data` carries `this` for the state.

The on-disk protocol (UUIDs, characteristic shapes, 512-byte MTU
constraint, `truncated` JSON summary above 512) is unchanged.

Preserve the `setValue(uint8_t*, len)` trap fix from CLAUDE.md — the C
API takes `len` explicitly, so the trap doesn't exist any more.

### Risks

- Largest single rewrite by line count. Schedule late so the rest of
  the build has stabilized first.
- BLE pairing — currently `none-in-current-firmware` per `/api/security`.
  Don't introduce pairing in this migration; spec 20 §BLE is the place
  to add it if desired.

### Test plan

`make ble` console: `ip` + `sk-status` round-trip. Lane B BLE tests
(`test_input_injection.py` BLE path). CONFIGURATION characteristic
512-byte truncation summary still emits.

### Estimate

L — 5-6 days. Confidence: medium.

---

## K. WebSockets: links2004 → esp_websocket_client

### Scope

Replace `WebSocketsClient` in `src/signalk.cpp`. Event callback shape
(WS_CONNECTED, WS_DISCONNECTED, WS_TEXT, WS_ERROR) maps 1:1 to
`WEBSOCKET_EVENT_*`.

### Design

```cpp
esp_websocket_client_config_t cfg = {
    .uri = ws_url.c_str(),     // "ws://10.x.x.x:3000/signalk/v1/stream?token=..."
    .reconnect_timeout_ms = 5000,
    .network_timeout_ms = 10000,
    .buffer_size = 4096,
};
esp_websocket_client_handle_t s_ws = esp_websocket_client_init(&cfg);
esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY,
                              on_ws_event, NULL);
esp_websocket_client_start(s_ws);
// later:
esp_websocket_client_send_text(s_ws, payload, len, portMAX_DELAY);
```

429-retry/backoff logic stays. Subscription is one `send_text` after
WS_CONNECTED. Delta parsing in `signalk_parser.cpp` is unchanged.

### Risks

- TLS / wss:// readiness — `esp_websocket_client` supports it natively;
  add cert bundle slot for future SK https setups.
- The 429-recovery code path I added earlier this session
  (persistent connection + retry-after) needs to map onto the IDF
  event loop's reconnect timer.

### Test plan

Lane A: SK live, sees deltas, recovers from a forced restart of the SK
container.

### Estimate

M — 3-4 days. Confidence: high.

---

## L. Cutover: framework = espidf

### Scope

```ini
framework = espidf     ; was arduino, espidf
```

Audit and remove any remaining `#include <Arduino.h>`, Arduino lib_deps
(`Arduino_GFX`, `gt911-arduino`, `NimBLE-Arduino`, `WebSockets`).

### Risks

- Any sneaky `String` / `Preferences` use that escaped the audit will
  fail to link.

### Test plan

Full host suite + USB flash + walk every screen + Lane A + B.

### Estimate

S — 1 day. Confidence: high (a build failure here is mechanical and
the linker tells you exactly where).

---

## M. MIDL runtime-rendering interplay & sequencing

The migration and the **MIDL firmware-render** work
(`feat/midl-firmware-render`, runtime LVGL trees from a config doc — spec
`docs/superpowers/specs/2026-06-19-generic-dashboard-runtime-design.md`)
touch the **same two resources**: the internal-SRAM heap and the LVGL
render path. They must be co-planned.

### Why they collide

- MIDL replaces compile-time `screen_*.cpp` HUDs with **runtime-built
  widget trees** (`layout_renderer.cpp` walking an `Element`/split-grid
  tree). Building/tearing down trees on every config apply is a **burst of
  heap + LVGL-object churn** — exactly the peak the ~22 KB internal-SRAM
  ceiling (drivers #1/#4) is most fragile against. A config apply that
  coincides with an OTA or a BLE write is the worst case.
- MIDL dashboards are **dynamic** (screens rebuilt on apply, swapped on
  `nav` actions). Tearing on a swapped-in screen is far more visible than
  on a static HUD — so MIDL is the **strongest beneficiary of tear-free
  double-buffering** (driver #3), delivered only by §H on IDF5.
- MIDL's own constraints (spec §6) are the **same CLAUDE.md memory traps**
  this migration must preserve: `memset`-in-place parse, PSRAM-allocated
  live `Config`, no large scratch on task-callback stacks, 512-byte BLE
  cap → REST fetch for large configs. The migration must not regress any
  of them, and MIDL's larger runtime POD makes the PSRAM-allocation rule
  even less negotiable.

### Sequencing (do *with*, not *after*)

1. **MIDL schema/web-renderer/manager work (v1 items 1–8) proceeds in
   parallel** — it is host/web/manager-side and platform-agnostic; it does
   not wait on IDF5.
2. **The firmware MIDL port (MIDL spec item 9: port `layout_renderer` to
   the grammar on-device) should land on IDF5**, after §H, so the dynamic
   dashboards it produces get double-buffering and the heap headroom from
   `sdkconfig` trims (driver #4). Landing the firmware MIDL port on the
   4.4 single-buffer path would ship a tearing, heap-fragile dashboard —
   technically possible, but it spends the migration's whole payoff.
3. **§H's acceptance gate gains a MIDL clause:** a MIDL config apply that
   rebuilds a multi-element screen must not drop internal-SRAM low-water
   below the safety floor, and the swapped-in screen must flip tear-free.
4. **Don't block MIDL v1 on the link blocker (§A.1).** Keep MIDL firmware
   work on the bench-usable hybrid slice; the `ssl_client` blocker is
   about `WiFiClientSecure`, orthogonal to rendering.

**One-line ordering:** MIDL schema/web ∥ A.1 link fix → §H display
port (double-buffer + IRAM-safe) → firmware MIDL render port → BLE
re-enable (§J) → cutover (§L).

---

## Interim 4.4 mitigations shipped vs. what only IDF5 unlocks

| Pain / capability | Shipped on 4.4 (interim) | Only on IDF5 / hybrid |
|---|---|---|
| OTA mid-flash hang (#1) | `app_pause_ui` pauses LVGL during OTA; reduces heap+cache contention | IRAM-safe esp_lcd + bounce buffers (no cache panic) + `sdkconfig` heap trims; streaming `esp_ota_write` |
| OTA rollback (#2) | NVS confirm-flag + boot-counter (software emulation) | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + real `PENDING_VERIFY` auto-revert |
| Render speed (#3) | quantize-and-cache rotations, dedicated LVGL task, fit-to-width, value scaling (spec 09) | esp_lcd RGB `num_fbs=2` + vsync page-flip (tear-free double-buffer) |
| `DIRECT`-mode flicker (#3) | none — single-buffer DIRECT flickers, left disabled | resolved by the double-buffer flip |
| RGB cache panic (#3) | avoided by not running sustained flash writes during scan-out (fragile) | `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y` + `CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y` |
| Heap / BLE headroom (#4) | LVGL allocs already in PSRAM; manual buffer trimming | NimBLE/Wi-Fi/driver buffer sizing via `sdkconfig` |
| MIDL dynamic dashboards (#5) | runs on 4.4 single-buffer (tears, heap-fragile) | double-buffer + heap headroom make it smooth and safe |

The 4.4 column is the **current ceiling** — every entry there is a
workaround that buys time, not a fix. The IDF5 column is the actual
remediation, and it is gated almost entirely on §A.1 + §H.

---

## Summary

### Ordered by complexity (lowest → highest)

| Step | Title              | Complexity | Risk    | Confidence |
|------|--------------------|------------|---------|------------|
| L    | Cutover            | S          | Low     | High       |
| E    | mDNS               | S          | Low     | High       |
| G    | OTA (after F)      | S          | Low     | High       |
| I    | Touch              | S          | Low     | High       |
| B    | IDF mechanicals    | S-M        | Low     | High       |
| C    | Storage (NVS)      | M          | Low     | High       |
| K    | WebSockets         | M          | Low     | High       |
| F    | HTTP client        | M          | Medium  | High       |
| A    | Hybrid + IDF 5 (link blocker A.1) | L | High | Medium |
| D    | WiFi STA/AP        | L          | Medium  | Medium     |
| J    | NimBLE C API       | L          | Medium  | Medium     |
| M    | MIDL interplay (co-plan) | —    | —       | —          |
| H    | Display (RGB+LCD, double-buffer) | L | High | Low-Medium |

### Ordered by dev speed (fastest → slowest)

| Days  | Step | Title                |
|-------|------|----------------------|
| 1     | L    | Cutover              |
| 1-2   | E    | mDNS                 |
| 2     | C    | Storage (NVS)        |
| 2     | G    | OTA                  |
| 2-3   | I    | Touch                |
| 3     | B    | IDF mechanicals (incl. 36 fmt-string fixes) |
| 3-4   | K    | WebSockets           |
| 4     | F    | HTTP client          |
| 5-6   | J    | NimBLE C API (effective prereq for A) |
| 5-7   | A    | Hybrid + IDF 5 (revised; see spike findings) |
| 5-7   | D    | WiFi STA/AP          |
| 7     | H    | Display              |

**Total: 41-56 dev-days** (revised; was 38-50), i.e. **8-11 calendar
weeks at one dev's pace** with code review, integration, and the
inevitable "but on the bench..." debugging baked in.

### Recommended execution order (dependency-aware, post-link-stage)

The earlier spike concluded *A is gated on J* (NimBLE-Arduino 1.4 won't
build on IDF 5). That conclusion is **superseded**: the hybrid env boots
its first slice with **BLE disabled** (`-DYEYBOATS_DISABLE_BLE`), so A no
longer waits on the NimBLE rewrite. The real critical path is now the
**§A.1 `ssl_client` link blocker** then the **§H display port** (the only
thing keeping `-idf5` from a clean link), and BLE re-enables on
NimBLE-Arduino 2.x afterward. Revised order:

1. **B-mechanicals-only** (fmt-string casts + the
   `-Wmisleading-indentation` fix in `board_cli.cpp:30`). Lands on Arduino
   2.x + IDF 4.4 — low risk, decouples the compile from A.
2. **A.1** — resolve the `ssl_client` duplicate-library link blocker
   (preferably by dropping `WiFiClientSecure` and letting §F/§K bring
   their own mbedTLS). **Critical path.** Gets `-idf5` to a clean link.
3. **H** — esp_lcd RGB port with `num_fbs=2` + IRAM-safe ISR + bounce
   buffers. The other half of the critical path: it's what makes `-idf5`
   *boot a screen*, and it delivers drivers #1/#3/#5. Do it on the hybrid
   so a bad rev reverts in one commit.
4. **A (finish)** — env boots end-to-end: walk screens, host suite, Lane A.
   Add `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` here (driver #2).
5. **G** — OTA on `esp_ota_*` + real rollback; soak OTA-while-rendering
   (driver #1 acceptance). (After F for the streaming HTTP path.)
6. **C** — Storage (unblocks D).
7. **D** — WiFi (unblocks F, K).
8. **F** — HTTP (also removes the `WiFiClientSecure` dependency for good).
9. **K** — WebSockets (after D).
10. **E** — mDNS (independent quick win).
11. **I** — Touch (low risk, any time after A).
12. **J** — re-enable BLE on NimBLE-Arduino 2.x, then the C-API rewrite.
    No longer an A prerequisite; it's the feature-parity long-pole.
13. **MIDL firmware render port** — land on IDF5 after §H (see §M): gets
    double-buffer + heap headroom for the dynamic dashboards.
14. **L** — Cutover to `framework = espidf`.

### Recommended commit cadence

- A.1, B, C, E, G, I, L are single-commit changes.
- D, F, K want 2-3 commits (one per sub-area: STA / AP / scan; or
  GET / POST / streaming).
- H, J want 5-7 commits (per-board for H; per-service for J).
- §M is not a step — it is a co-planning constraint applied across H, G,
  and the firmware MIDL port.

Worst case (everything slowest, the `ssl_client` blocker needs a
pioarduino-side fix, RGB timings fight back) is ~14 weeks. Best case (A.1
is resolved by dropping `WiFiClientSecure`, the panel timings copy
verbatim into the esp_lcd struct) is ~7 weeks. The two genuine unknowns
are **§A.1 (link blocker)** and **§H (RGB + double-buffer)** — both now on
the critical path; everything else is well-trodden.
