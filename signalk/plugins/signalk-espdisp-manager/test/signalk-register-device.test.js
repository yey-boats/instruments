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
  let postedCommand = ''
  await withServer((req, res) => {
    if (req.url === '/api/state') {
      res.setHeader('content-type', 'application/json')
      res.end(JSON.stringify({
        device: { id: 'espdisp-register-test', build: 'register-build' },
        wifi: { ip: '127.0.0.1' },
        webAuth: { enabled: false },
        manager: { registered: false },
        screen: { id: 'dashboard' },
        display: { width: 480, height: 480 }
      }))
      return
    }
    if (req.url === '/api/cmd' && req.method === 'POST') {
      req.setEncoding('utf8')
      req.on('data', (chunk) => { postedCommand += chunk })
      req.on('end', () => {
        res.setHeader('content-type', 'application/json')
        res.end(JSON.stringify({ ok: true }))
      })
      return
    }
    res.statusCode = 404
    res.end(JSON.stringify({ error: 'not found' }))
  }, async (port) => {
    const { manager } = makeManager({
      deviceWebAuth: { enabled: false },
      network: { mdns: { enabled: false, browser: false, advertiseManager: false } }
    })

    const result = await manager.registerDeviceFromSignalK({
      address: '127.0.0.1',
      port,
      profileId: 'default',
      sendManagerRegister: true,
      managerUrl: 'http://signalk.local:3000/plugins/espdisp-manager'
    })

    assert.strictEqual(result.deviceId, 'espdisp-register-test')
    assert.strictEqual(result.deviceCommand.status, 'sent')
    assert.strictEqual(postedCommand, 'manager-register http://signalk.local:3000/plugins/espdisp-manager')

    const device = manager.getDevice('espdisp-register-test')
    assert.strictEqual(device.claimed, true)
    assert.strictEqual(device.discovery.address, '127.0.0.1')
    assert.strictEqual(device.discovery.port, port)
    assert.strictEqual(device.assignedProfile, 'default')
  })
})()
