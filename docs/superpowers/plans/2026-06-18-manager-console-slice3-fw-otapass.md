# Slice 3 — Firmware runtime OTA password Implementation Plan

> REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Firmware edits to `src/net.cpp` MUST honor CLAUDE.md "Memory traps" (no large stack structs on task callbacks; the firmware-memory-traps-check skill applies).

**Goal:** Make the ESP firmware's OTA password runtime-settable (NVS) + a serial command to set it, applied at OTA init, falling back to the compile-time `OTA_PASSWORD` when unset. The "which password applies" logic is a pure, host-tested function.

**Architecture:** A header-only pure selector `ota_pass_effective(nvs, compiled)` (host-tested under `[env:native]`); `src/net.cpp` reads/stores the password in NVS (`prefs`), exposes `ota-pass`/`ota-pass-clear` serial commands, and uses the selector in `otaSetup()`.

**Tech Stack:** C++17, Arduino/ESP-IDF, `pio test -e native` (Unity), `pio run -e esp32-4848s040`.

**Spec:** `docs/superpowers/specs/2026-06-18-manager-device-console-design.md` §3.

**Repo:** `/Users/borissorochkin/code/embedded/espdisp` (`main`).

---

## Task 1: Pure selector + host test

**Files:** Create `include/ota_pass.h`, `test/test_ota_pass/test_ota_pass.cpp`; Modify `platformio.ini` (`[env:native]` test_filter).

- [ ] **Step 1 — header** `include/ota_pass.h`:
```cpp
#pragma once
// Pure (no-Arduino) selector for the effective OTA password: a runtime value
// stored in NVS wins over the compile-time default; empty/null runtime falls
// back to the compiled default. Host-tested under [env:native].
inline const char *ota_pass_effective(const char *nvs, const char *compiled) {
    if (nvs && nvs[0]) return nvs;
    return compiled ? compiled : "";
}
```

- [ ] **Step 2 — failing test** `test/test_ota_pass/test_ota_pass.cpp`:
```cpp
#include <string.h>
#include <unity.h>
#include "ota_pass.h"
void setUp() {}
void tearDown() {}
static void test_nvs_wins() { TEST_ASSERT_EQUAL_STRING("rt", ota_pass_effective("rt", "compiled")); }
static void test_empty_nvs_falls_back() { TEST_ASSERT_EQUAL_STRING("compiled", ota_pass_effective("", "compiled")); }
static void test_null_nvs_falls_back() { TEST_ASSERT_EQUAL_STRING("compiled", ota_pass_effective(nullptr, "compiled")); }
static void test_both_empty() { TEST_ASSERT_EQUAL_STRING("", ota_pass_effective("", "")); }
int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_nvs_wins);
    RUN_TEST(test_empty_nvs_falls_back);
    RUN_TEST(test_null_nvs_falls_back);
    RUN_TEST(test_both_empty);
    return UNITY_END();
}
```
Add `    test_ota_pass` to the `[env:native]` `test_filter` list in platformio.ini.

- [ ] **Step 3 — red:** `pio test -e native -f test_ota_pass` → FAIL only if header missing; since the header exists it should PASS immediately (header-only). If it passes on first run that's fine — it's a pure header. (No production code depends on it yet.) Confirm 4 tests pass.

- [ ] **Step 4 — commit:** `git commit -am "test(ota): pure effective-OTA-password selector + host tests"`

---

## Task 2: NVS storage + serial command + otaSetup wiring

**Files:** Modify `src/net.cpp`.

- [ ] **Step 1 — read** `src/net.cpp`: `otaSetup()` (~line 402, the `ArduinoOTA.setPassword(OTA_PASSWORD)` line); the `prefs` Preferences wrapper usage (`prefs.get_string(key, def)` / `prefs.put_string(key, val)` — see device_id at ~747-751); `handleSerialCommand()` (the `line.startsWith("wifi ")` / `line == "wifi-forget"` command blocks) to mirror the command style; and `#include "ota_pass.h"` at the top.

- [ ] **Step 2 — otaSetup uses the selector.** Replace:
```cpp
    if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
```
with:
```cpp
    // Runtime OTA password (NVS) wins over the compile-time default.
    std::string ota_nvs = prefs.get_string("ota_pass", "");
    const char *ota_pw = ota_pass_effective(ota_nvs.c_str(), OTA_PASSWORD);
    if (strlen(ota_pw) > 0) ArduinoOTA.setPassword(ota_pw);
```
(Use whatever string type `prefs.get_string` returns — read its signature; the device_id call at ~747 shows it returns something `.c_str()`-able. Match it. Keep the temporary off the stack only if it were large — a password String is tiny, so a local is fine.)

- [ ] **Step 3 — serial commands.** In `handleSerialCommand()`, add command handlers near the other `wifi-*` blocks (mirror their `startsWith`/`substring`/`return true` style):
```cpp
    if (line.startsWith("ota-pass ")) {
        String pw = line.substring(9);
        pw.trim();
        prefs.put_string("ota_pass", pw.c_str());
        ArduinoOTA.setPassword(pw.length() ? pw.c_str() : OTA_PASSWORD);
        printf("[ota] password set (len %u); effective next OTA\n", (unsigned)pw.length());
        return true;
    }
    if (line == "ota-pass-clear") {
        prefs.put_string("ota_pass", "");
        printf("[ota] password cleared; reverts to compile-time default\n");
        return true;
    }
    if (line == "ota-pass") {
        std::string cur = prefs.get_string("ota_pass", "");
        bool set = !cur.empty() || strlen(OTA_PASSWORD) > 0;
        printf("[ota] password %s (runtime %s)\n", set ? "set" : "unset", cur.empty() ? "no" : "yes");
        return true;
    }
```
(Adjust `std::string` vs `String` to match `prefs.get_string`'s actual return type. `ArduinoOTA.setPassword` at runtime takes effect for subsequent OTA sessions.)

- [ ] **Step 4 — build:** `pio run -e esp32-4848s040` → SUCCESS. `pio test -e native` → all pass (incl. test_ota_pass). `make pre-commit` (CLANG_FORMAT=/opt/homebrew/opt/llvm/bin/clang-format) → clean.

- [ ] **Step 5 — commit:** `git commit -am "feat(ota): runtime OTA password in NVS + ota-pass serial commands"`

---

## Task 3: Device sanity (no-regression)

- [ ] OTA-flash the bench device with the new build (`make ota-verify REMOTE=compulab@mythra-nav DEVICE_IP=10.42.0.67`) — the default NVS ota_pass is empty so the device keeps OTA-flashing with no password (unchanged behavior); confirm it boots. (Full runtime-password OTA round-trip is exercised in Slice 7/8 once the manager pushes a password.) If the bench is busy, skip and note — the build + native tests gate the code.

---

## Self-Review
- **Spec §3 coverage:** runtime OTA password in NVS (Task 2), serial setter (`ota-pass`/`ota-pass-clear`/`ota-pass`), applied in otaSetup via the pure selector, compile-time fallback. ✓
- **Memory traps:** no large stack structs; password Strings are tiny; selector is pure header. ✓
- **Host-tested:** the selection logic (`ota_pass_effective`) — nvs-wins / empty-fallback / null / both-empty. ✓
- **No placeholders:** full code for header, test, net.cpp edits; exact commands.
- **Consistency:** `ota_pass_effective(nvs, compiled)` signature used in header, test, and net.cpp; NVS key `"ota_pass"` consistent across set/clear/get/otaSetup.
