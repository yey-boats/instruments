const assert = require('assert')
const { makeManager } = require('./test-utils')
const { MockFirmware } = require('./mock-firmware')

const { manager, auth } = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' },
  network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' }
})

const firmware = new MockFirmware(manager, {
  deviceId: 'espdisp-mockfw',
  auth,
  version: '0.5.0-dev'
})

const registration = firmware.register()
assert.strictEqual(registration.status, 'registered')
assert.strictEqual(registration.deviceId, 'espdisp-mockfw')

const config = firmware.fetchConfig()
assert.strictEqual(config.deviceId, 'espdisp-mockfw')
assert.strictEqual(config.hash, firmware.fetchConfig().hash)

const heartbeat = firmware.heartbeat()
assert.strictEqual(heartbeat.status, 'ok')
assert.strictEqual(heartbeat.desiredConfig.reload, false)

manager.createCommand(firmware.deviceId, {
  type: 'theme.set',
  payload: { theme: 'night' }
})
manager.createCommand(firmware.deviceId, {
  type: 'screen.set',
  payload: { screen: 'autopilot' }
})

const results = firmware.pollAndExecute()
assert.strictEqual(results.length, 2)
assert.strictEqual(firmware.theme, 'night')
assert.strictEqual(firmware.screen, 'autopilot')

const device = manager.getDevice(firmware.deviceId)
assert.strictEqual(device.status.ui.theme, 'day')
firmware.heartbeat()
assert.strictEqual(manager.getDevice(firmware.deviceId).status.ui.theme, 'night')

const artifact = manager.addFirmwareArtifact({
  vendor: { id: 'navado', name: 'Navado' },
  product: { id: 'espdisp', name: 'ESP Display' },
  firmware: { name: 'espdisp', version: '0.5.1', channel: 'dev' },
  compatibility: { boards: ['sunton_4848s040'], chip: 'ESP32-S3' },
  file: {
    name: 'espdisp-0.5.1-esp32-4848s040.bin',
    size: 2048,
    sha256: 'sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd'
  }
})
const job = manager.createFirmwareJob(firmware.deviceId, { artifactId: artifact.artifactId })
firmware.pollAndExecute()
assert.strictEqual(manager.getFirmwareJob(firmware.deviceId, job.jobId).status, 'confirmed')
assert.strictEqual(firmware.firmware.version, '0.5.1')
