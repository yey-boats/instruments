# YB-MIDL Manager Migration Plan (phased)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Each SLICE is executed and reviewed before the next begins. Slices 2+ modify the live editor — checkpoint with the human between slices.

**Goal:** Migrate the `yey-boats-display-manager` editor from its `yeyboats.dashboard.v2` config model (`widgets`+`tiles`, `viewTypes` manifest, `lib/field-schema.js` validator) to the **YB-MIDL** model (`screens → elements → layout-tree`, `elements`+`classes` manifest, `@yey-boats/midl` validator) — incrementally and non-breakingly.

**Architecture:** The manager includes the MIDL repo as a submodule and consumes its built CJS validator. A bidirectional **v2 ↔ MIDL adapter** lets the existing model and the new model coexist during the transition, so each slice ships working software. Server-side validation adopts MIDL first (authoritative), then the editor UI is driven by the MIDL element catalog, then the editor authors MIDL natively. The final "device receives MIDL" cutover is gated on Plan 6 (firmware accepting MIDL).

**Tech Stack:** Node CommonJS + browser ES5 (manager, no bundler), `@yey-boats/midl` (built ESM/CJS), `node:test`, Playwright.

**Repos:** manager `/Users/borissorochkin/code/embedded/signalk-espdisp-manager`; MIDL submodule mounted at `<manager>/midl`.

---

## Why phased

The manager is deployed in the lab and has a working editor + validation + device config flow. A big-bang rewrite would break it. Each slice below is independently shippable and (slices 1–3) non-breaking; the model only flips once the adapter, validation, and UI are all MIDL-ready and the firmware (Plan 6) accepts MIDL.

## Slice map

| Slice | What | Breaking? | Couples with |
|---|---|---|---|
| **1. Foundation** | MIDL submodule + built-validator wrapper + bidirectional v2↔MIDL adapter + tests | No (additive) | — |
| 2. Server validation | validate-before-post: translate incoming v2 → MIDL → validate vs the device's generated manifest; reject/annotate on failure | No (guard only) | Slice 1 |
| 3. Catalog-driven UI | editor reads widget choices/limits from the MIDL manifest's `elements`/`classes` (replacing hardcoded `viewTypes`); still emits v2 | No (UI source swap) | Slice 1–2 |
| 4. Author MIDL natively | editor builds/stores MIDL documents; v2 kept only via adapter for legacy presets | Yes (storage) | Slice 1–3 |
| 5. Device cutover | post MIDL to devices; remove v2 path | Yes | **Plan 6** (firmware accepts MIDL) |

**This plan fully specifies Slice 1.** Slices 2–5 are scoped above and detailed just-in-time after each prior slice lands and is reviewed.

---

## Slice 1 — Foundation (this plan)

### Type/string mapping (v2 → MIDL element token)

| v2 `type` | MIDL element token |
|---|---|
| numeric | single-value |
| text | text |
| gauge | gauge |
| bar | bar |
| compass | compass |
| windRose / windCircle | windrose |
| trend | trend |
| control | autopilot |
| button | button |

### Task 1: MIDL submodule + built validator wrapper

**Files:**
- Create submodule at `<manager>/midl`
- Modify: `<manager>/package.json` (add `midl:build` script)
- Create: `<manager>/lib/midl.js`
- Create: `<manager>/test/midl.test.js`

- [ ] **Step 1: Add the MIDL submodule** (file protocol must be allowed for the local path):

```bash
cd /Users/borissorochkin/code/embedded/signalk-espdisp-manager
git -c protocol.file.allow=always submodule add /Users/borissorochkin/code/embedded/midl midl
```
(Note: the `.gitmodules` url is a local path for now; set it to the yey-boats/midl remote before sharing.)

- [ ] **Step 2: Add a build script to `<manager>/package.json`** — in the `"scripts"` object, add:

```json
"midl:build": "npm --prefix midl/ts install --no-audit --no-fund && npm --prefix midl/ts run build"
```

- [ ] **Step 3: Build the MIDL package so the CJS dist exists**

```bash
cd /Users/borissorochkin/code/embedded/signalk-espdisp-manager && npm run midl:build
```
Expected: `midl/ts/dist/index.cjs` exists.

- [ ] **Step 4: Create `<manager>/lib/midl.js`** (CJS wrapper around the built validator + manifest loading)

```javascript
'use strict'
const path = require('path')
const fs = require('fs')

// Built CJS bundle from the MIDL submodule (run `npm run midl:build` first).
const DIST = path.join(__dirname, '..', 'midl', 'ts', 'dist', 'index.cjs')

function lib() {
  // Lazy require so a missing build yields a clear error, not a load-time crash.
  // eslint-disable-next-line global-require
  return require(DIST)
}

// Load a generated capability manifest for a resolution class from the submodule.
function manifestForClass(className) {
  const p = path.join(__dirname, '..', 'midl', 'schemas', 'gen', `yb-midl-capabilities.${className}.json`)
  return JSON.parse(fs.readFileSync(p, 'utf8'))
}

// Validate a MIDL document (text) against a class's generated manifest.
function validateMidl(docText, className) {
  return lib().validateDocument(docText, manifestForClass(className), className)
}

module.exports = { lib, manifestForClass, validateMidl }
```

- [ ] **Step 5: Create `<manager>/test/midl.test.js`** (node:test; builds the dist on demand so the test is self-contained)

```javascript
'use strict'
const { test, before } = require('node:test')
const assert = require('node:assert')
const fs = require('fs')
const path = require('path')
const { execSync } = require('child_process')

const root = path.join(__dirname, '..')

before(() => {
  if (!fs.existsSync(path.join(root, 'midl', 'ts', 'dist', 'index.cjs'))) {
    execSync('npm run midl:build', { cwd: root, stdio: 'inherit' })
  }
})

test('validateMidl accepts a valid doc for square-480', () => {
  const { validateMidl } = require('../lib/midl')
  const doc = JSON.stringify({
    midl: '1.0.0',
    screens: [{ id: 'd', elements: { a: { type: 'single-value' } }, layout: { element: 'a' } }],
  })
  const r = validateMidl(doc, 'square-480')
  assert.strictEqual(r.ok, true, JSON.stringify(r.issues))
})

test('validateMidl rejects an unsupported element', () => {
  const { validateMidl } = require('../lib/midl')
  const doc = JSON.stringify({
    midl: '1.0.0',
    screens: [{ id: 'd', elements: { a: { type: 'no-such-widget' } }, layout: { element: 'a' } }],
  })
  const r = validateMidl(doc, 'square-480')
  assert.strictEqual(r.ok, false)
  assert.ok(r.issues.some((i) => /not supported/.test(i.message)))
})
```

- [ ] **Step 6: Run + commit**

```bash
cd /Users/borissorochkin/code/embedded/signalk-espdisp-manager
node --test test/midl.test.js
```
Expected: 2 tests pass. Then commit (do NOT push; the `git submodule add` already staged `.gitmodules` + the `midl` gitlink):

```bash
git add .gitmodules midl package.json lib/midl.js test/midl.test.js
git commit -m "feat(midl): include MIDL submodule + built-validator wrapper (slice 1.1)"
```

### Task 2: Bidirectional v2 ↔ MIDL adapter

**Files:**
- Create: `<manager>/lib/midl-adapter.js`
- Create: `<manager>/test/midl-adapter.test.js`

- [ ] **Step 1: Write the failing test `<manager>/test/midl-adapter.test.js`**

```javascript
'use strict'
const { test } = require('node:test')
const assert = require('node:assert')
const { v2ToMidl, midlToV2 } = require('../lib/midl-adapter')

const v2 = {
  settings: { defaultScreen: 'dashboard' },
  widgets: { items: { sog: { type: 'numeric', path: 'navigation.speedOverGround', unit: 'kn' } } },
  layout: { screens: [{ id: 'dashboard', type: 'grid', tiles: [{ widget: 'sog', area: { col: 0, row: 0 } }] }] },
}

test('v2ToMidl maps a numeric widget to a single-value element with a signalk binding', () => {
  const m = v2ToMidl(v2)
  assert.strictEqual(m.midl.match(/^\d+\.\d+\.\d+$/) != null, true)
  const screen = m.screens[0]
  assert.strictEqual(screen.id, 'dashboard')
  const el = screen.elements.sog
  assert.strictEqual(el.type, 'single-value')
  assert.deepStrictEqual(el.bindings.value, { kind: 'signalk', path: 'navigation.speedOverGround' })
  assert.strictEqual(el.format.unit, 'kn')
})

test('a 1x1 grid maps to a single-cell grid node referencing the element', () => {
  const m = v2ToMidl(v2)
  const layout = m.screens[0].layout
  assert.strictEqual(layout.rows, 1)
  assert.strictEqual(layout.cols, 1)
  assert.deepStrictEqual(layout.cells, [{ element: 'sog' }])
})

test('midlToV2 round-trips the element type and path', () => {
  const back = midlToV2(v2ToMidl(v2))
  assert.strictEqual(back.widgets.items.sog.type, 'numeric')
  assert.strictEqual(back.widgets.items.sog.path, 'navigation.speedOverGround')
})
```

- [ ] **Step 2: Run, confirm FAIL**

Run: `cd /Users/borissorochkin/code/embedded/signalk-espdisp-manager && node --test test/midl-adapter.test.js`
Expected: FAIL — Cannot find module '../lib/midl-adapter'.

- [ ] **Step 3: Create `<manager>/lib/midl-adapter.js`**

```javascript
'use strict'

const MIDL_VERSION = '1.0.0'

const V2_TO_MIDL = {
  numeric: 'single-value', text: 'text', gauge: 'gauge', bar: 'bar',
  compass: 'compass', windRose: 'windrose', windCircle: 'windrose',
  trend: 'trend', control: 'autopilot', button: 'button',
}
const MIDL_TO_V2 = {
  'single-value': 'numeric', text: 'text', gauge: 'gauge', bar: 'bar',
  compass: 'compass', windrose: 'windCircle', trend: 'trend',
  autopilot: 'control', button: 'button',
}

// Translate a v2 dashboard object into a MIDL config document.
function v2ToMidl(v2) {
  const items = (v2.widgets && v2.widgets.items) || {}
  const screens = ((v2.layout && v2.layout.screens) || []).map((s) => {
    const elements = {}
    let maxCol = 0
    let maxRow = 0
    const tiles = s.tiles || []
    for (const t of tiles) {
      const w = items[t.widget]
      if (!w) continue
      const el = { type: V2_TO_MIDL[w.type] || 'single-value' }
      if (w.title) el.name = w.title
      if (w.path) el.bindings = { value: { kind: 'signalk', path: w.path } }
      if (w.unit) el.format = { unit: w.unit }
      elements[t.widget] = el
      maxCol = Math.max(maxCol, (t.area && t.area.col) || 0)
      maxRow = Math.max(maxRow, (t.area && t.area.row) || 0)
    }
    const cols = maxCol + 1
    const rows = maxRow + 1
    const cells = []
    for (let r = 0; r < rows; r++) {
      for (let c = 0; c < cols; c++) {
        const tile = tiles.find((t) => t.area && t.area.col === c && t.area.row === r)
        cells.push(tile ? { element: tile.widget } : { element: '' })
      }
    }
    return { id: s.id, elements, layout: { rows, cols, cells } }
  })
  return { midl: MIDL_VERSION, screens }
}

// Translate a MIDL config document back into a v2 dashboard object.
function midlToV2(doc) {
  const widgets = { items: {} }
  const screens = (doc.screens || []).map((s) => {
    const tiles = []
    const cols = (s.layout && s.layout.cols) || 1
    const cells = (s.layout && s.layout.cells) || []
    cells.forEach((cell, i) => {
      if (!cell || !cell.element) return
      const el = s.elements[cell.element]
      if (!el) return
      widgets.items[cell.element] = {
        type: MIDL_TO_V2[el.type] || 'numeric',
        path: el.bindings && el.bindings.value ? el.bindings.value.path : undefined,
        unit: el.format ? el.format.unit : undefined,
        title: el.name,
      }
      tiles.push({ widget: cell.element, area: { col: i % cols, row: Math.floor(i / cols) } })
    })
    return { id: s.id, type: 'grid', tiles }
  })
  return { widgets, layout: { screens } }
}

module.exports = { v2ToMidl, midlToV2, V2_TO_MIDL, MIDL_TO_V2 }
```

- [ ] **Step 4: Run, confirm PASS**

Run: `cd /Users/borissorochkin/code/embedded/signalk-espdisp-manager && node --test test/midl-adapter.test.js`
Expected: 3 tests pass.

- [ ] **Step 5: Cross-check — a translated v2 preset validates as MIDL**

Add to `<manager>/test/midl-adapter.test.js` (append):

```javascript
test('a translated v2 dashboard validates against the square-480 manifest', () => {
  const { validateMidl } = require('../lib/midl')
  const doc = JSON.stringify(v2ToMidl(v2))
  const r = validateMidl(doc, 'square-480')
  assert.strictEqual(r.ok, true, JSON.stringify(r.issues))
})
```

Run again: `node --test test/midl-adapter.test.js` — expected 4 tests pass. (Note: `single-value` is in the square-480 manifest's elements; the sample uses only `numeric`→`single-value`, so it validates.)

- [ ] **Step 6: Run the manager's full unit suite to confirm no regressions**

Run: `cd /Users/borissorochkin/code/embedded/signalk-espdisp-manager && npm test`
Expected: existing tests still pass + the new ones.

- [ ] **Step 7: Commit**

```bash
cd /Users/borissorochkin/code/embedded/signalk-espdisp-manager
git add lib/midl-adapter.js test/midl-adapter.test.js
git commit -m "feat(midl): bidirectional v2<->MIDL adapter (slice 1.2)"
```

---

## Slice 1 done criteria

- Manager includes the MIDL submodule; `npm run midl:build` produces the CJS validator.
- `lib/midl.js` validates a MIDL doc against a generated class manifest; `lib/midl-adapter.js` converts v2 ↔ MIDL and a translated v2 preset validates clean.
- `npm test` is green (existing + new). No existing behavior changed (all additive).

## After Slice 1 (checkpoint)

Stop and review. Slice 2 (server validate-before-post) is the first slice that affects the live request path; specify and execute it only after Slice 1 is reviewed. Slices 4–5 must be coordinated with Plan 6 (firmware accepting MIDL) before the device cutover.
