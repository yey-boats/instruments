const assert = require('assert')
const { makeManager } = require('./test-utils')

const { manager, auth } = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' }
})

const announcement = manager.announceDiscoveredDevice({
  device: {
    id: 'ESP Display Helm',
    name: 'Helm Display',
    role: 'display',
    location: 'helm',
    address: '192.168.50.42',
    port: 80,
    display: { width: 480, height: 480, shape: 'square' },
    firmware: { version: '0.2.0-alpha' },
    services: [
      { type: '_espdisp._tcp', port: 80 },
      { type: '_arduino._tcp', port: 3232 }
    ]
  }
}, auth)

assert.strictEqual(announcement.status, 'discovered')
assert.strictEqual(announcement.device.deviceId, 'esp-display-helm')
assert.strictEqual(announcement.registered, false)

const discovered = manager.listDiscoveredDevices()
assert.strictEqual(discovered.devices.length, 1)
assert.strictEqual(discovered.devices[0].address, '192.168.50.42')
assert.strictEqual(discovered.devices[0].registered, false)
assert.strictEqual(discovered.devices[0].stale, false)

manager.registerDevice({
  id: 'esp-display-helm',
  name: 'Helm Display',
  role: 'display',
  location: 'helm',
  display: { width: 480, height: 480, shape: 'square' }
}, auth)

const registeredDiscovery = manager.listDiscoveredDevices()
assert.strictEqual(registeredDiscovery.devices[0].registered, true)

const devices = manager.listDevices({ location: 'helm' })
assert.strictEqual(devices.devices.length, 1)
assert.strictEqual(devices.devices[0].id, 'esp-display-helm')
