const assert = require('assert')
const { makeManager } = require('./test-utils')

const { manager, auth } = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' },
  network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' }
})

const deviceId = 'espdisp-112233445566'
const registration = manager.registerDevice({
  protocol: 'espdisp.management.v1',
  device: {
    id: deviceId,
    name: 'Future Firmware Contract',
    model: 'ESP32-4848S040',
    board: 'sunton_4848s040',
    mac: '11:22:33:44:55:66',
    chip: 'ESP32-S3',
    flashBytes: 16777216,
    psramBytes: 8388608,
    display: {
      width: 480,
      height: 480,
      rotation: 0,
      colorDepth: 16,
      density: 'mdpi',
      shape: 'square'
    },
    touch: {
      enabled: true,
      width: 480,
      height: 480,
      controller: 'GT911',
      interrupt: true
    },
    firmware: {
      name: 'espdisp',
      version: '0.5.0-dev',
      channel: 'dev',
      buildTime: '2026-05-27T18:00:00Z',
      git: { commit: 'abc1234', dirty: false }
    },
    capabilities: {
      touch: true,
      touchInterrupt: true,
      bleConfig: true,
      ota: true,
      pullOta: true,
      nmea0183Wifi: true,
      autopilotControls: true,
      widgets: {
        numeric: true,
        text: true,
        gauge: true,
        compass: true,
        windRose: true,
        trend: true,
        bar: true,
        button: true,
        autopilot: true,
        map: false
      },
      fonts: {
        scalable: false,
        sizes: [10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 42, 48, 56],
        families: ['default']
      }
    }
  }
}, auth)

assert.strictEqual(registration.deviceId, deviceId)
assert.strictEqual(registration.commands.pollMs, 15000)
assert.strictEqual(registration.heartbeat.intervalMs, 30000)

const discovery = manager.discovery()
assert.strictEqual(discovery.protocol, 'espdisp.management.v1')
assert.strictEqual(discovery.features.commands, true)
assert.strictEqual(discovery.features.firmware, true)

const firstConfig = manager.generateConfig(deviceId)
const secondConfig = manager.generateConfig(deviceId)
assert.strictEqual(firstConfig.hash, secondConfig.hash)
assert.strictEqual(firstConfig.network.hostname, deviceId)
assert.strictEqual(firstConfig.ota.address, `${deviceId}.local`)
assert.strictEqual(firstConfig.autopilot.allowEngage, false)
assert.strictEqual(firstConfig.display.width, 480)
assert.strictEqual(firstConfig.layout.variant, 'square-480')
assert.strictEqual(firstConfig.widgets.variant, 'square-480')
assert.strictEqual(firstConfig.widgets.items.mapPreview, undefined)
assert.deepStrictEqual(firstConfig.webAuth, {
  enabled: true,
  username: 'espdisp',
  password: 'espdisp-dev'
})

const { manager: openWebManager, auth: openWebAuth } = makeManager({
  auth: { mode: 'dev-shared-token', devToken: 'test-token' },
  deviceWebAuth: { enabled: false, username: 'espdisp', password: 'unused' }
})
openWebManager.registerDevice({ device: { id: 'espdisp-open-web', name: 'Open Web' } }, openWebAuth)
assert.deepStrictEqual(openWebManager.generateConfig('espdisp-open-web').webAuth, {
  enabled: false,
  username: 'espdisp',
  password: 'unused'
})

const heartbeat = manager.updateStatus(deviceId, {
  time: '2026-05-27T18:20:00Z',
  uptimeMs: 1234,
  network: {
    mode: 'sta',
    ssid: 'BoatWiFi',
    ip: '192.168.1.42',
    rssi: -55,
    hostname: deviceId,
    domain: 'local',
    fqdn: `${deviceId}.local`,
    mdns: { enabled: true, services: ['_espdisp._tcp', '_arduino._tcp'] }
  },
  signalk: {
    host: 'signalk.local',
    port: 3000,
    connected: true,
    lastDeltaAgeMs: 100
  },
  ui: {
    screen: 'dashboard',
    theme: 'day',
    brightness: 0.8,
    layoutVariant: firstConfig.layout.variant,
    widgetConfigHash: firstConfig.hash
  },
  display: {
    width: 480,
    height: 480,
    rotation: 0,
    brightness: 0.8
  },
  touch: {
    controller: 'GT911',
    mode: 'irq',
    irqCount: 44
  },
  firmware: {
    name: 'espdisp',
    version: '0.5.0-dev',
    partition: { running: 'ota_0', next: 'ota_1' },
    rollback: { supported: true, pendingConfirm: false }
  },
  ota: {
    enabled: true,
    mode: 'arduino-ota',
    address: `${deviceId}.local`,
    port: 3232,
    passwordSet: true
  },
  webAuth: {
    enabled: true,
    username: 'espdisp',
    passwordSet: true
  },
  config: {
    version: firstConfig.version,
    hash: firstConfig.hash,
    applied: true
  },
  errors: []
}, auth)

assert.strictEqual(heartbeat.status, 'ok')
assert.strictEqual(manager.getDevice(deviceId).webAuth.enabled, true)
assert.strictEqual(heartbeat.desiredConfig.reload, false)

const command = manager.createCommand(deviceId, {
  type: 'theme.set',
  payload: { theme: 'night' }
})
assert.strictEqual(manager.getCommand(deviceId, command.id).status, 'pending')

const polled = manager.getCommands(deviceId, auth, 10)
assert.strictEqual(polled.commands.length, 1)
assert.strictEqual(polled.commands[0].type, 'theme.set')
assert.strictEqual(manager.getCommand(deviceId, command.id).status, 'delivered')

const ack = manager.ackCommand(deviceId, command.id, {
  result: {
    ok: true,
    appliedAt: '2026-05-27T18:20:01Z'
  }
}, auth)
assert.strictEqual(ack.status, 'acknowledged')

const group = manager.createGroupCommand('display', {
  type: 'screen.set',
  payload: { screen: 'autopilot' }
})
assert.strictEqual(group.count, 1)

const automation = manager.automationEvent({
  type: 'notification',
  source: 'test',
  deviceId,
  command: {
    type: 'overlay.show',
    payload: { overlay: 'mob' }
  }
})
assert.strictEqual(automation.actions.length, 1)

const artifact = manager.addFirmwareArtifact({
  vendor: { id: 'navado', name: 'Navado', trust: { level: 'local', allowUnsigned: true } },
  product: { id: 'espdisp', name: 'ESP Display' },
  firmware: {
    name: 'espdisp',
    version: '0.5.1',
    channel: 'dev',
    git: { commit: 'def5678', dirty: false }
  },
  compatibility: {
    boards: ['sunton_4848s040'],
    chip: 'ESP32-S3',
    minFlashBytes: 16777216,
    requiresPsram: true,
    partitionScheme: 'ota_16mb'
  },
  file: {
    name: 'espdisp-0.5.1-esp32-4848s040.bin',
    size: 1784512,
    sha256: 'sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc'
  },
  signing: { signed: false }
})

assert.strictEqual(manager.getFirmwareArtifact(artifact.artifactId).firmware.version, '0.5.1')

const upgrades = manager.firmwareUpgradeMatrix()
const deviceUpgrade = upgrades.devices.find((device) => device.deviceId === deviceId)
assert.ok(deviceUpgrade)
assert.strictEqual(deviceUpgrade.upgradable, true)
assert.strictEqual(deviceUpgrade.latestArtifact.artifactId, artifact.artifactId)
assert.ok(deviceUpgrade.availableArtifacts.find((candidate) => candidate.artifactId === artifact.artifactId))

const job = manager.createFirmwareJob(deviceId, { artifactId: artifact.artifactId })
assert.strictEqual(job.status, 'queued')
assert.strictEqual(manager.listFirmwareJobs(deviceId).jobs.length, 1)
assert.strictEqual(manager.getFirmwareJob(deviceId, job.jobId).artifactId, artifact.artifactId)

const otaCommand = manager.pendingCommands(deviceId).find((cmd) => cmd.type === 'firmware.update')
assert.ok(otaCommand)
assert.strictEqual(otaCommand.payload.jobId, job.jobId)
assert.strictEqual(otaCommand.payload.sha256, 'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc')

const progress = manager.updateFirmwareProgress(deviceId, job.jobId, {
  state: 'downloading',
  bytesRead: 1024,
  bytesTotal: 1784512,
  percent: 1
}, auth)
assert.strictEqual(progress.status, 'downloading')

const confirmed = manager.confirmFirmware(deviceId, {
  jobId: job.jobId,
  firmware: { version: '0.5.1', git: { commit: 'def5678' } },
  rollbackConfirmed: true
}, auth)
assert.strictEqual(confirmed.status, 'confirmed')

const nightProfile = manager.upsertProfile({
  id: 'night-watch',
  name: 'Night Watch',
  version: 2,
  config: {
    settings: { theme: 'night', brightness: 0.25 },
    debug: { logLevel: 'warn', touchMode: 'irq' }
  }
})
assert.strictEqual(nightProfile.hash.startsWith('sha256:'), true)

const assignment = manager.assignProfile(deviceId, { profileId: 'night-watch' })
assert.strictEqual(assignment.assignedProfile, 'night-watch')
const assignedConfig = manager.generateConfig(deviceId)
assert.strictEqual(assignedConfig.settings.theme, 'night')
assert.strictEqual(assignedConfig.settings.brightness, 0.25)

const groups = manager.listGroups().groups
assert.ok(groups.find((group) => group.id === 'all' && group.devices.includes(deviceId)))
assert.ok(groups.find((group) => group.id === 'display' && group.devices.includes(deviceId)))

const cancelled = manager.createCommand(deviceId, {
  type: 'beep',
  payload: { pattern: 'short' }
})
manager.cancelCommand(deviceId, cancelled.id, 'test cancellation')
assert.strictEqual(manager.getCommand(deviceId, cancelled.id).status, 'cancelled')

const expired = manager.createCommand(deviceId, {
  type: 'beep',
  expiresAt: '2000-01-01T00:00:00.000Z'
})
assert.strictEqual(manager.getCommand(deviceId, expired.id).status, 'expired')

const incompatible = manager.addFirmwareArtifact({
  vendor: { id: 'navado', name: 'Navado' },
  product: { id: 'espdisp', name: 'ESP Display' },
  firmware: { name: 'espdisp', version: '9.9.9' },
  compatibility: { boards: ['other-board'] },
  file: { name: 'bad.bin', size: 1, sha256: 'sha256:bad' }
})
assert.throws(
  () => manager.createFirmwareJob(deviceId, { artifactId: incompatible.artifactId }),
  /firmware board is not compatible/
)

const boardIdDeviceId = 'espdisp-board-id'
manager.registerDevice({
  device: {
    id: boardIdDeviceId,
    name: 'Board ID Firmware',
    board_id: 'sunton_4848s040',
    chip: 'ESP32-S3',
    firmware: { name: 'espdisp', version: '0.5.0-dev' }
  }
}, auth)
const boardIdUpgrade = manager.firmwareUpgradeMatrix().devices.find((device) => device.deviceId === boardIdDeviceId)
assert.strictEqual(boardIdUpgrade.board, 'sunton_4848s040')
assert.strictEqual(boardIdUpgrade.upgradable, true)
assert.doesNotThrow(() => manager.createFirmwareJob(boardIdDeviceId, { artifactId: artifact.artifactId }))

const provisioned = makeManager({
  auth: { mode: 'provision-token', provisionToken: 'static-provision' }
})
const token = provisioned.manager.createProvisioningToken({ ttlMs: 60000, uses: 1 })
assert.strictEqual(provisioned.manager.listProvisioningTokens().tokens.length, 1)
const provisionedReg = provisioned.manager.registerDevice({
  device: { id: 'espdisp-provisioned', board: 'sunton_4848s040' }
}, { provision: token.token })
assert.strictEqual(provisionedReg.status, 'registered')
assert.strictEqual(provisioned.manager.authStatus('espdisp-provisioned').provisioned, true)
