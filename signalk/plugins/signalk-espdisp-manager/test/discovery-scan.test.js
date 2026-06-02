const assert = require('assert')
const http = require('http')
const { makeManager } = require('./test-utils')

async function withServer (handler, fn) {
  const server = http.createServer(handler)
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve))
  try {
    return await fn(server.address().port)
  } finally {
    await new Promise((resolve) => server.close(resolve))
  }
}

module.exports = (async () => {
  await withServer((req, res) => {
    if (req.url !== '/api/state') {
      res.statusCode = 404
      res.end(JSON.stringify({ error: 'not found' }))
      return
    }
    res.setHeader('content-type', 'application/json')
    res.end(JSON.stringify({
      device: { id: 'espdisp-scan-test', build: 'scan-build' },
      wifi: { ip: '127.0.0.1' },
      webAuth: { enabled: false },
      manager: { registered: false },
      screen: { id: 'dashboard' },
      touch: { mode: 'poll' },
      display: { width: 480, height: 480, rotation: 0 }
    }))
  }, async (port) => {
    const { manager } = makeManager({
      deviceWebAuth: { enabled: false },
      network: { mdns: { enabled: false, browser: false, advertiseManager: false } }
    })

    const scan = await manager.scanForDevices({
      method: 'ip',
      target: '127.0.0.1',
      ports: String(port),
      timeoutMs: 500
    })

    assert.strictEqual(scan.status, 'ok')
    assert.strictEqual(scan.found, 1)
    assert.strictEqual(scan.devices[0].deviceId, 'espdisp-scan-test')
    assert.strictEqual(scan.devices[0].source, 'ip-scan')
    assert.strictEqual(scan.devices[0].address, '127.0.0.1')
    assert.strictEqual(scan.devices[0].port, port)
    assert.strictEqual(scan.devices[0].display.width, 480)

    const listed = manager.listDiscoveredDevices().devices
    assert.strictEqual(listed.length, 1)
    assert.strictEqual(listed[0].deviceId, 'espdisp-scan-test')
    assert.strictEqual(listed[0].capabilities.web, true)
  })

  const { manager } = makeManager({
    network: { mdns: { enabled: false, browser: false, advertiseManager: false } }
  })
  const ble = await manager.scanForDevices({ method: 'ble' })
  assert.strictEqual(ble.status, 'unsupported')
  assert.strictEqual(ble.found, 0)
})()
