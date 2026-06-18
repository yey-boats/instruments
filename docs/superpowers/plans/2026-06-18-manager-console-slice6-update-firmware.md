# Slice 6 — Per-device Update Firmware (serial/OTA + validation) Implementation Plan

> REQUIRED SUB-SKILL: superpowers:subagent-driven-development.

**Goal:** On the device page, an "Update Firmware" control that picks a firmware artifact, picks a method (Serial via ESP Web Tools / OTA), shows a **connection-validation checklist** (blocking flash until it passes), and triggers the flash — OTA via the existing pull-OTA firmware-job, Serial by handing off to the flash page with device context.

**Architecture:** Reuse the EXISTING OTA mechanism (`createFirmwareJob` → device pulls `/firmware/download/:jobId` and self-flashes; the device page already has a firmware-update form). Add a host-tested `validateOtaTarget(deviceId)` (online / address-known / artifact-available) and rework the device-page firmware control into a method+validation UI. Serial = link to `flash.html` (Slice 5).

**Tech Stack:** Node manager, `node test/run.js`.

**Spec:** `docs/superpowers/specs/2026-06-18-manager-device-console-design.md` §5.

**Repo:** `/Users/borissorochkin/code/embedded/signalk-espdisp-manager` (`main`). OTA path is verifiable against the bench device IF the catalog has an artifact; the validation logic is node-tested.

---

## File Structure
- **Modify** `lib/manager.js`: `validateOtaTarget(deviceId)`.
- **Modify** `index.js`: rework the device-page firmware-update form (~line 2722) into a method selector + validation checklist; the OTA submit reuses the existing `/ui/devices/:id/firmware/update` route (createFirmwareJob); Serial = a link to flash.html.
- **Create** `test/ota-validate.test.js`; register in `test/run.js`.

---

## Task 1: Connection-validation logic (host-tested)

**Files:** Modify `lib/manager.js`; Create `test/ota-validate.test.js`; Modify `test/run.js`.

- [ ] **Step 1 — read** `lib/manager.js`: how a device's online/health + address are derived (the dashboard/describeDevice logic — `device.lastSeen`, `device.status`/health, `device.address`/`network`/wifi IP); `listFirmware()` (artifacts available); `createFirmwareJob`.

- [ ] **Step 2 — failing test** `test/ota-validate.test.js`:
```js
const assert = require('assert')
const { makeManager } = require('./test-utils')
const { MockFirmware } = require('./mock-firmware')
const { manager, auth } = makeManager({ auth: { mode: 'dev-shared-token', devToken: 't' }, network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' } })

// unknown device -> not ready
const v0 = manager.validateOtaTarget('nope')
assert.equal(v0.exists, false)
assert.equal(v0.ready, false)

const dev = new MockFirmware(manager, { deviceId: 'espdisp-ota-1', auth, display: { width: 480, height: 480, shape: 'square', rotation: 0, colorDepth: 16 } })
dev.register()
// fresh registration: exists, but no artifact in the catalog yet -> not ready (hasArtifact false)
const v1 = manager.validateOtaTarget('espdisp-ota-1')
assert.equal(v1.exists, true)
assert.equal(typeof v1.online, 'boolean')
assert.equal(v1.hasArtifact, false)
assert.equal(v1.ready, false)

// add an artifact -> hasArtifact true
manager.store.firmware.artifacts.push({ artifactId: 'fw-1', version: '0.5.0', file: { path: '/tmp/x.bin', name: 'x.bin' } })
const v2 = manager.validateOtaTarget('espdisp-ota-1')
assert.equal(v2.hasArtifact, true)
// ready iff exists && online && hasArtifact && addressKnown
assert.equal(v2.ready, v2.exists && v2.online && v2.hasArtifact && v2.addressKnown)
console.log('ota-validate.test: OK')
```
Register in test/run.js. Run `node test/ota-validate.test.js` → FAIL.

- [ ] **Step 3 — implement** `validateOtaTarget(deviceId)` in `lib/manager.js`:
```js
validateOtaTarget (deviceId) {
  const d = this.store.registry.devices[deviceId]
  if (!d) return { exists: false, online: false, addressKnown: false, hasArtifact: false, ready: false }
  // online: derive the same way the dashboard/health does. Read the existing
  // online/health computation and reuse it (e.g. lastSeen within the heartbeat
  // window). Use the same helper the device table uses; do NOT invent a new rule.
  const online = this.isDeviceOnline ? this.isDeviceOnline(d) : Boolean(d.online)
  const addressKnown = Boolean((d.network && (d.network.host || d.network.ip)) || d.address || (d.status && d.status.wifi && d.status.wifi.ip))
  const hasArtifact = (this.store.firmware.artifacts || []).length > 0
  return { exists: true, online, addressKnown, hasArtifact, ready: online && addressKnown && hasArtifact }
}
```
   - READ how online/health is actually computed for the device table (there IS an existing computation — the table shows "ok"/"offline"). Reuse that exact predicate (extract a helper if needed) so validation matches the table. Match the real `address`/wifi-IP field names from describeDevice.

- [ ] **Step 4 — green:** `node test/ota-validate.test.js` → OK. `node test/run.js` → no new failures. Commit: `git commit -am "feat(ota): validateOtaTarget connection pre-flight (online/address/artifact)"`

---

## Task 2: Update Firmware UI (method + validation)

**Files:** Modify `index.js` (the device-detail firmware control ~line 2722).

- [ ] **Step 1 — read** the device-detail page render (where the existing `<form action=".../firmware/update">` is, ~2722) + how the artifact options are rendered there.

- [ ] **Step 2 — rework the control** into an "Update Firmware" section with:
  - The validation checklist from `manager.validateOtaTarget(device.deviceId)`: render each of `online`, `addressKnown`, `hasArtifact` as a green ✓ / red ✗ row (reuse the `.ok`/`.bad` status pill styles).
  - A **method** radio/select: `OTA` (default) and `Serial (USB)`.
  - An **artifact** `<select name="artifactId">` (from the catalog).
  - For **OTA**: keep the existing `<form method="post" action="/plugins/espdisp-manager/ui/devices/:id/firmware/update">` (it calls createFirmwareJob). DISABLE the submit button when `!validateOtaTarget(...).ready` (e.g. `${ready ? '' : 'disabled'}` + a note "device offline / no artifact — resolve before OTA"). The device pulls + self-flashes via the existing job mechanism.
  - For **Serial**: a link/button "Flash via USB →" → `/signalk-espdisp-manager/flash.html` (opens the ESP Web Tools page; target=_blank). A note: "plug the device into this computer; flashing runs in your browser."
  - Small client JS to toggle the OTA form vs the Serial link based on the selected method (progressive enhancement; default OTA visible).

- [ ] **Step 3 — verify:** `node -e "require('./index.js'); console.log('ok')"`; `node test/run.js` no new failures. (The OTA job itself + the device pull are exercised against hardware/the bench in Task 3.)

- [ ] **Step 4 — commit:** `git commit -am "feat(ota): device Update-Firmware UI with method (serial/OTA) + connection validation"`

---

## Task 3: Deploy + OTA path check

- [ ] Push; rsync to mythra-nav; restart signalk-server.
- [ ] Verify the device page shows the Update Firmware section with the validation checklist + method selector. IF the catalog has a real firmware artifact, optionally trigger an OTA job for the bench device (espdisp-28372f8a0290) and watch the firmware-job progress (device pulls + flashes) — but do NOT disrupt the device if it's in use; this is optional. The validation UI + method selection are the deliverable; the OTA job reuses the proven existing mechanism.

---

## Self-Review
- **Spec §5 coverage:** pick firmware (artifact select), method (serial/OTA), connection validation (checklist + disabled-until-ready), flash (OTA = existing job; serial = flash.html). ✓
- **Reuse:** OTA via the EXISTING createFirmwareJob/pull mechanism — no new runner. ✓
- **Host-tested:** `validateOtaTarget` (exists/online/address/artifact/ready). ✓
- **Consistency:** `validateOtaTarget` shape `{exists,online,addressKnown,hasArtifact,ready}` used in method + UI; online predicate reuses the existing device-table computation.
