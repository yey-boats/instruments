const assert = require('assert')
const http = require('http')
const { ProtoControl } = require('../lib/proto-control')

// We validate outbound + inbound messages with the SAME shared lib the
// production code uses (@espdisp/proto, an ES module).
let proto
async function lib () {
  if (!proto) proto = await import('@espdisp/proto')
  return proto
}

// A minimal mock target implementing the protocol's IP endpoints:
//   GET  /api/p2p/device  -> DeviceRecord
//   POST /api/p2p/attach  -> AttachAck (validates the inbound Attach)
//   POST /api/p2p/switch  -> SwitchAck (validates the inbound Switch)
//   POST /api/p2p/detach  -> 200       (validates the inbound Detach)
// It records every request so the test can assert the attach->switch->detach
// ordering and the controller's color/id were carried.
function startMockTarget (opts) {
  opts = opts || {}
  const calls = []
  const inbound = []
  const server = http.createServer((req, res) => {
    let body = ''
    req.on('data', (c) => { body += c })
    req.on('end', async () => {
      const { validate } = await lib()
      const msg = body ? JSON.parse(body) : null
      calls.push(req.url)
      if (msg) inbound.push({ url: req.url, msg })
      const send = (obj, code = 200) => {
        res.writeHead(code, { 'content-type': 'application/json' })
        res.end(JSON.stringify(obj))
      }
      if (req.method === 'GET' && req.url === '/api/p2p/device') {
        return send(opts.deviceRecord || DEVICE_RECORD)
      }
      if (req.method === 'POST' && req.url === '/api/p2p/attach') {
        // The mock asserts the controller sent a schema-valid Attach.
        if (!validate.Attach(msg)) return send({ error: 'bad attach' }, 400)
        return send({
          v: '1.0', t: 'attachAck', accepted: true,
          sessionId: 's-mock-1', ttlMs: 10000,
          device: opts.deviceRecord || DEVICE_RECORD
        })
      }
      if (req.method === 'POST' && req.url === '/api/p2p/switch') {
        if (!validate.Switch(msg)) return send({ error: 'bad switch' }, 400)
        return send({ v: '1.0', t: 'switchAck', ok: true, currentView: msg.viewId })
      }
      if (req.method === 'POST' && req.url === '/api/p2p/detach') {
        if (!validate.Detach(msg)) return send({ error: 'bad detach' }, 400)
        return send({ ok: true })
      }
      send({ error: 'not found' }, 404)
    })
  })
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address()
      resolve({ server, port, base: `http://127.0.0.1:${port}`, calls, inbound })
    })
  })
}

const DEVICE_RECORD = {
  v: '1.0',
  deviceId: 'mfd-helm',
  name: 'Helm MFD',
  role: 'both',
  board: 'sunton_4848s040',
  display: '480x480',
  currentView: 'wind',
  views: [
    { id: 'wind', title: 'Wind' },
    { id: 'nav', title: 'Nav' }
  ],
  transports: ['ip', 'ble'],
  authRequired: false
}

module.exports = (async () => {
  const { validate } = await lib()

  // (a) Outbound Attach / Switch / Detach validate against the schema. We
  //     reach the private builders so we assert the exact bytes that go on the
  //     wire, independent of any server.
  {
    const pc = new ProtoControl({
      controllerId: 'plugin:espdisp-manager',
      name: 'SignalK Manager',
      color: '#ff9800',
      key: 'hunter2'
    })
    const attach = pc._attachMessage()
    assert.ok(validate.Attach(attach), 'outbound Attach validates against schema')
    assert.strictEqual(attach.controllerId, 'plugin:espdisp-manager')
    assert.strictEqual(attach.color, '#ff9800')
    assert.strictEqual(attach.key, 'hunter2', 'shared key carried when configured')

    const sw = { v: '1.0', t: 'switch', sessionId: 's-1', viewId: 'nav' }
    assert.ok(validate.Switch(sw), 'outbound Switch validates against schema')
    const det = { v: '1.0', t: 'detach', sessionId: 's-1' }
    assert.ok(validate.Detach(det), 'outbound Detach validates against schema')
  }

  // No key configured -> the Attach omits `key` (still valid; open auth).
  {
    const pc = new ProtoControl({ color: '#00bcd4' })
    const attach = pc._attachMessage()
    assert.ok(validate.Attach(attach), 'keyless Attach validates')
    assert.ok(!('key' in attach), 'no key field when none configured')
  }

  // (b) setScreen performs attach -> switch -> detach end to end against a
  //     real mock HTTP target and returns success.
  {
    const target = await startMockTarget()
    const pc = new ProtoControl({
      controllerId: 'plugin:espdisp-manager',
      name: 'Manager',
      color: '#ff9800'
    })
    const result = await pc.setScreen({ base: target.base }, 'nav')
    target.server.close()

    assert.strictEqual(result.ok, true, 'setScreen succeeds end-to-end')
    assert.strictEqual(result.currentView, 'nav', 'target switched to requested view')
    assert.strictEqual(result.detached, true, 'session was detached')

    // Ordering: attach, then switch, then detach.
    assert.deepStrictEqual(
      target.calls,
      ['/api/p2p/attach', '/api/p2p/switch', '/api/p2p/detach'],
      'attach -> switch -> detach ordering on the wire'
    )
    // The plugin's color/id were carried in the Attach the target received.
    const attachMsg = target.inbound.find((c) => c.url === '/api/p2p/attach').msg
    assert.strictEqual(attachMsg.color, '#ff9800', 'plugin color carried to target frame')
    assert.strictEqual(attachMsg.controllerId, 'plugin:espdisp-manager')
    const switchMsg = target.inbound.find((c) => c.url === '/api/p2p/switch').msg
    assert.strictEqual(switchMsg.sessionId, 's-mock-1', 'switch reuses the attach sessionId')
    assert.strictEqual(switchMsg.viewId, 'nav')
  }

  // (b2) describeDevice fetches + validates the DeviceRecord and surfaces
  //      pv/transports.
  {
    const target = await startMockTarget()
    const pc = new ProtoControl({})
    const desc = await pc.describeDevice({ base: target.base })
    target.server.close()
    assert.strictEqual(desc.ok, true, 'describeDevice ok for a compatible target')
    assert.strictEqual(desc.pv, '1.0')
    assert.deepStrictEqual(desc.transports, ['ip', 'ble'])
    assert.strictEqual(desc.record.deviceId, 'mfd-helm')
  }

  // (c) A version-incompatible DeviceRecord (major 2) is filtered out by
  //     discover(): describeDevice flags it and discover() drops it.
  {
    const incompatible = { ...DEVICE_RECORD, v: '2.0' }
    const target = await startMockTarget({ deviceRecord: incompatible })
    const pc = new ProtoControl({})

    const desc = await pc.describeDevice({ base: target.base })
    assert.strictEqual(desc.ok, false, 'incompatible record is not ok')
    assert.strictEqual(desc.reason, 'incompatible_version')

    const found = await pc.discover([{ base: target.base, deviceId: 'mfd-helm' }])
    target.server.close()
    assert.strictEqual(found.length, 0, 'discover() filters out the v2 device')
  }

  // (c2) A compatible device IS surfaced by discover().
  {
    const target = await startMockTarget()
    const pc = new ProtoControl({})
    const found = await pc.discover([{ base: target.base, deviceId: 'mfd-helm' }])
    target.server.close()
    assert.strictEqual(found.length, 1, 'discover() keeps the v1 device')
    assert.strictEqual(found[0].pv, '1.0')
  }

  // speaksProtocol: registry annotation gating.
  assert.strictEqual(
    ProtoControl.speaksProtocol({ proto: { pv: '1.0', transports: ['ip'] } }), true,
    'device with proto.pv + ip transport speaks the protocol')
  assert.strictEqual(
    ProtoControl.speaksProtocol({ proto: { pv: '1.0', transports: ['ble'] } }), false,
    'ble-only device does not speak the IP protocol path')
  assert.strictEqual(ProtoControl.speaksProtocol({}), false, 'plain registry device does not')

  console.log('proto-control test passed')
})()
