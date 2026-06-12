# SP1 — Stability (Evidence-First) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore device observability and harden the firmware so a 24 h soak runs with zero reboots and zero unrecovered stalls.

**Architecture:** Four independent workstreams. **A — Manager reachability** (plugin JS, host-testable now) fixes the verified defect that orphans the device on IP rotation. **B — Firmware net-health + canaries + heartbeat labeling** (C++/sdkconfig, device-gated). **C — Soak rig** (Python, runs on mythra-nav). **D — Rideshare cleanups** (firmware). A and C need no physical device and run first; B and D are gated on physical access to the bench device, currently unreachable (see `docs/reports/stability-evidence-2026-06-12.md`).

**STATUS 2026-06-12:** ✅ **Workstream A complete** (commits `fc591a9`, deployed+verified on mythra-nav — 9.2 s three-candidate fall-through proven). ✅ **Workstream C complete** (commit `15bbd81` — `espdisp soak` + pure verdict, 7 host tests, relay-verified). ⏳ **B2 (net_health)** is host-testable and ready to start next. ⛔ **B1/B3/B4/D** await a physical device (offline).

**Tech Stack:** Node.js (SignalK plugin, `node:test` via `test/run.js`), C++17 (firmware, Unity host tests via `pio test -e native`), ESP-IDF sdkconfig, Python 3 (`tools/espdisp.py`).

---

## Decomposition note

SP1 spans four subsystems. This document fully details **Workstream A** (bite-sized TDD, ready to execute today) and **Workstream C** (soak rig, host-testable). **Workstreams B and D** are scoped here with exact file targets, test commands, and acceptance criteria, but their bite-sized steps are deliberately deferred until the bench device is physically reachable — writing fake "expected output" for `make ota-verify` against an unreachable device would be a placeholder, not a plan. When the device is back, expand B and D into per-step tasks following the A/C pattern.

---

## Workstream A — Manager reachability & re-resolution

**Problem (verified 2026-06-12):** `Manager.deviceHttpBase()` resolves a device's
HTTP base from a fixed precedence — `status.network.ip` → `lastResolvedAddress`
→ `currentFqdn` → `desiredFqdn` — and returns the **first** candidate only
(`signalk/plugins/signalk-espdisp-manager/lib/manager.js:1054-1061`). When DHCP
rotates the IP, the cached `status.network.ip` is stale, the fetch times out,
and the live-status/logs/config proxies fail with `device_unreachable` even
though the device is reachable by its mDNS FQDN. The fix: try **all** candidate
addresses in precedence order, falling through on connection failure, and put
the mDNS FQDN ahead of stale cached IPs once an IP has proven unreachable.

**Files:**
- Modify: `signalk/plugins/signalk-espdisp-manager/lib/manager.js` (add `deviceHttpCandidates()`, rewrite `fetchDeviceJson()`)
- Test: `signalk/plugins/signalk-espdisp-manager/test/device-resolution.test.js` (create)
- Test runner: `signalk/plugins/signalk-espdisp-manager/test/run.js` is a **manual require-list** (one `require('./<name>.test')` line per suite) — the new suite must be added to it (Task A1 Step 4b).

All commands below run from `signalk/plugins/signalk-espdisp-manager/`.

### Task A1: Candidate enumeration

**Files:**
- Modify: `lib/manager.js:1054`
- Test: `test/device-resolution.test.js`

- [ ] **Step 1: Write the failing test**

Create `test/device-resolution.test.js`:

```js
const test = require('node:test')
const assert = require('node:assert')
// The exported class is EspDispManager (see lib/manager.js module.exports).
const { EspDispManager } = require('../lib/manager')

// Minimal instance carrying only what the resolution methods touch — they
// reference the module-level httpError, so a bare prototype object suffices.
function mgr () {
  return Object.create(EspDispManager.prototype)
}

test('deviceHttpCandidates lists ip then fqdn, deduped, port 80', () => {
  const device = {
    status: { network: { ip: '10.75.205.170' } },
    networkIdentity: {
      lastResolvedAddress: '10.75.205.170',
      currentFqdn: 'espdisp.local',
      desiredFqdn: 'espdisp-device.local'
    }
  }
  const got = mgr().deviceHttpCandidates(device)
  assert.deepStrictEqual(got, [
    'http://10.75.205.170:80',
    'http://espdisp.local:80',
    'http://espdisp-device.local:80'
  ])
})

test('deviceHttpCandidates throws when no address fields present', () => {
  assert.throws(() => mgr().deviceHttpCandidates({}), /device_address_unknown/)
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node --test test/device-resolution.test.js`
Expected: FAIL — `mgr().deviceHttpCandidates is not a function`.

- [ ] **Step 3: Add `deviceHttpCandidates`, keep `deviceHttpBase` as a thin wrapper**

In `lib/manager.js`, replace the `deviceHttpBase (device) { ... }` method (lines 1054-1061) with:

```js
  deviceHttpCandidates (device) {
    const ni = device.networkIdentity || {}
    const raw = [
      device.status && device.status.network && device.status.network.ip,
      ni.lastResolvedAddress,
      ni.currentFqdn,
      ni.desiredFqdn
    ].filter(Boolean)
    const seen = new Set()
    const urls = []
    for (const addr of raw) {
      const url = `http://${addr}:80`
      if (!seen.has(url)) { seen.add(url); urls.push(url) }
    }
    if (urls.length === 0) {
      throw httpError(409, 'device_address_unknown', 'device address is unknown')
    }
    return urls
  }

  deviceHttpBase (device) {
    return this.deviceHttpCandidates(device)[0]
  }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node --test test/device-resolution.test.js`
Expected: PASS (both tests).

- [ ] **Step 4b: Register the suite in the manual runner**

`test/run.js` is an explicit require-list. Add this line (keep alphabetical-ish
grouping with the other device tests):

```js
require('./device-resolution.test')
```

Run: `node test/run.js`
Expected: the new suite runs as part of the full set; all pass.

- [ ] **Step 5: Commit**

```bash
git add lib/manager.js test/device-resolution.test.js test/run.js
git commit -m "feat(manager): enumerate device http candidates for re-resolution"
```

### Task A2: Fetch falls through candidates on connection failure

**Files:**
- Modify: `lib/manager.js:1072` (`fetchDeviceJson`)
- Test: `test/device-resolution.test.js`

- [ ] **Step 1: Write the failing test**

Append to `test/device-resolution.test.js`:

```js
test('fetchDeviceJson falls through to the next candidate on failure', async () => {
  const m = mgr()
  m.deviceWebAuth = () => null
  const attempted = []
  // Stub the low-level getter: first URL (stale IP) throws ECONNREFUSED-like,
  // second (fqdn) succeeds.
  m._httpGetJson = (url) => {
    attempted.push(url)
    if (url.startsWith('http://10.75.205.170')) {
      return Promise.reject(new Error('device request timeout'))
    }
    return Promise.resolve({ ok: true, from: url })
  }
  const device = {
    status: { network: { ip: '10.75.205.170' } },
    networkIdentity: { currentFqdn: 'espdisp.local' }
  }
  const out = await m.fetchDeviceJson(device, '/api/state')
  assert.deepStrictEqual(attempted, [
    'http://10.75.205.170:80/api/state',
    'http://espdisp.local:80/api/state'
  ])
  assert.strictEqual(out.from, 'http://espdisp.local:80/api/state')
})

test('fetchDeviceJson rethrows the last error when all candidates fail', async () => {
  const m = mgr()
  m.deviceWebAuth = () => null
  m._httpGetJson = () => Promise.reject(new Error('device request timeout'))
  const device = { networkIdentity: { currentFqdn: 'espdisp.local' } }
  await assert.rejects(() => m.fetchDeviceJson(device, '/api/state'),
    /device request timeout/)
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node --test test/device-resolution.test.js`
Expected: FAIL — `fetchDeviceJson` currently calls `deviceHttpBase` (single URL) and the module-level `httpGetJson`, so `attempted` has one entry and `m._httpGetJson` is never consulted.

- [ ] **Step 3: Rewrite `fetchDeviceJson` to iterate candidates via an injectable getter**

In `lib/manager.js`, replace `fetchDeviceJson` (lines 1072-1076) with:

```js
  // Indirection so tests can stub the transport; defaults to the module
  // httpGetJson. Returns a Promise<json>.
  _httpGetJson (url, auth) {
    return httpGetJson(url, auth)
  }

  async fetchDeviceJson (device, path) {
    const auth = this.deviceWebAuth(device)
    const candidates = this.deviceHttpCandidates(device)
    let lastErr
    for (const base of candidates) {
      try {
        return await this._httpGetJson(`${base}${path}`, auth)
      } catch (err) {
        lastErr = err
        if (this.app && this.app.debug) {
          this.app.debug(`espdisp device fetch ${base}${path} failed: ${err.message}`)
        }
      }
    }
    throw lastErr
  }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `node --test test/device-resolution.test.js`
Expected: PASS (all four tests).

- [ ] **Step 5: Run the full plugin suite for no regressions**

Run: `node test/run.js`
Expected: existing suites pass, including `test/live-device.test.js` (its
`getLiveStatus` happy path still works — the first candidate is the live IP).

- [ ] **Step 6: Commit**

```bash
git add lib/manager.js test/device-resolution.test.js
git commit -m "fix(manager): fall through device address candidates on fetch failure"
```

### Task A3: Persist a freshly-resolved address back to the device record

**Files:**
- Modify: `lib/manager.js` (`fetchDeviceJson` success branch + `getDevice`/store write)
- Test: `test/device-resolution.test.js`

- [ ] **Step 1: Write the failing test**

Append to `test/device-resolution.test.js`:

```js
test('a non-primary winning candidate is promoted to lastResolvedAddress', async () => {
  const m = mgr()
  m.deviceWebAuth = () => null
  m._httpGetJson = (url) =>
    url.startsWith('http://espdisp.local')
      ? Promise.resolve({ ok: true })
      : Promise.reject(new Error('device request timeout'))
  const writes = []
  m.noteResolvedAddress = (device, host) => writes.push(host)
  const device = {
    id: 'espdisp',
    status: { network: { ip: '10.75.205.170' } },
    networkIdentity: { currentFqdn: 'espdisp.local' }
  }
  await m.fetchDeviceJson(device, '/api/state')
  assert.deepStrictEqual(writes, ['espdisp.local'])
})
```

- [ ] **Step 2: Run test to verify it fails**

Run: `node --test test/device-resolution.test.js`
Expected: FAIL — `m.noteResolvedAddress` is never called (method does not exist; success branch doesn't record the winner).

- [ ] **Step 3: Record the winning host on success**

In `lib/manager.js`, update the success branch of `fetchDeviceJson` and add the
helper. Change the `try` block to capture the host and call `noteResolvedAddress`
only when the winner is not the first candidate:

```js
    for (let i = 0; i < candidates.length; i++) {
      const base = candidates[i]
      try {
        const json = await this._httpGetJson(`${base}${path}`, auth)
        if (i > 0) {
          const host = base.replace(/^http:\/\//, '').replace(/:\d+$/, '')
          this.noteResolvedAddress(device, host)
        }
        return json
      } catch (err) {
        lastErr = err
        if (this.app && this.app.debug) {
          this.app.debug(`espdisp device fetch ${base}${path} failed: ${err.message}`)
        }
      }
    }
    throw lastErr
```

Add the helper next to `deviceHttpCandidates`:

```js
  noteResolvedAddress (device, host) {
    if (!device || !host) return
    device.networkIdentity = device.networkIdentity || {}
    device.networkIdentity.lastResolvedAddress = host
    // Devices live in the registry store; saveRegistry() is the persist call
    // used throughout this class (e.g. lib/manager.js:293,700,874).
    if (this.store && typeof this.store.saveRegistry === 'function') {
      this.store.saveRegistry()
    }
  }
```

> The test stubs `noteResolvedAddress`, so it stays green regardless of the
> persist call; `saveRegistry()` is the verified method name for device
> persistence in this class.

- [ ] **Step 4: Run test to verify it passes**

Run: `node --test test/device-resolution.test.js`
Expected: PASS (all five tests).

- [ ] **Step 5: Run the full suite**

Run: `node test/run.js`
Expected: all suites pass.

- [ ] **Step 6: Commit**

```bash
git add lib/manager.js test/device-resolution.test.js
git commit -m "feat(manager): promote a recovered address to lastResolvedAddress"
```

### Task A4: Deploy to mythra-nav and confirm recovery against the real device

**Files:** none (deployment + verification). This is the workstream's done-gate.

- [ ] **Step 1: Sync the plugin to the lab host**

The lab host is pre-authorized for dev pushes (mythra-nav, user
`compulab`). Copy the two changed files:

```bash
scp lib/manager.js test/device-resolution.test.js \
  compulab@mythra-nav:~/.signalk/node_modules/signalk-espdisp-manager/lib/manager.js \
  # (adjust install path; find it with: ssh compulab@mythra-nav 'ls ~/.signalk/node_modules | grep espdisp')
```

Expected: confirm the install path first; SignalK plugins typically live under
`~/.signalk/node_modules/<plugin>` on the server.

- [ ] **Step 2: Restart the plugin / SignalK server**

```bash
ssh compulab@mythra-nav 'sudo systemctl restart signalk || (cd ~/.signalk && pm2 restart signalk-server)'
```

Expected: server comes back; `curl -s http://mythra-nav:3000/signalk` returns the
endpoints JSON.

- [ ] **Step 3: Confirm the proxy now reaches the device by FQDN**

```bash
TOK=$(curl -s -X POST http://mythra-nav:3000/signalk/v1/auth/login \
  -H 'Content-Type: application/json' -d '{"username":"admin","password":"admin"}' \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
curl -s -H "Authorization: Bearer $TOK" \
  http://mythra-nav:3000/plugins/espdisp-manager/devices/espdisp/live/status | head -c 400
```

Expected: **either** real device JSON (`{"...","heap":...}`) if the device is
powered and on the boat subnet — proving FQDN fallback works — **or**, if the
device is genuinely offline, still `device_unreachable` but the server log
(`ssh compulab@mythra-nav 'journalctl -u signalk --since "2 min ago" | grep espdisp'`)
now shows **both** candidates attempted (stale IP then `espdisp.local`),
proving the fall-through executed.

- [ ] **Step 4: Record the outcome**

Append a "Workstream A verification" section to
`docs/reports/stability-evidence-2026-06-12.md` with the captured output, then:

```bash
cd /Users/borissorochkin/code/embedded/espdisp
git add docs/reports/stability-evidence-2026-06-12.md
git commit -m "docs(stability): record manager re-resolution verification"
git push origin main
```

---

## Workstream C — Soak rig (runs on mythra-nav)

**Goal:** An unattended scraper that proves/refutes stability claims with data.
Must run on the device's subnet (mythra-nav), polling the device through its own
HTTP API (or the manager proxy if direct access is blocked), writing JSONL and a
verdict table.

**Files:**
- Modify: `tools/espdisp.py` (add a `soak` subcommand)
- Test: `tools/test_soak.py` (create — pure-Python unit test of the verdict logic, no network)

All commands run from repo root.

### Task C1: Verdict logic (pure, host-tested)

**Files:**
- Create: `tools/soak_verdict.py` (pure functions; importable without network deps)
- Test: `tools/test_soak.py`

- [ ] **Step 1: Write the failing test**

Create `tools/test_soak.py`:

```python
import unittest
from soak_verdict import analyze, Sample

class TestVerdict(unittest.TestCase):
    def test_detects_reboot_on_uptime_regression(self):
        samples = [
            Sample(t=0,   uptime_ms=600000, sk_age_ms=200, heap=120000),
            Sample(t=30,  uptime_ms=630000, sk_age_ms=200, heap=119000),
            Sample(t=60,  uptime_ms=5000,   sk_age_ms=200, heap=121000),  # reboot
        ]
        v = analyze(samples)
        self.assertEqual(v.reboots, 1)
        self.assertFalse(v.passed)

    def test_detects_stall_when_sk_age_grows_past_threshold(self):
        samples = [
            Sample(t=0,  uptime_ms=1000,   sk_age_ms=200,    heap=120000),
            Sample(t=30, uptime_ms=31000,  sk_age_ms=35000,  heap=120000),  # stalled >30s
        ]
        v = analyze(samples, stall_ms=30000)
        self.assertEqual(v.stalls, 1)
        self.assertFalse(v.passed)

    def test_clean_run_passes(self):
        samples = [
            Sample(t=i*30, uptime_ms=1000 + i*30000, sk_age_ms=200, heap=120000 - i*10)
            for i in range(10)
        ]
        v = analyze(samples)
        self.assertEqual(v.reboots, 0)
        self.assertEqual(v.stalls, 0)
        self.assertTrue(v.passed)

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools && python3 -m pytest test_soak.py -q` (or `python3 test_soak.py`)
Expected: FAIL — `ModuleNotFoundError: No module named 'soak_verdict'`.

- [ ] **Step 3: Implement the verdict module**

Create `tools/soak_verdict.py`:

```python
from dataclasses import dataclass
from typing import List

@dataclass
class Sample:
    t: int          # seconds since start
    uptime_ms: int
    sk_age_ms: int  # ms since last SignalK value update
    heap: int       # free internal heap bytes

@dataclass
class Verdict:
    reboots: int
    stalls: int
    min_heap: int
    samples: int
    passed: bool

def analyze(samples: List[Sample], stall_ms: int = 30000) -> Verdict:
    reboots = 0
    stalls = 0
    min_heap = min((s.heap for s in samples), default=0)
    for i in range(1, len(samples)):
        prev, cur = samples[i - 1], samples[i]
        # uptime going backwards (allowing small jitter) means the device rebooted
        if cur.uptime_ms + 5000 < prev.uptime_ms:
            reboots += 1
    for s in samples:
        if s.sk_age_ms >= stall_ms:
            stalls += 1
    passed = reboots == 0 and stalls == 0 and len(samples) > 0
    return Verdict(reboots=reboots, stalls=stalls, min_heap=min_heap,
                   samples=len(samples), passed=passed)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd tools && python3 -m pytest test_soak.py -q`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/soak_verdict.py tools/test_soak.py
git commit -m "feat(soak): pure verdict logic for reboot/stall detection"
```

### Task C2: Scraper subcommand wiring

**Files:**
- Modify: `tools/espdisp.py` (add `soak` subcommand that polls `/api/state`+`/api/diag` every 30 s, appends `Sample` rows to JSONL, prints `analyze()` at exit/SIGINT)

- [ ] **Step 1: Inspect the existing CLI structure**

Run: `grep -nE "add_parser|argparse|subcommand|def cmd_" tools/espdisp.py | head`
Expected: reveals the subparser pattern to mirror. Follow it exactly — do not
restructure the CLI.

- [ ] **Step 2: Add the `soak` subcommand**

Wire a `soak` subparser with args `--host` (device IP/FQDN or manager proxy
base), `--interval 30`, `--out soak-YYYYMMDD.jsonl`, `--stall-ms 30000`. Each
tick: GET `<host>/api/state` and `/api/diag`, extract `uptime_ms`,
`sk` last-update age, free heap; append one JSON line; on `KeyboardInterrupt`
load the JSONL into `Sample`s and print `soak_verdict.analyze(...)` as a table.

Reuse the field paths captured in `docs/reports/stability-evidence-2026-06-12.md`
(`status.ui.uptime_ms`, `status.network`, etc.) and the device's `/api/diag`
heap fields. Verify field names against a live `/api/state` capture before
trusting them.

- [ ] **Step 3: Smoke-run against the manager proxy**

Run (from a host that can reach mythra-nav):
```bash
python3 tools/espdisp.py soak \
  --host "http://mythra-nav:3000/plugins/espdisp-manager/devices/espdisp/live" \
  --interval 30 --out /tmp/soak-smoke.jsonl
# let it take 2-3 samples, then Ctrl-C
```
Expected: a verdict table prints; `/tmp/soak-smoke.jsonl` has one line per tick.
(If the device is offline this records `device_unreachable` ticks — still valid
rig behavior; the verdict notes zero usable samples.)

- [ ] **Step 4: Commit**

```bash
git add tools/espdisp.py
git commit -m "feat(soak): espdisp.py soak subcommand scrapes state/diag to JSONL"
```

---

## Workstream B — Firmware net-health, canaries, heartbeat labeling (DEVICE-GATED)

Cannot be bite-sized with honest expected-output until the bench device is
reachable. Scope, targets, and acceptance criteria below; expand to per-step
tasks (A/C pattern) once `make flash`/`make ota` works again.

- **B1 — Stack canaries.** In `sdkconfig.esp32-4848s040`, change
  `CONFIG_COMPILER_STACK_CHECK_MODE_NONE=y` (line 221) to
  `CONFIG_COMPILER_STACK_CHECK_MODE_STRONG=y` and clear the related
  `# ... is not set` lines (222-224) / `CONFIG_STACK_CHECK_NONE` (1424).
  Acceptance: `make build` succeeds; `make ota-verify` boots; a deliberate
  stack-overflow test fixture (temporary) trips a named `__stack_chk_fail`
  panic visible in the prevboot ring.
- **B2 — `net_health` pure module.** Create `include/net_health.h` +
  `src/net_health.cpp` (a state machine: inputs = WS frame age, HTTP
  success/fail counts, RSSI-present flag; output = `OK | DEGRADED | DEAD`).
  Add `src/net_health.cpp` to the `[env:native]` `build_src_filter` in
  `platformio.ini:247` and a `test/test_net_health/test_net_health.cpp`
  Unity suite (host-testable NOW — this sub-task is not device-gated; it can
  be lifted into Workstream A/C ordering). Acceptance: `pio test -e native -f
  test_net_health` green.
- **B3 — Rewire the stall ladder.** In `src/signalk.cpp`,
  `check_stall_autorecover` (line 418) keys its escalation off the
  `net_health` verdict instead of trusting `WiFi.status()`/raw timers alone.
  Preserve the RTC boot-loop guard (`s_rtc_recovery_*`, lines 376-398) and the
  30/90/180 s thresholds (360-362). Acceptance: existing behavior unchanged on
  a healthy link; on a forced half-up state the ladder still escalates.
- **B4 — S5 heartbeat labeling.** In `src/manager.cpp`, the heartbeat path
  records pre-flight refusal (`NotProvisioned`/`LowHeap`/`WifiDown`) distinctly
  from transport failure in the `/api/diag` counters. Acceptance: `/api/diag`
  exposes separate counters; visible in a soak capture.

## Workstream D — Rideshare cleanups (DEVICE-GATED for verify only)

- **D1 — Release-gate `touch_cal`.** `src/ui/screen_touch_cal.cpp` builds the
  calibration UI unconditionally. Wrap the **screen builder** (not the core
  `touch_cal::apply/set/reset` path, which must stay) behind a
  `#ifndef ESPDISP_RELEASE_BUILD`. Add the `-D ESPDISP_RELEASE_BUILD` flag to a
  release env in `platformio.ini`. Acceptance: dev build still shows the screen;
  release build drops it and shrinks the binary.
- **D2 — Settings segmented control.** NOTE: `src/ui/screen_settings.cpp`
  already has a `Segmented` struct + `make_segmented`/`update_segment` factory
  (lines 31-106). Re-scope D2 to: audit for any remaining duplicated
  segmented-control blocks not yet routed through the factory; consolidate only
  if real duplication remains. If none, mark D2 done with a one-line note. Do
  not manufacture a refactor that isn't needed.

---

## Self-review notes

- **Spec coverage:** SP1 spec items map as — discovery/re-resolution → A1-A4;
  heartbeat-loop restoration → A4 (verification) + B4 (labeling); stack
  canaries → B1; soak rig on mythra-nav → C1-C2; net_health/S4 → B2-B3; S5 →
  B4; cleanups → D1-D2. The spec's "device unreachable" finding is the reason A
  is sequenced first.
- **Corrections folded in:** mDNS is already enabled in current firmware
  (`src/net.cpp:548-567`), so the defect is manager-side, not device-side — A
  reflects that. `screen_settings.cpp` already has the Segmented factory, so D2
  is re-scoped to an audit.
- **Ordering rationale:** A and C need no physical device and are fully
  host-tested; B2's `net_health` host test is also doable now and may be pulled
  forward. B1/B3/B4/D verification are gated on physical device access.
