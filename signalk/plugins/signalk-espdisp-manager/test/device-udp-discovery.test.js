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
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'espdisp-manager-device-udp-'))
  const app = {
    getDataDirPath: () => dataDir,
    debug: () => {}
  }
  const manager = new EspDispManager(app, {
    discoveryUdp: { enabled: false },
    deviceDiscoveryUdp: { enabled: true, bind: '127.0.0.1', port: 0 },
    network: { mdns: { browser: false, advertiseManager: false } }
  })
  await once(manager.deviceDiscoverySocket, 'listening')
  const port = manager.deviceDiscoverySocket.address().port

  const client = dgram.createSocket('udp4')
  client.send(Buffer.from(JSON.stringify({
    protocol: 'espdisp.device.announce.v1',
    deviceId: 'espdisp-udp-test',
    address: '192.168.50.44',
    port: 80,
    authRequired: true,
    device: { id: 'espdisp-udp-test', board: 'native_fake' },
    firmware: { name: 'espdisp', version: '0.5.0-test' },
    display: { width: 480, height: 480 }
  })), port, '127.0.0.1')

  await new Promise((resolve) => setTimeout(resolve, 50))
  client.close()
  manager.close()

  const discovered = manager.listDiscoveredDevices().devices
  assert.strictEqual(discovered.length, 1)
  assert.strictEqual(discovered[0].deviceId, 'espdisp-udp-test')
  assert.strictEqual(discovered[0].address, '192.168.50.44')
  assert.strictEqual(discovered[0].authRequired, true)
  assert.strictEqual(discovered[0].auth.mode, 'basic')
  assert.strictEqual(discovered[0].board, 'native_fake')
  assert.strictEqual(discovered[0].firmware.version, '0.5.0-test')
  assert.strictEqual(discovered[0].display.width, 480)
})()
