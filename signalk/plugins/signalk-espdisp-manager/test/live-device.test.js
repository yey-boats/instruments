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

function registerLiveDevice (manager, auth, port, id = 'espdisp-live') {
  manager.registerDevice({ device: { id, name: 'Live' } }, auth)
  manager.updateStatus(id, {
    network: { ip: '127.0.0.1', hostname: id, domain: 'local' },
    config: { hash: 'old' }
  }, auth)
  // fetchDeviceJson resolves through deviceHttpCandidates now; override it so
  // the fake server's dynamic port is used instead of the hardcoded :80.
  manager.deviceHttpCandidates = () => [`http://127.0.0.1:${port}`]
  manager.deviceHttpBase = () => `http://127.0.0.1:${port}`
  return id
}

module.exports = (async () => {
  await withServer((req, res) => {
    const expectedAuth = `Basic ${Buffer.from('espdisp:secret').toString('base64')}`
    assert.strictEqual(req.headers.authorization, expectedAuth)
    res.setHeader('content-type', 'application/json')
    if (req.url === '/api/state') {
      res.end(JSON.stringify({ wifi: { ip: '127.0.0.1' }, sk: { state: 'live' } }))
      return
    }
    if (req.url === '/api/logs?since=7') {
      res.end(JSON.stringify({ entries: [{ seq: 8, line: 'hello' }], lastSeq: 8 }))
      return
    }
    res.statusCode = 404
    res.end(JSON.stringify({ error: 'not found' }))
  }, async (port) => {
    const { manager, auth } = makeManager({
      auth: { mode: 'dev-shared-token', devToken: 'test-token' },
      deviceWebAuth: { enabled: true, username: 'espdisp', password: 'secret' }
    })
    const id = registerLiveDevice(manager, auth, port)

    const status = await manager.getLiveStatus(id)
    assert.strictEqual(status.sk.state, 'live')
    const logs = await manager.getLiveLogs(id, 7)
    assert.strictEqual(logs.entries[0].line, 'hello')
  })

  await withServer((req, res) => {
    assert.strictEqual(req.headers.authorization, undefined)
    res.setHeader('content-type', 'application/json')
    res.end(JSON.stringify({ sk: { state: 'open' } }))
  }, async (port) => {
    const { manager, auth } = makeManager({
      auth: { mode: 'dev-shared-token', devToken: 'test-token' },
      deviceWebAuth: { enabled: false, username: 'espdisp', password: 'secret' }
    })
    const id = registerLiveDevice(manager, auth, port, 'espdisp-open')

    const status = await manager.getLiveStatus(id)
    assert.strictEqual(status.sk.state, 'open')
  })

  await withServer((req, res) => {
    res.statusCode = 401
    res.setHeader('content-type', 'application/json')
    res.end(JSON.stringify({ error: 'auth required' }))
  }, async (port) => {
    const { manager, auth } = makeManager({
      auth: { mode: 'dev-shared-token', devToken: 'test-token' },
      deviceWebAuth: { enabled: true, username: 'espdisp', password: 'wrong' }
    })
    const id = registerLiveDevice(manager, auth, port, 'espdisp-unauthorized')

    await assert.rejects(
      () => manager.getLiveStatus(id),
      (err) => err.status === 401 &&
        err.payload &&
        err.payload.error &&
        err.payload.error.code === 'device_http_error'
    )
  })
})()
