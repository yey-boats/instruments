# Slice 5 — ESP Web Tools embed + manifest + WebSerial provisioning Implementation Plan

> REQUIRED SUB-SKILL: superpowers:subagent-driven-development. NOTE: the manifest + binary serving are node-testable here; the actual USB flash + WebSerial provisioning require a device cabled to a browser machine (secure context) — implemented + deployed here, **on-device flash verified by the operator**.

**Goal:** In the manager, serve an ESP Web Tools manifest + the factory binary same-origin, and add a "Flash new device (USB)" UI that embeds esp-web-tools (esptool-js/WebSerial), gated to secure contexts, then runs a custom WebSerial provisioning console that sends the firmware's existing serial commands (WiFi/id/ota-pass) sourced from System Settings.

**Architecture:** Manager routes `GET /firmware/manifest/:artifactId` (ESP Web Tools JSON) + `GET /firmware/artifacts/:artifactId/binary` (streams the artifact `file.path`). A `public/flash.html` (or a section) loads esp-web-tools via a `<script type="module">` and an `<esp-web-install-button>`; a `public/provision.js` does post-flash WebSerial provisioning using System-Settings creds fetched from the manager.

**Tech Stack:** Node manager, esp-web-tools web component, Web Serial API, `node test/run.js`.

**Spec:** `docs/superpowers/specs/2026-06-18-manager-device-console-design.md` §4.

**Repo:** `/Users/borissorochkin/code/embedded/signalk-espdisp-manager` (`main`).

---

## File Structure
- **Modify** `index.js`: `firmwareManifest(artifactId)` route + binary-serving route; a `/ui/flash` page route; add a "Flash new device" link/section on the merged Devices page.
- **Modify** `lib/manager.js`: `firmwareManifest(artifactId)` builder + an artifact-by-id binary resolver (reuse `firmwareDownloadInfo`-style logic by artifactId).
- **Create** `public/flash.html` — the esp-web-tools page (artifact picker + install button + secure-context gate + provisioning panel).
- **Create** `public/provision.js` — post-flash WebSerial provisioning console.
- **Create** `public/vendor/install-button.js` — vendored esp-web-tools module (or a CDN `<script type=module src=...>`; vendoring is more robust offline — see Task 2).
- **Create** `test/manifest.test.js`; register in `test/run.js`.

---

## Task 1: Manifest + binary serving (node-verifiable)

**Files:** Modify `lib/manager.js`, `index.js`; Create `test/manifest.test.js`; Modify `test/run.js`.

- [ ] **Step 1 — read** `lib/manager.js` `firmwareDownloadInfo(jobId)` + `listFirmware()` + the `store.firmware.artifacts[]` shape (each artifact: `artifactId`, `version`, `file: { path, name, sha256, size, contentType }`). And `index.js` `/firmware/download/:jobId` route (the streaming pattern).

- [ ] **Step 2 — failing test** `test/manifest.test.js`:
```js
const assert = require('assert')
const { makeManager } = require('./test-utils')
const { manager } = makeManager({ auth: { mode: 'dev-shared-token', devToken: 't' }, network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' } })

// seed a firmware artifact directly in the store
manager.getSettings && manager.getSettings() // ensure store ready
manager.__test && null
const store = manager.store
store.firmware.artifacts.push({ artifactId: 'fw-1', version: '0.5.0', file: { path: '/tmp/firmware-factory.bin', name: 'firmware-factory.bin', sha256: 'abc', size: 2202336, contentType: 'application/octet-stream' } })

const man = manager.firmwareManifest('fw-1')
assert.equal(man.name, 'espdisp')
assert.equal(man.version, '0.5.0')
assert.ok(Array.isArray(man.builds), 'manifest has builds[]')
assert.equal(man.builds[0].chipFamily, 'ESP32-S3')
assert.equal(man.builds[0].parts[0].offset, 0)
assert.ok(/\/firmware\/artifacts\/fw-1\/binary$/.test(man.builds[0].parts[0].path), 'part path points at the binary route')
assert.equal(manager.firmwareManifest('nope'), null, 'unknown artifact -> null')
console.log('manifest.test: OK')
```
(If `manager.store` is not accessible, seed via whatever the firmware-contract tests use to add an artifact — read an existing firmware test. Adjust.)
Register in `test/run.js`. Run `node test/manifest.test.js` → FAIL.

- [ ] **Step 3 — implement manager** `firmwareManifest(artifactId)` in `lib/manager.js`:
```js
firmwareManifest (artifactId) {
  const a = (this.store.firmware.artifacts || []).find((x) => x.artifactId === artifactId)
  if (!a) return null
  return {
    name: 'espdisp',
    version: a.version || '0',
    new_install_prompt_erase: true,
    builds: [{
      chipFamily: 'ESP32-S3',
      parts: [{ path: `/plugins/espdisp-manager/firmware/artifacts/${encodeURIComponent(artifactId)}/binary`, offset: 0 }]
    }]
  }
}
firmwareArtifactFile (artifactId) {
  const a = (this.store.firmware.artifacts || []).find((x) => x.artifactId === artifactId)
  return a && a.file && a.file.path ? a.file : null
}
```

- [ ] **Step 4 — implement routes** in `index.js` (near the other `/firmware/*` routes):
```js
router.get('/firmware/manifest/:artifactId', wrap(getManager, (manager, req, res) => {
  const man = manager.firmwareManifest(req.params.artifactId)
  if (!man) { res.statusCode = 404; res.json({ error: { code: 'artifact_not_found' } }); return }
  res.setHeader('content-type', 'application/json')
  res.end(JSON.stringify(man))
}))
router.get('/firmware/artifacts/:artifactId/binary', wrap(getManager, (manager, req, res) => {
  const file = manager.firmwareArtifactFile(req.params.artifactId)
  if (!file) { res.statusCode = 404; res.json({ error: { code: 'artifact_binary_missing' } }); return }
  res.setHeader('content-type', file.contentType || 'application/octet-stream')
  if (file.size) res.setHeader('content-length', String(file.size))
  res.setHeader('content-disposition', `attachment; filename="${path.basename(file.name || file.path)}"`)
  fs.createReadStream(file.path).on('error', (err) => {
    if (!res.headersSent) { res.statusCode = 404; res.json({ error: { code: 'artifact_binary_missing', message: err.message } }) } else { res.destroy(err) }
  }).pipe(res)
}))
```

- [ ] **Step 5 — green:** `node test/manifest.test.js` → OK. `node test/run.js` → no new failures.

- [ ] **Step 6 — commit:** `git commit -am "feat(flash): ESP Web Tools manifest + same-origin firmware binary serving"`

---

## Task 2: Flash-new-device UI (esp-web-tools embed + secure-context gate + WebSerial provisioning)

**Files:** Create `public/flash.html`, `public/provision.js`, `public/vendor/install-button.js`; Modify `index.js` (`/ui/flash` route + a link on the Devices page).

**NOTE:** flash + provisioning are operator-verified on hardware. Build it deployable + correct; do not block on a device.

- [ ] **Step 1 — vendor esp-web-tools (PREFERRED — avoids CDN-compromise risk).** Download the esp-web-tools install-button module into `public/vendor/install-button.js`: `curl -sL https://unpkg.com/esp-web-tools@10/dist/web/install-button.js -o public/vendor/install-button.js` (pin the version). Vendoring is the default — it works offline on the lab AND removes the supply-chain exposure of loading a remote script. **Only if vendoring is impossible**, fall back to a CDN `<script type="module">` and you MUST add Subresource Integrity: `integrity="sha384-<hash>" crossorigin="anonymous"` (compute the hash from the fetched file: `openssl dgst -sha384 -binary install-button.js | openssl base64 -A`). Report which path you used; prefer vendoring.

- [ ] **Step 2 — `public/flash.html`:** a standalone page (served at `/signalk-espdisp-manager/flash.html`) that:
  - Loads the esp-web-tools module (`<script type="module" src="vendor/install-button.js"></script>` or CDN).
  - Has an artifact `<select id="artifact">` (populated by fetching `/plugins/espdisp-manager/firmware/catalog` → list artifacts; each option value = artifactId, label = version).
  - An `<esp-web-install-button id="btn">` whose `manifest` attribute is set from JS to `/plugins/espdisp-manager/firmware/manifest/<selected-artifactId>` when the artifact changes.
  - **Secure-context gate:** on load, if `!('serial' in navigator)`, hide the button and show a notice: "USB flashing needs a secure context — open this page via http://localhost (e.g. the SSH tunnel) or HTTPS, on the computer the device is plugged into."
  - A **"Provision after flash"** panel (provision.js) — see Step 3.
  Keep styling minimal/consistent (you can inline a small style block).

- [ ] **Step 3 — `public/provision.js`:** after a successful flash (esp-web-tools fires events / or a manual "Provision over serial" button), open a WebSerial port (`navigator.serial.requestPort()` → `open({ baudRate: 115200 })`), and send the firmware's text commands sourced from System Settings:
  - Fetch creds: `GET /plugins/espdisp-manager/ui/settings` is HTML; instead add/By a small JSON endpoint — fetch `/plugins/espdisp-manager/settings.json` (add this route in index.js returning `manager.getSettings()` BUT WITHOUT secrets is wrong here — provisioning NEEDS the real WiFi password. SECURITY: this endpoint must require the manager/SignalK session (it's already behind SignalK auth) and return the real creds for provisioning. Add `router.get('/provisioning/payload', wrap(getManager, (m,req,res)=>res.json({ wifi: m.getSettings().network, ota: m.getSettings().ota.password, number: m.allocateDeviceNumber() })))` — returns real creds, auth-gated.).
  - Over the serial writer, send (with small delays, `\n`-terminated): `wifi "<ssid>" <pass>` (reboots), then after reconnect `id <number>`, `ota-pass <otapw>`. Mirror the command order/timing from `tools/provision_device.py` (wifi triggers reboot → wait). Show a log of sent commands + any serial output read back.
  - Robust to the reboot: after `wifi`, the port drops; provisioning of id/ota-pass can be deferred to config-push (Slice 7) once the device is networked — so for Slice 5, sending the WiFi creds (network bootstrap) over serial is the MUST; id/ota-pass MAY be sent here or left to Slice 7's config-push. Implement WiFi-bootstrap-over-serial now; note id/ota-pass go via config-push (Slice 7).

- [ ] **Step 4 — `/ui/flash` route + Devices link.** Add `router.get('/ui/flash', ...)` rendering the shell with an iframe/embed of flash.html, OR serve flash.html directly as a static asset and add a "Flash new device (USB)" link on the merged Devices page pointing at `/signalk-espdisp-manager/flash.html`. Add the `/provisioning/payload` route (Step 3, auth-gated). Add `module.exports.__flashRoutesPresent = true` is not needed; just wire the routes.

- [ ] **Step 5 — verify (node + load):** `node -e "require('./index.js'); console.log('ok')"`; `node test/run.js` no new failures; `curl`-equivalent: confirm `/signalk-espdisp-manager/flash.html` is served (it's a public asset) and `/plugins/espdisp-manager/firmware/manifest/<id>` returns JSON for a seeded artifact (covered by the test). The esp-web-tools flash + WebSerial provisioning are operator-verified on hardware.

- [ ] **Step 6 — commit:** `git commit -am "feat(flash): Flash-new-device UI (esp-web-tools embed, secure-context gate, WebSerial provisioning)"`

---

## Task 3: Deploy + operator handoff

- [ ] Push; rsync to mythra-nav; restart signalk-server; confirm `/signalk-espdisp-manager/flash.html` loads and the manifest endpoint returns JSON. Provide the operator steps: open `http://localhost:3000/signalk-espdisp-manager/flash.html` (via the SSH tunnel, secure context) on the computer with the ESP cabled, pick an artifact, click Install, then Provision. Note the artifact must exist (factory.bin imported/uploaded — a separate CI/upload step).

---

## Self-Review
- **Spec §4 coverage:** manifest endpoint (Task 1), same-origin binary serving (Task 1), esp-web-tools embed (Task 2), secure-context gate (Task 2), server-sourced WiFi creds over serial (Task 2/provision.js), `/provisioning/payload` auth-gated (Task 2). ✓
- **Server-sourced provisioning:** creds come from System Settings via `/provisioning/payload` (auth-gated), the client only relays the WiFi bootstrap over the cable; id/ota-pass deferred to Slice 7 config-push. ✓
- **Hardware gap:** flash + WebSerial provisioning operator-verified; manifest + binary + page-load node/curl-verified.
- **Dependency noted:** a real factory.bin artifact must exist in the catalog (CI publish / upload) for an end-to-end flash.
