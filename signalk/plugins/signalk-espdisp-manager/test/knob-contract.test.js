const assert = require('assert')
const { makeManager } = require('./test-utils')
const makePlugin = require('../index')

// Knob remote-display wire contract.
//
// This test PINS the exact JSON shapes the ESP32 firmware parser depends on
// for the Waveshare knob's remote enumeration + view-switch feature, so that a
// future plugin change which renames/removes a key the firmware reads fails in
// CI instead of silently breaking the device.
//
// The consumer is src/manager.cpp (BOARD_ID_WAVESHARE_KNOB_1_8):
//
//   knob_refresh_displays()  GET /devices/summary
//     JsonArray devices = r["devices"];          // top-level "devices" array
//     for (JsonObject d : devices) {
//       const char *id   = d["id"]   | "";        // <- REQUIRED string
//       if (own && strcmp(own, id)==0) continue;  //    (skips own device id)
//       const char *name = d["name"] | id;        // <- REQUIRED string
//       const char *cur  = d["currentScreen"]|""; // <- REQUIRED (string|null)
//     }
//   NOTE: the firmware does NOT read role/online in this code path. Those keys
//   are tolerated-extra (see ArduinoJson note below) but are NOT part of the
//   pinned contract, so we don't assert the firmware needs them.
//
//   knob_fetch_views(dev_idx) GET /devices/:id/views
//     JsonArray views = r["views"];              // top-level "views" array
//     const char *cur = r["current"] | "";       // <- "current" read as STRING
//     for (JsonObject v : views) {
//       const char *vid   = v["id"]   | "";        // <- REQUIRED string
//       const char *title = v["title"]| vid;       // <- REQUIRED string
//     }
//
// ArduinoJson tolerates EXTRA keys: the firmware only reads the named fields,
// so the plugin ADDING fields (e.g. role/online/location) is always safe. The
// breaking change this test guards against is RENAMING or REMOVING any of the
// listed keys, or changing their JSON type out from under the parser.

// ---------------------------------------------------------------------------
// Seed a fake registry: a device WITH a stored screen list (resolves a real
// layout -> non-fallback views), a device WITHOUT one (-> fallback view list),
// and the local/own device (the knob skips its own id in the summary loop).
// ---------------------------------------------------------------------------

const { manager, auth } = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' }
})

const ownId = 'espdisp-knob00000001' // the knob's own id (firmware skips this)
const withScreensId = 'espdisp-helm00000002' // device with a resolved layout
const noScreensId = 'espdisp-aux000000003' // device that falls back

manager.registerDevice({
  device: {
    id: ownId,
    name: 'Knob',
    role: 'controller',
    board: 'waveshare_knob_1_8'
  }
}, auth)

manager.registerDevice({
  device: {
    id: withScreensId,
    name: 'Helm Display',
    role: 'display',
    board: 'sunton_4848s040',
    display: { width: 480, height: 480, shape: 'square' }
  }
}, auth)

manager.registerDevice({
  device: {
    id: noScreensId,
    name: 'Aux Display',
    role: 'display',
    board: 'sunton_4848s040'
  }
}, auth)

// Seed the default profile's layout (presets) so the "with screens" device
// resolves a real, non-fallback screen list via deviceViews(). listProfiles()
// is the documented first-inspection seeding trigger.
manager.listProfiles()

// Force the "no screens" device onto a profile with an explicitly empty layout
// so deviceViews() takes the fallback (KNOWN_VIEW_IDS) branch.
manager.upsertProfile({
  id: 'empty-layout',
  name: 'Empty Layout',
  config: { layout: { version: 1, screens: [] } }
})
manager.assignProfile(noScreensId, { profileId: 'empty-layout' })

// A heartbeat carrying ui.screen so currentScreen / current are populated.
// 'dashboard' is one of the resolved preset screens for the helm device.
manager.updateStatus(withScreensId, {
  time: new Date().toISOString(),
  network: { mode: 'sta', ip: '192.168.1.20' },
  ui: { screen: 'dashboard', theme: 'day' }
}, auth)

// ===========================================================================
// 1. Shape / contract assertions (projection helpers)
// ===========================================================================

// --- GET /devices/summary  (deviceSummaries) -------------------------------

const summary = manager.deviceSummaries()
assert.ok(summary && typeof summary === 'object', 'summary is an object')
assert.ok(Array.isArray(summary.devices),
  'CONTRACT: top-level "devices" is an array (firmware: r["devices"].as<JsonArray>())')

const withScreensSummary = summary.devices.find((d) => d.id === withScreensId)
assert.ok(withScreensSummary, 'seeded display appears in summary')

// Every key the firmware reads must be present and the right type.
assert.strictEqual(typeof withScreensSummary.id, 'string',
  'CONTRACT: summary[].id is a string (firmware: d["id"])')
assert.strictEqual(typeof withScreensSummary.name, 'string',
  'CONTRACT: summary[].name is a string (firmware: d["name"])')
// currentScreen: string once a heartbeat with ui.screen arrived.
assert.strictEqual(typeof withScreensSummary.currentScreen, 'string',
  'CONTRACT: summary[].currentScreen is a string (firmware: d["currentScreen"])')
assert.strictEqual(withScreensSummary.currentScreen, 'dashboard',
  'currentScreen reflects the reported ui.screen')

// currentScreen may also be null when the device has not reported a screen.
// The firmware reads it as `d["currentScreen"] | ""` so null is parsed as the
// empty string -> tolerated. Assert the null case is emitted for a fresh device.
const ownSummary = summary.devices.find((d) => d.id === ownId)
assert.ok(ownSummary, 'own device present in summary')
assert.ok(Object.prototype.hasOwnProperty.call(ownSummary, 'currentScreen'),
  'CONTRACT: currentScreen key is always present (string|null)')
assert.strictEqual(ownSummary.currentScreen, null,
  'no heartbeat-screen -> currentScreen null (firmware coalesces to "")')

// No MISSING firmware-required key on any summary element.
for (const d of summary.devices) {
  for (const key of ['id', 'name', 'currentScreen']) {
    assert.ok(Object.prototype.hasOwnProperty.call(d, key),
      `CONTRACT: summary element must contain "${key}" (firmware reads d["${key}"])`)
  }
}

// Own-device skip: the firmware skips entries whose id === its own device id.
// That filtering happens firmware-side; the plugin DOES include the own id in
// the list (the knob is itself a registered device), so confirm it's present
// so the firmware has something to skip against.
assert.ok(summary.devices.some((d) => d.id === ownId),
  'own device id is present in the list for the firmware to skip')

// --- GET /devices/:id/views  (deviceViews) ---------------------------------

// (a) device WITH a resolved layout -> real, non-fallback views.
const helmViews = manager.deviceViews(withScreensId)
assert.ok(helmViews && typeof helmViews === 'object', 'views response is an object')
assert.ok(Array.isArray(helmViews.views),
  'CONTRACT: top-level "views" is an array (firmware: r["views"].as<JsonArray>())')
assert.ok(helmViews.views.length > 0, 'resolved layout yields at least one view')
for (const v of helmViews.views) {
  assert.strictEqual(typeof v.id, 'string',
    'CONTRACT: views[].id is a string (firmware: v["id"])')
  assert.strictEqual(typeof v.title, 'string',
    'CONTRACT: views[].title is a string (firmware: v["title"])')
}
const helmViewIds = helmViews.views.map((v) => v.id)
// Resolved (non-fallback) views come from the seeded default preset layout.
// 'dashboard' is in both the preset list AND the fallback list, so also assert
// a preset-only id ('nav') is present to prove this is the resolved branch.
assert.ok(helmViewIds.includes('dashboard'),
  'resolved views include the preset screen "dashboard"')
assert.ok(helmViewIds.includes('nav'),
  'resolved views include a preset-only screen ("nav"), proving non-fallback branch')

// "current": the firmware reads `r["current"] | ""` (STRING). The plugin emits
// the active screen id as a string, or null when none is known. Both satisfy
// the firmware's string-coalescing read; assert the key is present and is a
// string here (a screen was reported).
assert.ok(Object.prototype.hasOwnProperty.call(helmViews, 'current'),
  'CONTRACT: "current" key is always present (firmware: r["current"])')
assert.strictEqual(typeof helmViews.current, 'string',
  'CONTRACT: "current" is a string screen id when known (firmware: r["current"] | "")')
assert.strictEqual(helmViews.current, 'dashboard', 'current reflects the reported screen')

// (b) device WITHOUT a screen list -> FALLBACK view list (knob menu never empty).
const fallbackViews = manager.deviceViews(noScreensId)
assert.ok(Array.isArray(fallbackViews.views), 'fallback views is an array')
assert.ok(fallbackViews.views.length > 0, 'fallback view list is non-empty')
for (const v of fallbackViews.views) {
  assert.strictEqual(typeof v.id, 'string', 'fallback view id is a string')
  assert.strictEqual(typeof v.title, 'string', 'fallback view title is a string')
}
const fallbackIds = fallbackViews.views.map((v) => v.id)
// Known fallback ids the firmware/menu expect (KNOWN_VIEW_IDS in lib/manager.js).
assert.ok(fallbackIds.includes('dashboard'),
  'CONTRACT: fallback views include the known id "dashboard"')
assert.ok(fallbackIds.includes('autopilot'),
  'CONTRACT: fallback views include the known id "autopilot"')
// "current" present even with no heartbeat-screen (null -> firmware "").
assert.ok(Object.prototype.hasOwnProperty.call(fallbackViews, 'current'),
  'CONTRACT: "current" present on fallback response')
assert.strictEqual(fallbackViews.current, null,
  'no reported screen -> current null (firmware coalesces to "")')

// ===========================================================================
// 2. End-to-end HTTP route test
// ===========================================================================
//
// The plugin has no `express` dependency available in this repo and does not
// export registerRoutes(), but it DOES expose the public `registerWithRouter`
// hook that SignalK calls. We drive that hook with a minimal Express-compatible
// router shim (router.use/get/post/...) that records the registered handlers,
// then dispatch real requests through it with mock req/res objects. This
// exercises the actual route registration + handler bodies in index.js (path
// matching, :id param extraction, res.json) end-to-end against the same routes
// the firmware hits, rather than calling the manager helpers directly.

function makeRouterShim () {
  const routes = [] // { method, segments, handler }
  const middleware = []
  const toSegments = (p) => p.split('/').filter(Boolean)
  const add = (method) => (routePath, ...handlers) => {
    routes.push({ method, segments: toSegments(routePath), handler: handlers[handlers.length - 1] })
  }
  const router = {
    use: (fn) => { middleware.push(fn) },
    get: add('GET'),
    post: add('POST'),
    patch: add('PATCH'),
    put: add('PUT'),
    delete: add('DELETE')
  }
  const match = (method, urlPath) => {
    const parts = toSegments(urlPath)
    for (const route of routes) {
      if (route.method !== method) continue
      if (route.segments.length !== parts.length) continue
      const params = {}
      let ok = true
      for (let i = 0; i < route.segments.length; i++) {
        const seg = route.segments[i]
        if (seg.startsWith(':')) params[seg.slice(1)] = decodeURIComponent(parts[i])
        else if (seg !== parts[i]) { ok = false; break }
      }
      if (ok) return { route, params }
    }
    return null
  }
  // Express-router semantics: literal /devices/summary must win over
  // /devices/:id. The plugin registers summary BEFORE :id routes, and our
  // match() iterates in registration order, so first-match honours that.
  router._dispatch = (method, urlPath) => new Promise((resolve) => {
    const m = match(method, urlPath)
    const res = {
      statusCode: 200,
      _json: undefined,
      status (code) { this.statusCode = code; return this },
      json (body) { this._json = body; resolve({ status: this.statusCode, body }) },
      setHeader () {},
      end () { resolve({ status: this.statusCode, body: this._json }) }
    }
    const req = {
      method,
      url: urlPath,
      params: m ? m.params : {},
      query: {},
      headers: {},
      get: () => '',
      body: {}
    }
    if (!m) { res.status(404).json({ error: { code: 'not_found' } }); return }
    // Run recorded middleware (jsonBody) then the handler, mirroring Express.
    let i = 0
    const next = () => {
      if (i < middleware.length) { const fn = middleware[i++]; fn(req, res, next) } else {
        Promise.resolve(m.route.handler(req, res)).catch((err) => {
          res.status(err.status || 500).json({ error: { message: err.message } })
        })
      }
    }
    next()
  })
  return router
}

const routeTest = (async () => {
  // Stand up the real plugin and point it at the SAME data dir as the seeded
  // `manager` above. The plugin's start() builds its own EspDispManager which
  // loads that on-disk registry, so the three devices we already registered are
  // visible to the real route handlers without re-seeding.
  const plugin = makePlugin({
    getDataDirPath: () => manager.store.dataDir,
    debug: () => {},
    handleMessage: () => {},
    registerPutHandler: () => {}
  })

  const router = makeRouterShim()
  plugin.registerWithRouter(router)
  // The route closures call getManager(), which returns the plugin's manager
  // only after start(). Start it (same data dir) so routes resolve live state.
  plugin.start({
    discoveryUdp: { enabled: false },
    deviceDiscoveryUdp: { enabled: false },
    firmware: { github: { enabled: false } },
    network: { mdns: { browser: false, advertiseManager: false } },
    auth: { mode: 'dev-shared-token', devToken: 'test-token' }
  })

  // --- GET /devices/summary ---
  const summaryResp = await router._dispatch('GET', '/devices/summary')
  assert.strictEqual(summaryResp.status, 200, 'GET /devices/summary -> 200')
  assert.ok(Array.isArray(summaryResp.body.devices),
    'route: summary.devices is an array')
  const sEl = summaryResp.body.devices.find((d) => d.id === withScreensId)
  assert.ok(sEl, 'route summary includes seeded device')
  for (const key of ['id', 'name', 'currentScreen']) {
    assert.ok(Object.prototype.hasOwnProperty.call(sEl, key),
      `route: summary element has firmware-required key "${key}"`)
  }
  assert.strictEqual(typeof sEl.id, 'string', 'route: summary id string')
  assert.strictEqual(typeof sEl.name, 'string', 'route: summary name string')

  // --- GET /devices/:id/views (literal summary route must not shadow this) ---
  const viewsResp = await router._dispatch('GET', `/devices/${withScreensId}/views`)
  assert.strictEqual(viewsResp.status, 200, 'GET /devices/:id/views -> 200')
  assert.ok(Array.isArray(viewsResp.body.views), 'route: views is an array')
  assert.ok(viewsResp.body.views.length > 0, 'route: views non-empty')
  for (const v of viewsResp.body.views) {
    assert.strictEqual(typeof v.id, 'string', 'route: view id string')
    assert.strictEqual(typeof v.title, 'string', 'route: view title string')
  }
  assert.ok(Object.prototype.hasOwnProperty.call(viewsResp.body, 'current'),
    'route: views response has "current"')

  // Confirm /devices/summary is matched as the literal route, not as
  // /devices/:id with id="summary" (that would 200 from getDevice -> 404).
  // Our shim matches in registration order and summary is registered first,
  // matching Express; assert the summary body is the list shape, not a device.
  assert.ok(!summaryResp.body.id,
    '/devices/summary returns the list, not a single device (route ordering)')

  plugin.stop()
})()

routeTest
  .then(() => console.log('knob-contract test passed'))
  .catch((err) => { console.error(err); process.exit(1) })

module.exports = routeTest
