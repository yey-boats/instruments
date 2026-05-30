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
assert.strictEqual(discovered.devices[0].auth.required, false)
assert.strictEqual(discovered.devices[0].seenCount, 1)

manager.registerDevice({
  id: 'esp-display-helm',
  name: 'Helm Display',
  role: 'display',
  location: 'helm',
  display: { width: 480, height: 480, shape: 'square' }
}, auth)

const registeredDiscovery = manager.listDiscoveredDevices()
assert.strictEqual(registeredDiscovery.devices[0].registered, true)

const claimed = manager.claimDiscoveredDevice('esp-display-helm', {
  profileId: 'default',
  role: 'display',
  location: 'helm'
})
assert.strictEqual(claimed.status, 'updated')

const devices = manager.listDevices({ location: 'helm' })
assert.strictEqual(devices.devices.length, 1)
assert.strictEqual(devices.devices[0].id, 'esp-display-helm')
assert.strictEqual(devices.devices[0].discovery.address, '192.168.50.42')
assert.strictEqual(devices.devices[0].auth.web.mode, 'none')

manager.recordDiscoveredDevice({
  deviceId: 'helm-display-spare',
  address: '192.168.50.42',
  port: 80,
  display: { width: 480, height: 480 }
})
manager.recordDiscoveredDevice({
  deviceId: 'helm-display-spare',
  address: '192.168.50.43',
  port: 80,
  display: { width: 480, height: 480 }
})
manager.recordDiscoveredDevice({
  deviceId: 'helm-display-shadow',
  address: '192.168.50.43',
  port: 80,
  display: { width: 480, height: 480 }
})

const conflicts = manager.listDiscoveredDevices().devices
const spare = conflicts.find((d) => d.deviceId === 'helm-display-spare')
const shadow = conflicts.find((d) => d.deviceId === 'helm-display-shadow')
assert.strictEqual(spare.previousAddresses[0], '192.168.50.42')
assert.strictEqual(spare.seenCount, 2)
assert.strictEqual(spare.duplicate, true)
assert.strictEqual(spare.conflict.type, 'address')
assert.deepStrictEqual(spare.conflict.deviceIds, ['helm-display-shadow'])
assert.strictEqual(shadow.duplicate, true)

const identityChange = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' }
}).manager

identityChange.recordDiscoveredDevice({
  deviceId: 'espdisp-device',
  address: '192.168.50.44',
  port: 80,
  display: { width: 480, height: 480 }
})
identityChange.store.discovery.devices['espdisp-device'].lastSeen = new Date(Date.now() - 120000).toISOString()
identityChange.recordDiscoveredDevice({
  deviceId: 'espdisp-90028a2f3728',
  address: '192.168.50.44',
  port: 80,
  display: { width: 480, height: 480 }
})
identityChange.store.discovery.devices['espdisp-90028a2f3728'].lastSeen = new Date(Date.now() - 90000).toISOString()
identityChange.recordDiscoveredDevice({
  deviceId: 'espdisp-28372f8a0290',
  address: '192.168.50.44',
  port: 80,
  display: { width: 480, height: 480 }
})

const migrated = identityChange.listDiscoveredDevices().devices
const current = migrated.find((d) => d.deviceId === 'espdisp-28372f8a0290')
const legacy = migrated.find((d) => d.deviceId === 'espdisp-device')
const rawEfuse = migrated.find((d) => d.deviceId === 'espdisp-90028a2f3728')
assert.deepStrictEqual(current.replacedDeviceIds, ['espdisp-device', 'espdisp-90028a2f3728'])
assert.strictEqual(current.duplicate, false)
assert.strictEqual(current.conflict, null)
assert.strictEqual(legacy.superseded, true)
assert.strictEqual(legacy.supersededBy, 'espdisp-28372f8a0290')
assert.strictEqual(legacy.duplicate, false)
assert.strictEqual(legacy.conflict, null)
assert.strictEqual(rawEfuse.superseded, true)
assert.strictEqual(rawEfuse.duplicate, false)

const staleDuplicates = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' }
}).manager
staleDuplicates.recordDiscoveredDevice({
  deviceId: 'old-name',
  address: '192.168.50.45',
  port: 80
})
staleDuplicates.recordDiscoveredDevice({
  deviceId: 'new-name',
  address: '192.168.50.45',
  port: 80
})
staleDuplicates.store.discovery.devices['old-name'].lastSeen = new Date(Date.now() - 120000).toISOString()
staleDuplicates.store.discovery.devices['new-name'].lastSeen = new Date(Date.now() - 120000).toISOString()
const staleList = staleDuplicates.listDiscoveredDevices().devices
assert.strictEqual(staleList.find((d) => d.deviceId === 'old-name').duplicate, false)
assert.strictEqual(staleList.find((d) => d.deviceId === 'new-name').duplicate, false)
