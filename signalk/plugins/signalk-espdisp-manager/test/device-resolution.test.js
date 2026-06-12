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
