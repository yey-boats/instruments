# Manager Device Console — Slice 1 (UI restructure) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Merge the manager's Overview + Devices into one landing page, drop `Presets`/`Layout editor` from the nav, and stop manager actions from silently failing when the SignalK login expires (show a re-login prompt).

**Architecture:** All changes are in the SignalK manager plugin (`signalk-espdisp-manager`), a Node/Express-style server-rendered app. The pages are built by `renderUiShell()` + per-page render functions in `index.js`. We merge two render functions, edit the `nav()` items array, and add one shared client-side script (injected by `renderUiShell`) that intercepts form POSTs and surfaces auth-expiry.

**Tech Stack:** Node 18, server-rendered HTML strings, `node test/run.js` (assert-based), Playwright (`*.pw.js`).

**Spec:** `docs/superpowers/specs/2026-06-18-manager-device-console-design.md` §1.

**Repo:** `/Users/borissorochkin/code/embedded/signalk-espdisp-manager` (its own git repo, `main`). `node_modules` present. Run tests with `node test/run.js` or a single file `node test/<name>.test.js`.

---

## File Structure

- **Modify** `index.js`:
  - `nav()` (~line 2748): remove `profiles` (Presets) + `layout` (Layout editor) + `overview` entries; keep a single `Devices` entry (→ `/ui`) and `Firmware`.
  - Page dispatch (~line 1227) + `renderOverviewPage` (~1239) + `renderDevicesPage` (~1255): merge into one home page = stat tiles + pending + registered table + clear actions. `/ui` and `/ui/devices` both render it.
  - `renderUiShell()` (~line where the shell template lives): inject a shared `<script>` (a string constant) that wraps form POSTs to detect login-expiry.
- **Create** `test/ui-restructure.test.js` — asserts the merged page HTML + nav contents.
- **Create** `test/relogin.pw.js` — Playwright: a form POST against an expired session surfaces the re-login modal (registered in `playwright.config.js` glob if needed; otherwise run directly).
- **Modify** `test/run.js` — register `ui-restructure.test`.

---

## Task 1: Merge Overview + Devices into one home page

**Files:**
- Modify: `index.js` (`renderDevicesPage`, `renderOverviewPage`, the page-dispatch block ~1227, the `/ui/devices` route ~591)
- Create: `test/ui-restructure.test.js`
- Modify: `test/run.js`

- [ ] **Step 1: Read the current code.** Read `index.js` lines ~1220–1320 (the page dispatch, `renderOverviewPage`, the start of `renderDevicesPage`) and the `/ui` (~586) and `/ui/devices` (~591) routes. Note: `renderOverviewPage(dashboard)` builds the stat tiles (`.grid` of `.metric`) + a "Recent devices" table; `renderDevicesPage(devices, req, manager)` builds the Devices stat line + clear actions + pending section + registered table. The dashboard object has `.devices`, `.online`, `.configDrift`, `.pendingCommands`, `.firmwareJobs` counts (confirm exact field names by reading `renderOverviewPage`).

- [ ] **Step 2: Write the failing test**

`test/ui-restructure.test.js`:

```js
const assert = require('assert')
const { makeManager } = require('./test-utils')
const { MockFirmware } = require('./mock-firmware')

const { manager, auth } = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' },
  network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' }
})
const dev = new MockFirmware(manager, {
  deviceId: 'espdisp-merge-1', auth,
  display: { width: 480, height: 480, shape: 'square', rotation: 0, colorDepth: 16 }
})
dev.register()

// renderHomePage is the merged renderer (stats + devices). It must be exported
// for test (see implementation). It takes the dashboard summary + devices + req + manager.
const { __renderHomePage } = require('../index.js')
const dashboard = manager.dashboard()
const html = __renderHomePage(dashboard, dashboard.devices, { query: {} }, manager)

// Merged page has BOTH the overview stat tiles AND the devices table + clear actions.
assert.ok(/class="grid"/.test(html), 'merged page has the overview stat grid')
assert.ok(/Clear all/.test(html), 'merged page has the Clear all action')
assert.ok(/Registered \(/.test(html), 'merged page has the registered devices table')
assert.ok(/espdisp-merge-1/.test(html), 'merged page lists the registered device')

console.log('ui-restructure.test: home merge OK')
```

(If `manager.dashboard()` is not the exact method name, read `index.js` for how the `/ui` route obtains the dashboard summary — likely `manager.dashboard()` or `manager.summary()` — and use that. Adjust the test accordingly.)

- [ ] **Step 3: Register + run red**

Add `require('./ui-restructure.test')` to `test/run.js` after the `display-widgets.test` require.
Run: `node test/ui-restructure.test.js`
Expected: FAIL — `__renderHomePage` is not exported (TypeError: not a function).

- [ ] **Step 4: Implement the merge**

In `index.js`:
1. Add a `renderHomePage(dashboard, devices, req, manager)` function that returns the overview stat-tile `.grid` block (lift it from `renderOverviewPage`) immediately followed by the full devices body (lift the `<section class="panel">…</section>` from `renderDevicesPage`, minus any duplicate `<h2>Devices</h2>` — keep one). Reuse the existing helper closures (`act`, `deviceTable`, `renderPendingDiscoverySection`) — call `renderDevicesPage`'s internals or refactor them into `renderHomePage`. Keep it DRY: have `renderDevicesPage` delegate to `renderHomePage` so there is one implementation.
2. Export it for tests: at the bottom of `index.js` where `module.exports` is (or add) `module.exports.__renderHomePage = renderHomePage` (guard if module.exports is a function — attach as a property).
3. Page dispatch (~1227): make the default (`page === '' || page === 'overview'`) AND `page === 'devices'` both `return renderHomePage(dashboard, dashboard.devices, req, manager)`.
4. `/ui/devices` route (~591): keep it, but have it render the same shell with `page='devices'` (so the merged page shows, active nav = Devices). `/ui` route (~586) renders the same merged page with `page='devices'` active too (single nav entry).

- [ ] **Step 5: Run green + full suite**

Run: `node test/ui-restructure.test.js` → `ui-restructure.test: home merge OK`.
Run: `node test/run.js` → confirm no NEW failures vs the known pre-existing ones (`app-dock-config`, `widget-parity`, `proto-control` are environmental; record them first if unsure by stashing your change mentally — the only acceptable failures are those three).

- [ ] **Step 6: Commit**

```bash
git add index.js test/ui-restructure.test.js test/run.js
git commit -m "feat(ui): merge Overview + Devices into one home page"
```

---

## Task 2: Trim the nav (remove Presets + Layout editor + Overview)

**Files:**
- Modify: `index.js` (`nav()` ~line 2748)
- Modify: `test/ui-restructure.test.js`

- [ ] **Step 1: Add the failing assertion**

Append to `test/ui-restructure.test.js`:

```js
// Nav: only Devices + Firmware remain (Presets / Layout editor / Overview gone).
const { __nav } = require('../index.js')
const navHtml = __nav('devices')
assert.ok(/>Devices</.test(navHtml), 'nav has Devices')
assert.ok(/>Firmware</.test(navHtml), 'nav has Firmware')
assert.ok(!/>Presets</.test(navHtml), 'nav no longer has Presets')
assert.ok(!/>Layout editor</.test(navHtml), 'nav no longer has Layout editor')
assert.ok(!/>Overview</.test(navHtml), 'nav no longer has Overview (merged into Devices)')
console.log('ui-restructure.test: nav trim OK')
```

- [ ] **Step 2: Run red**

Run: `node test/ui-restructure.test.js`
Expected: FAIL — `__nav` not exported (and/or Presets still present).

- [ ] **Step 3: Implement**

In `index.js` `nav()`, replace the `items` array with:

```js
  const items = [
    ['devices', '/plugins/espdisp-manager/ui', 'Devices'],
    ['firmware', '/plugins/espdisp-manager/ui/firmware', 'Firmware']
  ]
```

Export the nav function for tests: add `module.exports.__nav = nav` next to the other test exports. (The `/ui/profiles` and `/ui/layout` routes stay defined and reachable directly; we only remove the nav entries.)

- [ ] **Step 4: Run green**

Run: `node test/ui-restructure.test.js` → both `home merge OK` and `nav trim OK`.
Run: `node test/run.js` → no new failures.

- [ ] **Step 5: Commit**

```bash
git add index.js test/ui-restructure.test.js
git commit -m "feat(ui): drop Presets + Layout editor + Overview from nav"
```

---

## Task 3: Re-login prompt on expired session

**Files:**
- Modify: `index.js` (`renderUiShell` — inject a shared script + a modal container)
- Create: `test/relogin.pw.js`

The manager pages do server-rendered `<form method="post">` submits. When the SignalK session cookie expires, those POSTs get a login redirect / 401 and the page silently does nothing. We intercept submits client-side, run them via `fetch`, and if the response lands on the login page (or 401), show a "Session expired" modal instead of failing silently.

- [ ] **Step 1: Read `renderUiShell`** to find the `</body>` / closing of the template and where `body` is interpolated, so the script + modal go at the end of `<body>`.

- [ ] **Step 2: Implement the shared script + modal**

In `renderUiShell`, just before `</body>`, inject:

```html
<div id="relogin-modal" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,.5);z-index:9999;align-items:center;justify-content:center;">
  <div style="background:#fff;color:#172026;max-width:360px;padding:20px;border-radius:8px;">
    <h2 style="margin:0 0 8px;">Session expired</h2>
    <p style="margin:0 0 14px;">Your SignalK login has expired, so the action didn't run. Log in again, then retry.</p>
    <a href="/admin/#/login" target="_top" style="display:inline-block;background:#116078;color:#fff;padding:8px 14px;border-radius:4px;text-decoration:none;font-weight:600;">Re-login</a>
    <button onclick="document.getElementById('relogin-modal').style.display='none'" style="margin-left:8px;background:#fff;color:#116078;">Dismiss</button>
  </div>
</div>
<script>
(function () {
  function looksLikeLogin (resp) {
    return resp.status === 401 || /\/admin\/?#?\/?login|Please Login/i.test(resp.url || '')
  }
  document.addEventListener('submit', async function (e) {
    var form = e.target
    if (!form || form.method.toLowerCase() !== 'post') return
    // confirm() in onsubmit attrs still applies: if it already returned false the
    // browser won't fire 'submit', so we only get here on a confirmed submit.
    e.preventDefault()
    try {
      var resp = await fetch(form.action, { method: 'POST', body: new FormData(form), redirect: 'follow', credentials: 'include' })
      if (looksLikeLogin(resp)) { document.getElementById('relogin-modal').style.display = 'flex'; return }
      // success: the POST 303-redirected to a result page; show it.
      window.location.assign(resp.url || window.location.href)
    } catch (err) {
      document.getElementById('relogin-modal').style.display = 'flex'
    }
  }, true)
})();
</script>
```

(Place the modal div + script literally inside the template string returned by `renderUiShell`, before `</body>`. Escape `</script>` correctly inside the JS template string — there is none here, but keep the block as a plain template literal segment.)

- [ ] **Step 3: Sanity build**

Run: `node -e "require('./index.js'); console.log('index loads OK')"` → prints OK (no syntax error in the injected template).
Run: `node test/run.js` → no new failures.

- [ ] **Step 4: Playwright check**

Create `test/relogin.pw.js` that: starts the manager (reuse the harness in existing `*.pw.js` files — read `test/dashboard-ui.pw.js` for how it boots the server + a page), loads `/plugins/espdisp-manager/ui` WITHOUT auth (or with an expired/blank session), submits a POST form (e.g. the clear-all form), and asserts `#relogin-modal` becomes visible. Follow the existing pw harness patterns exactly; if the harness can't easily simulate an expired session, assert instead that a submit whose fetch resolves to a `/admin#/login` URL shows the modal (stub `window.fetch` via `page.addInitScript`). Keep it to one focused assertion.

Run: `node test/relogin.pw.js` (or via the project's playwright runner) → passes.

- [ ] **Step 5: Commit**

```bash
git add index.js test/relogin.pw.js
git commit -m "feat(ui): prompt re-login when a manager action hits an expired session"
```

---

## Task 4: Deploy + verify on the lab

**Files:** none (deploy).

- [ ] **Step 1: Push the repo**

```bash
git push origin main
```

- [ ] **Step 2: Deploy to mythra-nav** (rsync the changed `index.js` + tests; restart signalk-server)

```bash
rsync -az --exclude node_modules --exclude test-results --exclude .git --exclude .gitignore --exclude deploy --exclude '*.bak*' \
  ./ compulab@mythra-nav:/home/compulab/yeydisp-signalk/plugins/signalk-espdisp-manager/
ssh compulab@mythra-nav 'docker restart signalk-server >/dev/null && for i in $(seq 1 30); do c=$(curl -s -o /dev/null -w "%{http_code}" http://localhost:3000/signalk); [ "$c" = 200 -o "$c" = 401 ] && { echo "up $c"; break; }; sleep 3; done'
```

- [ ] **Step 3: Verify** via the SSH tunnel + browser (Playwright or manual): the landing page shows the merged stats+devices, the nav has only Devices + Firmware, and a form action that hits an expired session pops the re-login modal. Confirm Clear-offline/Clear-all still work.

---

## Self-Review

- **Spec §1 coverage:** merge Overview+Devices (Task 1), remove Presets/Layout-editor nav (Task 2), relogin-on-expiry (Task 3), deploy (Task 4). ✓
- **No placeholders:** every code step has the actual code; test names + run commands are exact. The one judgement call (pw harness for expired session) names the fallback (stub fetch) explicitly.
- **Type/name consistency:** `renderHomePage` / `__renderHomePage` / `__nav` used consistently; `nav()` items shape matches the existing `[id, href, label]` tuple; the relogin modal id `#relogin-modal` is consistent between the div and the script.
- **DRY:** `renderDevicesPage` delegates to `renderHomePage` (one implementation); the relogin script is injected once in `renderUiShell` so it applies to every page.
