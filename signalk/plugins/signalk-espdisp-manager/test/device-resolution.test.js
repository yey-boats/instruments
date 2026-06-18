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
  assert.throws(() => mgr().deviceHttpCandidates({}), (err) => {
    assert.strictEqual(err.status, 409)
    assert.strictEqual(err.payload.error.code, 'device_address_unknown')
    return true
  })
})

test('fetchDeviceJson falls through to the next candidate on failure', async () => {
  const m = mgr()
  m.deviceWebAuth = () => null
  const attempted = []
  // Stub the low-level getter: first URL (stale IP) throws timeout-like,
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

test('hostnameConflict ignores stale/offline peers, flags only live ones', () => {
  const m = mgr()
  m.options = { heartbeatMs: 5000 }
  const fresh = new Date().toISOString()
  const ancient = new Date(Date.now() - 86400000).toISOString() // 1 day ago
  m.store = {
    registry: {
      devices: {
        live: { id: 'live', lastSeen: fresh, networkIdentity: { desiredHostname: 'espdisp-boat' } },
        // a stale duplicate (mock/old registration) wanting the same hostname
        stale: { id: 'stale', lastSeen: ancient, networkIdentity: { desiredHostname: 'espdisp-boat' } },
        never: { id: 'never', networkIdentity: { desiredHostname: 'espdisp-boat' } }
      }
    }
  }
  // The live device must NOT conflict against stale/never-seen duplicates.
  assert.strictEqual(m.hostnameConflict('live', 'espdisp-boat'), false)
  // A peer that is itself online DOES count as a real conflict.
  m.store.registry.devices.other = { id: 'other', lastSeen: fresh, networkIdentity: { desiredHostname: 'espdisp-boat' } }
  assert.strictEqual(m.hostnameConflict('live', 'espdisp-boat'), true)
  // deviceHealth surfaces a live, unflagged device as ok (no conflict).
  assert.strictEqual(m.deviceHealth({ networkIdentity: { conflict: false } }, true, 0), 'ok')
})

test('deviceCapabilities returns the reported manifest, else null', () => {
  const m = mgr()
  const manifest = { version: 1, maxViews: 8, viewTypes: { numeric: {} } }
  m.store = {
    registry: {
      devices: {
        live: { id: 'live', status: { ui: { capabilities: manifest } } },
        bare: { id: 'bare', status: { ui: { screen: 'nav' } } },
        cold: { id: 'cold' }
      }
    }
  }
  assert.deepStrictEqual(m.deviceCapabilities('live'), manifest)
  assert.strictEqual(m.deviceCapabilities('bare'), null) // reported status, no manifest
  assert.strictEqual(m.deviceCapabilities('cold'), null) // never reported
})

test('deleteOfflineDevices removes only stale entries; clearAllDevices empties the list', () => {
  const m = mgr()
  m.options = { heartbeatMs: 5000 }
  const fresh = new Date().toISOString()
  const old = new Date(Date.now() - 86400000).toISOString()
  const noop = () => {}
  m.store = {
    registry: {
      devices: {
        live: { id: 'live', lastSeen: fresh },
        stale1: { id: 'stale1', lastSeen: old },
        stale2: { id: 'stale2' } // never seen
      }
    },
    commands: { queues: {} },
    discovery: { devices: {} },
    saveRegistry: noop, saveCommands: noop, saveDiscovery: noop, audit: noop
  }
  const r = m.deleteOfflineDevices()
  assert.strictEqual(r.removed, 2)
  assert.deepStrictEqual(Object.keys(m.store.registry.devices), ['live']) // online kept
  m.store.registry.devices.x = { id: 'x', lastSeen: fresh }
  const r2 = m.clearAllDevices()
  assert.strictEqual(r2.removed, 2)
  assert.strictEqual(Object.keys(m.store.registry.devices).length, 0)
})

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
