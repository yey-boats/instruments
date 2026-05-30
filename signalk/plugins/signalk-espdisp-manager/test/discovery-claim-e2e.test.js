const assert = require('assert')
const { makeManager } = require('./test-utils')
const { MockFirmware } = require('./mock-firmware')

const { manager, auth } = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' },
  network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' }
})

const helmPreset = manager.upsertProfile({
  id: 'helm-managed',
  name: 'Helm Managed',
  version: 1,
  config: {
    settings: { defaultScreen: 'dashboard', theme: 'day', brightness: 0.9 },
    widgets: {
      defaults: { fontSize: 18, labelFontSize: 12, valueFontSize: 50, unitFontSize: 16 },
      items: {
        sog: { type: 'numeric', title: 'SOG', path: 'navigation.speedOverGround', unit: 'kn', valueFontSize: 50 },
        ap: { type: 'autopilot', title: 'Pilot', path: 'steering.autopilot.state', requires: { capability: 'autopilotControls' } },
        mapPreview: { type: 'map', title: 'Map', path: 'navigation.position' }
      },
      variants: [
        {
          id: 'wide-800x480',
          match: { display: { width: 800, height: 480 } },
          defaults: { valueFontSize: 56 }
        },
        {
          id: 'square-480',
          match: { display: { width: 480, height: 480 } },
          defaults: { valueFontSize: 42 }
        }
      ]
    },
    layout: {
      variants: [
        {
          id: 'wide-800x480',
          match: { display: { width: 800, height: 480 } },
          screens: [
            { id: 'dashboard', type: 'grid', tiles: [{ widget: 'sog' }, { widget: 'ap' }, { widget: 'mapPreview' }] }
          ]
        },
        {
          id: 'square-480',
          match: { display: { width: 480, height: 480 } },
          screens: [
            { id: 'dashboard', type: 'grid', tiles: [{ widget: 'sog' }, { widget: 'ap' }] }
          ]
        }
      ]
    }
  }
})
assert.strictEqual(helmPreset.id, 'helm-managed')

const nightPreset = manager.upsertProfile({
  id: 'night-managed',
  name: 'Night Managed',
  version: 1,
  config: {
    settings: { defaultScreen: 'wind', theme: 'night', brightness: 0.25 },
    widgets: {
      defaults: { fontSize: 14, labelFontSize: 10, valueFontSize: 34, unitFontSize: 12 },
      items: {
        sog: { type: 'numeric', title: 'SOG', path: 'navigation.speedOverGround', unit: 'kn', valueFontSize: 34 }
      }
    },
    layout: {
      screens: [
        { id: 'wind', type: 'grid', tiles: [{ widget: 'sog' }] }
      ]
    }
  }
})
assert.strictEqual(nightPreset.id, 'night-managed')

const firmware = new MockFirmware(manager, {
  deviceId: 'espdisp-e2e-wide',
  auth,
  display: {
    width: 800,
    height: 480,
    rotation: 0,
    colorDepth: 16,
    density: 'mdpi',
    shape: 'wide'
  }
})

const discovered = manager.announceDiscoveredDevice({
  device: {
    ...firmware.identity(),
    address: '192.168.50.42',
    port: 80,
    services: [
      { type: '_espdisp._tcp', port: 80 },
      { type: '_arduino._tcp', port: 3232 }
    ]
  }
}, auth)
assert.strictEqual(discovered.status, 'discovered')
assert.strictEqual(discovered.registered, false)

const claimed = manager.claimDiscoveredDevice(firmware.deviceId, {
  profileId: 'helm-managed',
  role: 'helm',
  location: 'cockpit',
  sendReload: true
})
assert.strictEqual(claimed.status, 'claimed')
assert.strictEqual(claimed.assignedProfile, 'helm-managed')
assert.strictEqual(claimed.command.type, 'config.reload')
assert.strictEqual(manager.listDiscoveredDevices().devices[0].registered, true)
const claimedRecord = manager.getDevice(firmware.deviceId)
assert.strictEqual(claimedRecord.claim.source, 'discovery')
assert.strictEqual(claimedRecord.discovery.address, '192.168.50.42')
assert.strictEqual(claimedRecord.auth.manager.mode, 'dev-shared-token')
assert.strictEqual(claimedRecord.auth.web.username, 'espdisp')

const registration = firmware.register()
assert.strictEqual(registration.status, 'updated')
assert.strictEqual(registration.assignedProfile, 'helm-managed')
assert.strictEqual(manager.getDevice(firmware.deviceId).claim.source, 'discovery')

const reloadResults = firmware.pollAndExecute()
assert.strictEqual(reloadResults.length, 1)
assert.strictEqual(reloadResults[0].command.type, 'config.reload')
assert.strictEqual(firmware.config.profile, 'helm-managed')
assert.strictEqual(firmware.config.display.selectedVariant, 'wide-800x480')
assert.strictEqual(firmware.config.widgets.variant, 'wide-800x480')
assert.strictEqual(firmware.config.widgets.items.mapPreview, undefined)
assert.strictEqual(firmware.config.layout.screens[0].tiles.length, 2)
assert.strictEqual(firmware.config.widgets.defaults.valueFontSize, 56)

let heartbeat = firmware.heartbeat()
assert.strictEqual(heartbeat.status, 'ok')
assert.strictEqual(heartbeat.desiredConfig.reload, false)
assert.strictEqual(manager.dashboard().counts.configDrift, 0)

const apply = manager.applyProfile('night-managed', {
  deviceIds: [firmware.deviceId],
  clearOverrides: true,
  sendReload: true
})
assert.strictEqual(apply.count, 1)
assert.strictEqual(apply.results[0].assignedProfile, 'night-managed')
assert.strictEqual(apply.results[0].command.type, 'config.reload')

const nightReload = firmware.pollAndExecute()
assert.strictEqual(nightReload.length, 1)
assert.strictEqual(firmware.config.profile, 'night-managed')
assert.strictEqual(firmware.config.settings.theme, 'night')
assert.strictEqual(firmware.config.layout.screens[0].id, 'wind')

heartbeat = firmware.heartbeat()
assert.strictEqual(heartbeat.status, 'ok')
assert.strictEqual(heartbeat.desiredConfig.reload, false)
assert.strictEqual(manager.dashboard().counts.configDrift, 0)
