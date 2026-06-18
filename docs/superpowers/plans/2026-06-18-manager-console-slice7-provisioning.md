# Slice 7 — Provisioning config-push + auto-numbering Implementation Plan

> REQUIRED SUB-SKILL: superpowers:subagent-driven-development.

**Goal:** When a freshly-flashed device registers, the manager auto-assigns the next device number (from System Settings numbering) and includes the System-Settings OTA password in the device's generated config, so config-push delivers number + OTA password server-side. The firmware applies the pushed OTA password to NVS (reusing Slice 3).

**Architecture:** Manager `registerDevice` calls `allocateDeviceNumber()` for a new, unnamed device and stores it as the device `name`/`assignedNumber`; `generateConfig` includes `ota.password` from System Settings. The firmware's existing config-apply (`src/manager.cpp` — already applies `id` + OTA metadata) reads `ota.password` from the pushed config and stores it via the Slice-3 NVS mechanism (`ota_pass`).

**Tech Stack:** Node manager (`node test/run.js`) + C++ firmware (`pio test -e native`, `pio run`). Manager parts host-verified; the device applying the pushed config is operator/bench-verified.

**Spec:** `docs/superpowers/specs/2026-06-18-manager-device-console-design.md` §6.

**Repos:** manager `/Users/borissorochkin/code/embedded/signalk-espdisp-manager`; firmware `/Users/borissorochkin/code/embedded/espdisp`.

---

## Task 1: Auto-numbering at registration (manager, host-tested)

**Files:** Modify `lib/manager.js` (`registerDevice`, and the Slice-2 `allocateDeviceNumber` guard); Create `test/provision-number.test.js`; Modify `test/run.js`.

- [ ] **Step 1 — read** `registerDevice`: the `merged` object (`name: existing.name || incoming.name || id`), `created` flag, `this.store.saveRegistry()`. And `allocateDeviceNumber()` (Slice 2).

- [ ] **Step 2 — failing test** `test/provision-number.test.js`:
```js
const assert = require('assert')
const { makeManager } = require('./test-utils')
const { MockFirmware } = require('./mock-firmware')
const { manager, auth } = makeManager({ auth: { mode: 'dev-shared-token', devToken: 't' }, network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' } })
manager.updateSettings({ numbering: { prefix: 'boat-', pad: 2, next: 7 } })
// a new device that reports no name gets the next number assigned
const dev = new MockFirmware(manager, { deviceId: 'espdisp-aaaaaaaaaaaa', auth, name: '', display: { width: 480, height: 480, shape: 'square', rotation: 0, colorDepth: 16 } })
const reg = dev.register()
const rec = manager.store.registry.devices['espdisp-aaaaaaaaaaaa']
assert.equal(rec.assignedNumber, 'boat-07', 'new device got the next number')
assert.equal(rec.name, 'boat-07', 'name set to the assigned number')
assert.equal(manager.getSettings().numbering.next, 8, 'counter advanced once')
// re-register (same device) does NOT reassign
dev.register()
assert.equal(manager.store.registry.devices['espdisp-aaaaaaaaaaaa'].assignedNumber, 'boat-07', 're-register keeps number')
assert.equal(manager.getSettings().numbering.next, 8, 'counter not advanced on re-register')
console.log('provision-number.test: OK')
```
Register in test/run.js. Run → FAIL.

- [ ] **Step 3 — implement.** First, in `allocateDeviceNumber()` add the Slice-2 minor guard: `const n = this.store.settings.numbering || (this.store.settings.numbering = require... )` — simplest: `if (!this.store.settings.numbering) this.store.settings.numbering = { prefix: 'espdisp-', next: 1, pad: 3 }` at the top. Then in `registerDevice`, AFTER `const merged = {...}` and the `created` computation, before saveRegistry:
```js
    if (created && !existing.assignedNumber && (!incoming.name || incoming.name === id)) {
      merged.assignedNumber = this.allocateDeviceNumber()
      merged.name = merged.assignedNumber
    } else if (existing.assignedNumber) {
      merged.assignedNumber = existing.assignedNumber
    }
```
   - Place this so it only fires on first creation of a device that reported no meaningful name (name empty or equal to its id). Preserve an operator-set name if present. Read the exact `merged`/`incoming` variable names and adapt.

- [ ] **Step 4 — green:** `node test/provision-number.test.js` → OK. `node test/run.js` → no new failures. Commit: `git commit -am "feat(provision): auto-assign device number at first registration"`

---

## Task 2: OTA password in generated config (manager, host-tested)

**Files:** Modify `lib/manager.js` (`generateConfig`); Create `test/provision-config.test.js`; Modify `test/run.js`.

- [ ] **Step 1 — read** `generateConfig(id)` (~line 1366): what it returns (the device config object + hash). Find where to inject an `ota` / provisioning section.

- [ ] **Step 2 — failing test** `test/provision-config.test.js`:
```js
const assert = require('assert')
const { makeManager } = require('./test-utils')
const { MockFirmware } = require('./mock-firmware')
const { manager, auth } = makeManager({ auth: { mode: 'dev-shared-token', devToken: 't' }, network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' } })
manager.updateSettings({ ota: { password: 'flashpw' } })
const dev = new MockFirmware(manager, { deviceId: 'espdisp-bbbbbbbbbbbb', auth, display: { width: 480, height: 480, shape: 'square', rotation: 0, colorDepth: 16 } })
dev.register()
const cfg = manager.generateConfig('espdisp-bbbbbbbbbbbb')
// the generated device config carries the OTA password so config-push delivers it
assert.ok(cfg.config, 'has config')
assert.equal(cfg.config.ota && cfg.config.ota.password, 'flashpw', 'config carries the OTA password from system settings')
// no OTA password set -> field absent/empty (do not push an empty password over the existing one)
manager.updateSettings({ ota: { password: '' } }) // empty keeps existing 'flashpw'; force-clear via a direct store edit:
manager.store.settings.ota.password = ''
const cfg2 = manager.generateConfig('espdisp-bbbbbbbbbbbb')
assert.ok(!cfg2.config.ota || !cfg2.config.ota.password, 'no password -> not pushed')
console.log('provision-config.test: OK')
```
Register in test/run.js. Run → FAIL.

- [ ] **Step 3 — implement** in `generateConfig(id)`: after the config object is assembled (before hashing), add:
```js
    const ota = this.store.settings && this.store.settings.ota
    if (ota && ota.password) {
      config.ota = Object.assign({}, config.ota, { password: ota.password })
    }
```
   (Use the actual local variable name for the config object in generateConfig. The hash is computed AFTER, so the OTA password participates in the config hash — which is fine: changing the OTA password is a config change the device should fetch. Match the code.)

- [ ] **Step 4 — green:** `node test/provision-config.test.js` → OK. `node test/run.js` → no new failures. Commit: `git commit -am "feat(provision): include system-settings OTA password in generated device config"`

---

## Task 3: Firmware applies pushed OTA password (firmware; build-tested)

**Files:** Modify `src/manager.cpp` (the config-apply path). Honor CLAUDE.md memory traps (manager.cpp is heap-sensitive — small locals only).

- [ ] **Step 1 — read** `src/manager.cpp` config-apply (~line 1057 "OTA metadata. Lock the policy via the config", and where it parses the fetched config JSON + applies `id`). Find where it reads the config JSON (ArduinoJson doc) and applies fields.

- [ ] **Step 2 — implement.** In the config-apply path, after parsing the config JSON, if it has an `ota.password` string, store it via the Slice-3 NVS mechanism and apply:
```cpp
    // Manager-pushed OTA password (server-sourced provisioning). Persist to NVS
    // (same key as the `ota-pass` console command) so OTA uses it after reboot.
    const char *ota_pw = doc["ota"]["password"] | "";
    if (ota_pw && ota_pw[0]) {
        storage::Namespace prefs("net", false);
        prefs.put_string("ota_pass", ota_pw);
        ArduinoOTA.setPassword(ota_pw);
    }
```
   - Adapt `doc`/JSON accessor to the actual ArduinoJson variable + the config-apply function's structure. Use the SAME NVS namespace/key (`"net"`/`"ota_pass"`) as Slice 3. Include `<ArduinoOTA.h>`/the OTA header if not already in scope (otaSetup is in net.cpp — `ArduinoOTA` may not be directly includable in manager.cpp; if not, route through a `net::set_ota_password(const char*)` helper added to net.cpp/net.h that does the NVS put + setPassword. Prefer a `net::` helper to avoid coupling manager.cpp to ArduinoOTA).
   - PREFERRED: add `void net::set_ota_password(const char *pw)` in net.cpp (does the NVS put_string + ArduinoOTA.setPassword) + declare in net.h; call it from manager.cpp. Cleaner separation.

- [ ] **Step 3 — build:** `pio run -e esp32-4848s040` → SUCCESS. `pio test -e native` → all pass. `make pre-commit` (CLANG_FORMAT) → clean.

- [ ] **Step 4 — commit:** `git commit -am "feat(provision): firmware applies manager-pushed OTA password from config"`

---

## Task 4: Deploy + operator handoff

- [ ] Deploy the manager (push, rsync, restart). The firmware change ships on the next device flash/OTA.
- [ ] Operator end-to-end (Slice 8): flash a fresh device via ESP Web Tools (USB), the WebSerial step bootstraps WiFi from System Settings, the device joins + registers, the manager auto-numbers it + config-pushes the OTA password, the device applies it → appears in the registry, controllable, OTA-ready. Document these steps for the operator.

---

## Self-Review
- **Spec §6 coverage:** auto-number at registration (Task 1), OTA password via config-push (Task 2), firmware applies it (Task 3), idempotent (re-register keeps number; empty password not pushed). ✓
- **Slice-2 guard fix** folded into Task 1 (allocateDeviceNumber numbering default). ✓
- **Host-tested:** numbering assignment + idempotence; config carries OTA password + empty-not-pushed. Firmware build-tested; device application operator-verified.
- **Secrets:** the OTA password rides the device's authenticated config-fetch (device-token-gated), server-sourced — consistent with the design.
- **Consistency:** NVS key `ota_pass`/namespace `net` shared with Slice 3; `assignedNumber` field on the device record.
