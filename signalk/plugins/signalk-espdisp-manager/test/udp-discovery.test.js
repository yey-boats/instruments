const assert = require('assert')
const dgram = require('dgram')
const fs = require('fs')
const os = require('os')
const path = require('path')
const { EspDispManager } = require('../lib/manager')

function once (emitter, event) {
  return new Promise((resolve, reject) => {
    emitter.once(event, resolve)
    emitter.once('error', reject)
  })
}

module.exports = (async () => {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'espdisp-manager-udp-'))
  const app = {
    getDataDirPath: () => dataDir,
    debug: () => {}
  }
  const manager = new EspDispManager(app, {
    serverId: 'test-sk',
    signalk: { host: 'auto', port: 3000 },
    discoveryUdp: { enabled: true, bind: '127.0.0.1', port: 0 },
    deviceDiscoveryUdp: { enabled: false },
    network: { mdns: { browser: false, advertiseManager: false } }
  })
  await once(manager.discoverySocket, 'listening')
  const port = manager.discoverySocket.address().port

  const client = dgram.createSocket('udp4')
  const reply = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('UDP discovery timeout')), 1000)
    client.once('message', (msg) => {
      clearTimeout(timer)
      resolve(JSON.parse(msg.toString('utf8')))
    })
    client.send(Buffer.from('espdisp.signalk.discover.v1'), port, '127.0.0.1')
  })
  client.close()
  manager.close()

  assert.strictEqual(reply.protocol, 'espdisp.signalk.discovery.v1')
  assert.strictEqual(reply.serverId, 'test-sk')
  assert.strictEqual(reply.host, 'auto')
  assert.strictEqual(reply.port, 3000)
  assert.strictEqual(reply.manager.basePath, '/plugins/espdisp-manager')
})()
