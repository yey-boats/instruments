class MockFirmware {
  constructor (manager, options) {
    this.manager = manager
    this.deviceId = options.deviceId || 'espdisp-mock'
    this.auth = options.auth
    this.screen = 'dashboard'
    this.theme = 'day'
    this.brightness = 0.8
    this.config = null
    this.firmware = {
      name: 'espdisp',
      version: options.version || '0.5.0-dev',
      channel: 'dev',
      git: { commit: 'mock000', dirty: false },
      partition: { running: 'ota_0', next: 'ota_1' },
      rollback: { supported: true, pendingConfirm: false }
    }
    this.display = options.display || {
      width: 480,
      height: 480,
      rotation: 0,
      colorDepth: 16,
      density: 'mdpi',
      shape: 'square'
    }
    this.capabilities = options.capabilities || {
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

  identity () {
    return {
      id: this.deviceId,
      name: 'Mock ESP Display',
      model: 'ESP32-4848S040',
      board: 'sunton_4848s040',
      mac: 'AA:BB:CC:DD:EE:99',
      chip: 'ESP32-S3',
      flashBytes: 16777216,
      psramBytes: 8388608,
      display: this.display,
      touch: {
        enabled: true,
        width: this.display.width,
        height: this.display.height,
        controller: 'GT911',
        interrupt: true
      },
      firmware: this.firmware,
      capabilities: this.capabilities
    }
  }

  register () {
    return this.manager.registerDevice({
      protocol: 'espdisp.management.v1',
      device: this.identity()
    }, this.auth)
  }

  fetchConfig () {
    this.config = this.manager.generateConfig(this.deviceId)
    return this.config
  }

  heartbeat () {
    return this.manager.updateStatus(this.deviceId, {
      uptimeMs: 1000,
      network: {
        mode: 'sta',
        ssid: 'BoatWiFi',
        ip: '192.168.1.99',
        rssi: -52,
        hostname: this.config ? this.config.network.hostname : this.deviceId,
        domain: 'local',
        fqdn: this.config ? this.config.network.fqdn : `${this.deviceId}.local`,
        mdns: { enabled: true, services: ['_espdisp._tcp', '_arduino._tcp'] }
      },
      signalk: {
        host: 'signalk.local',
        port: 3000,
        connected: true,
        lastDeltaAgeMs: 200
      },
      ui: {
        screen: this.screen,
        theme: this.theme,
        brightness: this.brightness,
        layoutVariant: this.config ? this.config.layout.variant : null,
        widgetConfigHash: this.config ? this.config.hash : null
      },
      display: this.display,
      firmware: this.firmware,
      ota: {
        enabled: true,
        mode: 'arduino-ota',
        address: this.config ? this.config.ota.address : `${this.deviceId}.local`,
        port: 3232,
        passwordSet: false
      },
      config: this.config
        ? { version: this.config.version, hash: this.config.hash, applied: true }
        : { version: 0, hash: null, applied: false },
      errors: []
    }, this.auth)
  }

  pollAndExecute () {
    const polled = this.manager.getCommands(this.deviceId, this.auth, 10)
    return polled.commands.map((command) => {
      const result = this.execute(command)
      this.manager.ackCommand(this.deviceId, command.id, { result }, this.auth)
      return { command, result }
    })
  }

  execute (command) {
    try {
      switch (command.type) {
        case 'screen.set':
          this.screen = command.payload.screen
          return { ok: true, message: 'screen changed' }
        case 'theme.set':
          this.theme = command.payload.theme
          return { ok: true, message: 'theme changed' }
        case 'brightness.set':
          this.brightness = command.payload.value
          return { ok: true, message: 'brightness changed' }
        case 'config.reload':
        case 'layout.reload':
          this.fetchConfig()
          return { ok: true, message: 'config reloaded' }
        case 'firmware.update':
          return this.executeFirmwareUpdate(command)
        default:
          return {
            ok: false,
            code: 'unsupported_command',
            message: `unsupported command ${command.type}`
          }
      }
    } catch (err) {
      return { ok: false, code: 'failed', message: err.message }
    }
  }

  executeFirmwareUpdate (command) {
    const jobId = command.payload.jobId
    this.manager.updateFirmwareProgress(this.deviceId, jobId, {
      state: 'downloading',
      bytesRead: command.payload.size || 0,
      bytesTotal: command.payload.size || 0,
      percent: 100
    }, this.auth)
    this.manager.updateFirmwareProgress(this.deviceId, jobId, {
      state: 'rebooting',
      bytesRead: command.payload.size || 0,
      bytesTotal: command.payload.size || 0,
      percent: 100
    }, this.auth)
    this.firmware = {
      ...this.firmware,
      version: command.payload.version,
      partition: { running: 'ota_1', next: 'ota_0' }
    }
    this.manager.confirmFirmware(this.deviceId, {
      jobId,
      firmware: this.firmware,
      rollbackConfirmed: true
    }, this.auth)
    return { ok: true, message: 'firmware updated' }
  }
}

module.exports = { MockFirmware }
