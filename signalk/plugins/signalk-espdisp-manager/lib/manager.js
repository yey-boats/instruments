const { JsonStore } = require('./store')
const {
  now,
  randomId,
  sha256Json,
  clone,
  sanitizeDeviceId,
  hostnameFromPolicy,
  mergeDeep
} = require('./util')

const PROTOCOL = 'espdisp.management.v1'

class EspDispManager {
  constructor (app, options) {
    this.app = app
    this.options = normalizeOptions(options || {})
    this.store = new JsonStore(app.getDataDirPath())
    this.store.init()
  }

  discovery () {
    return {
      protocol: PROTOCOL,
      serverId: this.options.serverId,
      basePath: '/plugins/espdisp-manager',
      auth: {
        methods: this.options.auth.mode === 'dev-shared-token'
          ? ['bearer-device-token', 'dev-shared-token']
          : ['provision-token', 'bearer-device-token']
      },
      intervals: {
        heartbeatMs: this.options.heartbeatMs,
        commandPollMs: this.options.commandPollMs
      },
      features: {
        registry: true,
        discovery: true,
        config: true,
        commands: true,
        firmware: true,
        ota: true,
        networkIdentity: true
      }
    }
  }

  registerDevice (body, auth) {
    const incoming = body.device || body
    const id = sanitizeDeviceId(incoming.id || incoming.deviceId)
    if (!id) throw httpError(400, 'invalid_request', 'device.id is required')
    this.requireDeviceAuth(id, auth, true)

    const existing = this.store.registry.devices[id] || {}
    const created = !existing.id
    const merged = {
      ...existing,
      id,
      name: existing.name || incoming.name || id,
      claimed: existing.claimed || false,
      role: existing.role || incoming.role || 'display',
      location: existing.location || incoming.location || null,
      firstSeen: existing.firstSeen || now(),
      lastSeen: now(),
      identity: incoming,
      capabilities: incoming.capabilities || existing.capabilities || {},
      display: incoming.display || existing.display || null,
      touch: incoming.touch || existing.touch || null,
      firmware: incoming.firmware || existing.firmware || null,
      board: incoming.board || existing.board || null,
      mac: incoming.mac || existing.mac || null,
      assignedProfile: existing.assignedProfile || null,
      overrides: existing.overrides || {},
      networkIdentity: this.resolveNetworkIdentity(existing, incoming),
      status: existing.status || {}
    }
    if (!merged.assignedProfile) {
      merged.assignedProfile = this.matchProfileForDevice(merged) || 'default'
    }

    if (!merged.deviceToken && this.options.auth.mode === 'dev-shared-token') {
      merged.deviceToken = this.options.auth.devToken
      merged.deviceTokenId = 'dev'
    } else if (!merged.deviceToken) {
      merged.deviceToken = randomId('dev')
      merged.deviceTokenId = randomId('tok')
    }

    this.store.registry.devices[id] = merged
    this.store.saveRegistry()
    this.store.audit(created ? 'device.registered' : 'device.reregistered', id)

    return {
      status: created ? 'registered' : 'updated',
      deviceId: id,
      claimed: merged.claimed,
      assignedProfile: merged.assignedProfile,
      deviceToken: created ? merged.deviceToken : undefined,
      tokenId: merged.deviceTokenId,
      config: {
        version: this.configVersion(merged),
        hash: this.generateConfig(id).hash,
        url: `/plugins/espdisp-manager/devices/${id}/config`
      },
      commands: {
        url: `/plugins/espdisp-manager/devices/${id}/commands`,
        pollMs: this.options.commandPollMs
      },
      heartbeat: {
        url: `/plugins/espdisp-manager/devices/${id}/status`,
        intervalMs: this.options.heartbeatMs
      }
    }
  }

  listDevices () {
    const filter = arguments[0] || {}
    const nowMs = Date.now()
    const onlineWindowMs = Math.max(this.options.heartbeatMs * 3, 15000)
    const values = Object.values(this.store.registry.devices)
      .map((device) => {
        const summary = summarizeDevice(device)
        const lastSeenMs = summary.lastSeen ? Date.parse(summary.lastSeen) : 0
        summary.online = lastSeenMs > 0 && nowMs - lastSeenMs <= onlineWindowMs
        summary.pendingCommands = this.pendingCommands(summary.id).length
        summary.health = this.deviceHealth(device, summary.online, summary.pendingCommands)
        return summary
      })
      .filter((device) => {
        if (filter.health && device.health !== filter.health) return false
        if (filter.profile && device.assignedProfile !== filter.profile && device.profile !== filter.profile) return false
        if (filter.role && device.role !== filter.role) return false
        if (filter.location && device.location !== filter.location) return false
        return true
      })
    return {
      devices: values.sort((a, b) => a.id.localeCompare(b.id))
    }
  }

  listDiscoveredDevices () {
    const registered = this.store.registry.devices
    return {
      devices: Object.values(this.store.discovery.devices || {})
        .map((device) => ({
          ...device,
          registered: Boolean(registered[device.deviceId]),
          stale: device.lastSeen ? Date.now() - Date.parse(device.lastSeen) > 60000 : true
        }))
        .sort((a, b) => {
          const aTime = Date.parse(a.lastSeen || 0)
          const bTime = Date.parse(b.lastSeen || 0)
          return bTime - aTime || a.deviceId.localeCompare(b.deviceId)
        })
    }
  }

  announceDiscoveredDevice (body, auth) {
    const incoming = body.device || body
    const id = sanitizeDeviceId(incoming.id || incoming.deviceId)
    if (!id) throw httpError(400, 'invalid_request', 'device.id is required')
    if (this.options.auth.mode !== 'disabled') {
      const ok = auth && (
        auth.bearer === this.options.auth.devToken ||
        auth.provision === this.options.auth.provisionToken
      )
      if (!ok) throw httpError(401, 'unauthorized', 'discovery announcement requires manager token')
    }

    const record = {
      deviceId: id,
      name: incoming.name || id,
      role: incoming.role || 'display',
      location: incoming.location || null,
      firstSeen: this.store.discovery.devices[id]
        ? this.store.discovery.devices[id].firstSeen
        : now(),
      lastSeen: now(),
      address: incoming.address || incoming.ip || null,
      port: Number(incoming.port || 80),
      transport: incoming.transport || 'http',
      services: incoming.services || [],
      mdns: incoming.mdns || null,
      display: incoming.display || null,
      firmware: incoming.firmware || null,
      board: incoming.board || null,
      capabilities: incoming.capabilities || {}
    }
    this.store.discovery.devices[id] = record
    this.store.saveDiscovery()
    this.store.audit('device.discovered', id, { address: record.address, port: record.port })
    return {
      status: 'discovered',
      device: record,
      registered: Boolean(this.store.registry.devices[id]),
      registerUrl: `/plugins/espdisp-manager/devices/register`
    }
  }

  listGroups () {
    const groups = new Map()
    const add = (id, deviceId) => {
      if (!id) return
      const key = String(id).toLowerCase()
      const group = groups.get(key) || { id: key, devices: [] }
      if (!group.devices.includes(deviceId)) group.devices.push(deviceId)
      groups.set(key, group)
    }
    Object.values(this.store.registry.devices).forEach((device) => {
      add('all', device.id)
      add(device.role, device.id)
      add(device.location, device.id)
    })
    return { groups: Array.from(groups.values()).sort((a, b) => a.id.localeCompare(b.id)) }
  }

  pluginCapabilities () {
    return {
      protocol: PROTOCOL,
      widgets: {
        types: [
          'numeric',
          'text',
          'gauge',
          'compass',
          'windRose',
          'trend',
          'bar',
          'button',
          'autopilot'
        ],
        optionalTypes: ['map']
      },
      fonts: {
        properties: [
          'fontSize',
          'labelFontSize',
          'valueFontSize',
          'unitFontSize',
          'titleFontSize',
          'buttonFontSize'
        ]
      },
      layout: {
        types: ['grid'],
        variantMatching: ['board', 'display.width', 'display.height', 'display.shape', 'capabilities']
      },
      limits: {
        maxWidgets: 32,
        maxScreens: 16,
        maxTilesPerScreen: 16,
        maxWidgetIdLength: 31,
        maxPathLength: 95,
        maxTitleLength: 23
      }
    }
  }

  dashboard () {
    this.expireCommands()
    const devices = Object.values(this.store.registry.devices)
    const nowMs = Date.now()
    const onlineWindowMs = Math.max(this.options.heartbeatMs * 3, 15000)
    const summaries = devices
      .map((device) => {
        const config = this.generateConfig(device.id)
        const lastSeenMs = device.lastSeen ? Date.parse(device.lastSeen) : 0
        const online = lastSeenMs > 0 && nowMs - lastSeenMs <= onlineWindowMs
        const pending = this.pendingCommands(device.id).length
        return {
          id: device.id,
          name: device.name,
          role: device.role,
          location: device.location,
          online,
          lastSeen: device.lastSeen || null,
          board: device.board || null,
          firmware: device.firmware || null,
          display: this.resolveDisplay(device),
          profile: device.assignedProfile || 'default',
          desiredConfig: {
            version: this.configVersion(device),
            hash: config.hash,
            layoutVariant: config.layout ? config.layout.variant : null,
            widgetVariant: config.widgets ? config.widgets.variant : null
          },
          reportedConfig: device.config || null,
          configDrift: !device.config || device.config.hash !== config.hash,
          networkIdentity: device.networkIdentity || null,
          pendingCommands: pending,
          health: this.deviceHealth(device, online, pending)
        }
      })
      .sort((a, b) => a.id.localeCompare(b.id))

    const commandCounts = this.store.commands.commands.reduce((acc, cmd) => {
      acc[cmd.status] = (acc[cmd.status] || 0) + 1
      return acc
    }, {})
    const jobCounts = this.store.jobs.jobs.reduce((acc, job) => {
      acc[job.status] = (acc[job.status] || 0) + 1
      return acc
    }, {})

    return {
      generatedAt: now(),
      protocol: PROTOCOL,
      serverId: this.options.serverId,
      counts: {
        devices: devices.length,
        online: summaries.filter((device) => device.online).length,
        offline: summaries.filter((device) => !device.online).length,
        configDrift: summaries.filter((device) => device.configDrift).length,
        pendingCommands: this.store.commands.commands.filter((cmd) => cmd.status === 'pending').length,
        profiles: Object.keys(this.store.profiles.profiles).length,
        firmwareArtifacts: this.store.firmware.artifacts.length,
        firmwareJobs: this.store.jobs.jobs.length
      },
      commandCounts,
      firmwareJobCounts: jobCounts,
      groups: this.listGroups().groups,
      devices: summaries,
      recentCommands: this.store.commands.commands.slice(-10).reverse(),
      recentFirmwareJobs: this.store.jobs.jobs.slice(-10).reverse()
    }
  }

  deviceHealth (device, online, pendingCommands) {
    if (!online) return 'offline'
    if (device.config && device.config.applied === false) return 'config-error'
    if (pendingCommands > 0) return 'pending'
    if (device.networkIdentity && device.networkIdentity.conflict) return 'network-conflict'
    return 'ok'
  }

  getDevice (id) {
    const device = this.store.registry.devices[id]
    if (!device) throw httpError(404, 'device_not_found', 'device not found')
    return device
  }

  patchDevice (id, patch) {
    const device = this.getDevice(id)
    const allowed = ['name', 'role', 'location', 'claimed', 'assignedProfile', 'overrides', 'networkIdentity']
    allowed.forEach((key) => {
      if (Object.prototype.hasOwnProperty.call(patch, key)) device[key] = patch[key]
    })
    device.networkIdentity = this.resolveNetworkIdentity(device, device.identity || {})
    device.updatedAt = now()
    this.store.saveRegistry()
    this.store.audit('device.updated', id, patch)
    return device
  }

  assignProfile (id, body) {
    const device = this.getDevice(id)
    const profileId = body.profileId || body.profile
    if (!profileId) throw httpError(400, 'invalid_request', 'profileId is required')
    if (!this.store.profiles.profiles[profileId]) {
      throw httpError(404, 'profile_not_found', 'profile not found')
    }
    device.assignedProfile = profileId
    if (body.overrides) device.overrides = body.overrides
    device.updatedAt = now()
    this.store.saveRegistry()
    this.store.audit('device.profile.assigned', id, { profileId })
    return {
      deviceId: id,
      assignedProfile: profileId,
      config: {
        version: this.configVersion(device),
        hash: this.generateConfig(id).hash
      }
    }
  }

  updateStatus (id, body, auth) {
    this.requireDeviceAuth(id, auth)
    const device = this.getDevice(id)
    device.lastSeen = now()
    device.status = body
    if (body.firmware) device.firmware = body.firmware
    if (body.display) device.display = body.display
    if (body.touch) device.touch = body.touch
    if (body.network) {
      device.networkIdentity = {
        ...(device.networkIdentity || {}),
        currentHostname: body.network.hostname || null,
        currentDomain: body.network.domain || null,
        currentFqdn: body.network.fqdn || null,
        lastResolvedAddress: body.network.ip || null,
        mdnsEnabled: Boolean(body.network.mdns && body.network.mdns.enabled)
      }
    }
    if (body.config) {
      device.config = {
        version: body.config.version,
        hash: body.config.hash,
        applied: body.config.applied
      }
    }
    this.store.saveRegistry()
    return {
      status: 'ok',
      serverTime: now(),
      desiredConfig: {
        version: this.configVersion(device),
        hash: this.generateConfig(id).hash,
        reload: !device.config || device.config.hash !== this.generateConfig(id).hash
      },
      commandsPending: this.pendingCommands(id).length
    }
  }

  generateConfig (id) {
    const device = this.getDevice(id)
    const profile = this.store.profiles.profiles[device.assignedProfile || 'default'] ||
      this.store.profiles.profiles.default
    const body = mergeDeep(profile.config || {}, device.overrides || {})
    const networkIdentity = this.resolveNetworkIdentity(device, device.identity || {})
    const display = this.resolveDisplay(device)
    const widgets = this.resolveWidgets(body.widgets || {}, device, display)
    const layout = this.resolveLayout(body.layout || {}, device, display, widgets)
    const config = {
      protocol: PROTOCOL,
      deviceId: id,
      version: this.configVersion(device),
      profile: profile.id,
      generatedAt: now(),
      display: {
        ...display,
        selectedVariant: layout.variant || widgets.variant || null
      },
      management: {
        heartbeatMs: this.options.heartbeatMs,
        commandPollMs: this.options.commandPollMs
      },
      network: {
        hostname: networkIdentity.desiredHostname,
        domain: networkIdentity.desiredDomain,
        fqdn: networkIdentity.desiredFqdn,
        mdns: {
          enabled: this.options.network.mdns.enabled,
          services: [
            {
              type: '_espdisp._tcp',
              port: 80,
              txt: {
                id,
                role: device.role || 'display',
                location: device.location || '',
                fw: device.firmware && device.firmware.version
                  ? device.firmware.version
                  : ''
              }
            },
            {
              type: '_arduino._tcp',
              port: 3232,
              txt: {
                id,
                ota: 'true'
              }
            }
          ]
        }
      },
      signalk: {
        host: this.options.signalk.host,
        port: this.options.signalk.port,
        useMdns: true
      },
      ota: {
        enabled: true,
        mode: 'arduino-ota',
        address: networkIdentity.desiredFqdn,
        port: 3232,
        passwordSet: Boolean(device.secrets && device.secrets.otaPasswordSet)
      },
      ...body,
      layout,
      widgets
    }
    const hashBody = clone(config)
    delete hashBody.generatedAt
    config.hash = sha256Json(hashBody)
    return config
  }

  listProfiles () {
    return { profiles: Object.values(this.store.profiles.profiles) }
  }

  upsertProfile (profile) {
    if (!profile.id) throw httpError(400, 'invalid_request', 'profile.id is required')
    profile.version = Number(profile.version || 1)
    profile.updatedAt = now()
    profile.hash = sha256Json(profile.config || {})
    this.store.profiles.profiles[profile.id] = profile
    this.store.saveProfiles()
    this.store.audit('profile.upserted', profile.id)
    return profile
  }

  matchProfileForDevice (device) {
    const display = this.resolveDisplay(device)
    const profiles = Object.values(this.store.profiles.profiles)
      .filter((profile) => profile.id !== 'default' && profile.match)
      .sort((a, b) => Number(b.priority || 0) - Number(a.priority || 0))
    const matched = profiles.find((profile) => this.profileMatches(profile, device, display))
    return matched ? matched.id : null
  }

  profileMatches (profile, device, display) {
    const match = profile.match || {}
    if (match.role && match.role !== device.role) return false
    if (match.location && match.location !== device.location) return false
    return this.variantMatches(match, device, display)
  }

  createCommand (id, body) {
    this.getDevice(id)
    if (!body.type) throw httpError(400, 'invalid_request', 'command type is required')
    const command = {
      id: body.id || randomId('cmd'),
      deviceId: id,
      type: body.type,
      status: 'pending',
      createdAt: now(),
      expiresAt: body.expiresAt || new Date(Date.now() + 60000).toISOString(),
      requiresAck: body.requiresAck !== false,
      payload: body.payload || {},
      result: null
    }
    this.store.commands.commands.push(command)
    this.store.saveCommands()
    this.store.audit('command.created', id, { commandId: command.id, type: command.type })
    return command
  }

  cancelCommand (id, commandId, reason) {
    const cmd = this.getCommand(id, commandId)
    if (cmd.status === 'acknowledged' || cmd.status === 'failed') {
      throw httpError(409, 'conflict', `cannot cancel ${cmd.status} command`)
    }
    cmd.status = 'cancelled'
    cmd.cancelledAt = now()
    cmd.result = { ok: false, code: 'cancelled', message: reason || 'cancelled' }
    this.store.saveCommands()
    this.store.audit('command.cancelled', id, { commandId })
    return cmd
  }

  expireCommands () {
    const nowMs = Date.now()
    let changed = false
    this.store.commands.commands.forEach((cmd) => {
      if ((cmd.status === 'pending' || cmd.status === 'delivered') &&
          Date.parse(cmd.expiresAt) <= nowMs) {
        cmd.status = 'expired'
        cmd.expiredAt = now()
        cmd.result = { ok: false, code: 'command_expired', message: 'command expired' }
        changed = true
      }
    })
    if (changed) this.store.saveCommands()
  }

  createGroupCommand (groupId, body) {
    const devices = this.devicesInGroup(groupId)
    const commands = devices.map((device) => this.createCommand(device.id, body))
    return {
      groupId,
      count: commands.length,
      commands
    }
  }

  automationEvent (body) {
    const event = {
      id: body.id || randomId('evt'),
      type: body.type || 'external',
      createdAt: now(),
      source: body.source || 'api',
      payload: body.payload || {}
    }
    const actions = []
    if (body.command && body.deviceId) {
      actions.push(this.createCommand(body.deviceId, body.command))
    }
    if (body.command && body.groupId) {
      actions.push(this.createGroupCommand(body.groupId, body.command))
    }
    this.store.audit('automation.event', event.id, { event, actionCount: actions.length })
    return { event, actions }
  }

  getCommands (id, auth, limit) {
    this.requireDeviceAuth(id, auth)
    this.expireCommands()
    const nowMs = Date.now()
    const commands = this.store.commands.commands
      .filter((cmd) => cmd.deviceId === id && cmd.status === 'pending')
      .filter((cmd) => Date.parse(cmd.expiresAt) > nowMs)
      .slice(0, Math.max(1, Math.min(Number(limit) || 10, 50)))

    commands.forEach((cmd) => {
      cmd.status = 'delivered'
      cmd.deliveredAt = now()
    })
    if (commands.length > 0) this.store.saveCommands()
    return { commands }
  }

  getCommand (id, commandId) {
    this.getDevice(id)
    this.expireCommands()
    const cmd = this.store.commands.commands.find((c) => c.deviceId === id && c.id === commandId)
    if (!cmd) throw httpError(404, 'command_not_found', 'command not found')
    return cmd
  }

  ackCommand (id, commandId, body, auth) {
    this.requireDeviceAuth(id, auth)
    const cmd = this.store.commands.commands.find((c) => c.deviceId === id && c.id === commandId)
    if (!cmd) throw httpError(404, 'command_not_found', 'command not found')
    const ok = body && body.result && body.result.ok !== false
    cmd.status = ok ? 'acknowledged' : 'failed'
    cmd.acknowledgedAt = now()
    cmd.result = body.result || body
    this.store.saveCommands()
    this.store.audit(`command.${cmd.status}`, id, { commandId })
    return { status: cmd.status, result: cmd.result }
  }

  pendingCommands (id) {
    this.expireCommands()
    const nowMs = Date.now()
    return this.store.commands.commands.filter((cmd) => {
      return cmd.deviceId === id &&
        (cmd.status === 'pending' || cmd.status === 'delivered') &&
        Date.parse(cmd.expiresAt) > nowMs
    })
  }

  devicesInGroup (groupId) {
    const id = String(groupId || '').toLowerCase()
    const devices = Object.values(this.store.registry.devices)
    if (id === 'all') return devices
    return devices.filter((device) => {
      return String(device.role || '').toLowerCase() === id ||
        String(device.location || '').toLowerCase() === id
    })
  }

  rotateDeviceToken (id) {
    const device = this.getDevice(id)
    const token = randomId('dev')
    device.deviceToken = token
    device.deviceTokenId = randomId('tok')
    device.updatedAt = now()
    this.store.saveRegistry()
    this.store.audit('device.token.rotated', id, { tokenId: device.deviceTokenId })
    return {
      deviceId: id,
      tokenId: device.deviceTokenId,
      deviceToken: token
    }
  }

  revokeDeviceToken (id) {
    const device = this.getDevice(id)
    device.deviceToken = null
    device.deviceTokenId = null
    device.updatedAt = now()
    this.store.saveRegistry()
    this.store.audit('device.token.revoked', id)
    return { deviceId: id, revoked: true }
  }

  listFirmware () {
    return clone(this.store.firmware)
  }

  addFirmwareArtifact (body) {
    if (!body.vendor || !body.vendor.id) throw httpError(400, 'invalid_request', 'vendor.id is required')
    if (!body.product || !body.product.id) throw httpError(400, 'invalid_request', 'product.id is required')
    if (!body.firmware || !body.firmware.version) throw httpError(400, 'invalid_request', 'firmware.version is required')
    if (!body.file || !body.file.sha256) throw httpError(400, 'invalid_request', 'file.sha256 is required')
    const artifact = {
      ...body,
      artifactId: body.artifactId || randomId('fw'),
      uploadedAt: now()
    }
    this.store.firmware.artifacts.push(artifact)
    this.store.saveFirmware()
    this.store.audit('firmware.artifact.added', artifact.artifactId)
    return artifact
  }

  getFirmwareArtifact (artifactId) {
    const artifact = this.store.firmware.artifacts.find((fw) => fw.artifactId === artifactId)
    if (!artifact) throw httpError(404, 'artifact_not_found', 'firmware artifact not found')
    return artifact
  }

  createFirmwareJob (id, body) {
    const device = this.getDevice(id)
    const artifact = this.store.firmware.artifacts.find((fw) => fw.artifactId === body.artifactId)
    if (!artifact) throw httpError(404, 'artifact_not_found', 'firmware artifact not found')
    this.validateFirmwareCompatibility(device, artifact)
    const job = {
      jobId: randomId('ota-job'),
      deviceId: id,
      artifactId: artifact.artifactId,
      status: 'queued',
      createdAt: now(),
      policy: body.policy || { reboot: true, confirmAfterBoot: true, rollbackOnFailure: true },
      progress: { state: 'queued', bytesRead: 0, bytesTotal: artifact.file ? artifact.file.size : 0 }
    }
    this.store.jobs.jobs.push(job)
    this.store.saveJobs()
    this.createCommand(id, {
      type: 'firmware.update',
      expiresAt: body.expiresAt,
      payload: {
        jobId: job.jobId,
        artifactId: artifact.artifactId,
        version: artifact.firmware && artifact.firmware.version,
        url: `/plugins/espdisp-manager/firmware/download/${job.jobId}`,
        sha256: artifact.file && artifact.file.sha256,
        size: artifact.file && artifact.file.size,
        mode: 'pull',
        reboot: job.policy.reboot,
        confirmAfterBoot: job.policy.confirmAfterBoot
      }
    })
    return job
  }

  listFirmwareJobs (id) {
    this.getDevice(id)
    return {
      jobs: this.store.jobs.jobs.filter((job) => job.deviceId === id)
    }
  }

  getFirmwareJob (id, jobId) {
    this.getDevice(id)
    const job = this.store.jobs.jobs.find((j) => j.deviceId === id && j.jobId === jobId)
    if (!job) throw httpError(404, 'job_not_found', 'firmware job not found')
    return job
  }

  firmwareDownloadInfo (jobId) {
    const job = this.store.jobs.jobs.find((j) => j.jobId === jobId)
    if (!job) throw httpError(404, 'job_not_found', 'firmware job not found')
    const artifact = this.getFirmwareArtifact(job.artifactId)
    if (!artifact.file || !artifact.file.path) {
      throw httpError(404, 'artifact_binary_missing', 'firmware binary is not stored in this test fixture')
    }
    return { job, artifact }
  }

  updateFirmwareProgress (id, jobId, body, auth) {
    this.requireDeviceAuth(id, auth)
    const job = this.store.jobs.jobs.find((j) => j.deviceId === id && j.jobId === jobId)
    if (!job) throw httpError(404, 'job_not_found', 'firmware job not found')
    job.status = body.state || job.status
    job.progress = { ...job.progress, ...body, updatedAt: now() }
    this.store.saveJobs()
    return job
  }

  confirmFirmware (id, body, auth) {
    this.requireDeviceAuth(id, auth)
    const job = this.store.jobs.jobs.find((j) => j.deviceId === id && j.jobId === body.jobId)
    if (!job) throw httpError(404, 'job_not_found', 'firmware job not found')
    job.status = 'confirmed'
    job.confirmedAt = now()
    job.confirmation = body
    this.store.saveJobs()
    this.store.audit('firmware.job.confirmed', id, { jobId: job.jobId })
    return job
  }

  resolveDisplay (device) {
    const source = device.display ||
      (device.identity && device.identity.display) ||
      {}
    return {
      width: Number(source.width || 480),
      height: Number(source.height || 480),
      rotation: Number(source.rotation || 0),
      colorDepth: Number(source.colorDepth || 16),
      density: source.density || 'mdpi',
      shape: source.shape || (Number(source.width || 480) === Number(source.height || 480) ? 'square' : 'wide'),
      safeArea: source.safeArea || {
        x: 0,
        y: 0,
        width: Number(source.width || 480),
        height: Number(source.height || 480)
      }
    }
  }

  resolveLayout (layout, device, display, widgets) {
    const selected = this.selectVariant(layout.variants || [], device, display)
    const screens = selected && selected.screens ? selected.screens : (layout.screens || [])
    const widgetItems = widgets && widgets.items ? widgets.items : {}
    const resolved = {
      ...clone(layout),
      variant: selected ? selected.id : layout.variant || null,
      screens: this.filterScreensByWidgets(screens, widgetItems)
    }
    delete resolved.variants
    return resolved
  }

  filterScreensByWidgets (screens, widgetItems) {
    return (screens || []).map((screen) => {
      if (!Array.isArray(screen.tiles)) return clone(screen)
      return {
        ...clone(screen),
        tiles: screen.tiles.filter((tile) => !tile.widget || widgetItems[tile.widget])
      }
    })
  }

  resolveWidgets (widgets, device, display) {
    const selected = this.selectVariant(widgets.variants || [], device, display)
    const defaults = mergeDeep(widgets.defaults || {}, selected && selected.defaults ? selected.defaults : {})
    const supported = device.capabilities && device.capabilities.widgets
      ? device.capabilities.widgets
      : {}
    const items = {}
    Object.keys(widgets.items || {}).forEach((id) => {
      const widget = widgets.items[id]
      if (!this.widgetSupported(widget, supported, device.capabilities || {})) return
      items[id] = {
        ...widget,
        fontSize: this.resolveFontSize(device, widget.fontSize || defaults.fontSize),
        labelFontSize: this.resolveFontSize(device, widget.labelFontSize || defaults.labelFontSize),
        valueFontSize: this.resolveFontSize(device, widget.valueFontSize || defaults.valueFontSize),
        unitFontSize: this.resolveFontSize(device, widget.unitFontSize || defaults.unitFontSize)
      }
    })
    return {
      version: widgets.version || 1,
      variant: selected ? selected.id : widgets.variant || null,
      defaults: {
        ...defaults,
        fontSize: this.resolveFontSize(device, defaults.fontSize),
        labelFontSize: this.resolveFontSize(device, defaults.labelFontSize),
        valueFontSize: this.resolveFontSize(device, defaults.valueFontSize),
        unitFontSize: this.resolveFontSize(device, defaults.unitFontSize)
      },
      items
    }
  }

  selectVariant (variants, device, display) {
    return variants.find((variant) => this.variantMatches(variant.match || {}, device, display)) || null
  }

  variantMatches (match, device, display) {
    if (match.board && match.board !== device.board) return false
    if (match.display) {
      if (match.display.width && Number(match.display.width) !== display.width) return false
      if (match.display.height && Number(match.display.height) !== display.height) return false
      if (match.display.shape && match.display.shape !== display.shape) return false
    }
    if (match.capabilities) {
      for (const key of Object.keys(match.capabilities)) {
        if (Boolean(device.capabilities && device.capabilities[key]) !== Boolean(match.capabilities[key])) {
          return false
        }
      }
    }
    return true
  }

  widgetSupported (widget, supportedWidgets, capabilities) {
    if (supportedWidgets && Object.prototype.hasOwnProperty.call(supportedWidgets, widget.type) &&
        supportedWidgets[widget.type] === false) {
      return false
    }
    if (widget.requires && widget.requires.capability &&
        capabilities[widget.requires.capability] !== true) {
      return false
    }
    return true
  }

  resolveFontSize (device, requested) {
    const size = Number(requested || 16)
    const fonts = device.capabilities && device.capabilities.fonts
    const supported = fonts && Array.isArray(fonts.sizes) && fonts.sizes.length > 0
      ? fonts.sizes.map(Number).sort((a, b) => a - b)
      : [10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 42, 48, 56]
    let best = supported[0]
    supported.forEach((candidate) => {
      if (candidate <= size) best = candidate
    })
    return best
  }

  validateFirmwareCompatibility (device, artifact) {
    const compatibility = artifact.compatibility || {}
    if (compatibility.boards && compatibility.boards.length > 0) {
      const board = device.board || (device.identity && device.identity.board)
      if (!compatibility.boards.includes(board)) {
        throw httpError(409, 'incompatible_firmware', 'firmware board is not compatible', {
          board,
          supportedBoards: compatibility.boards
        })
      }
    }
    if (compatibility.chip && device.identity && device.identity.chip &&
        compatibility.chip !== device.identity.chip) {
      throw httpError(409, 'incompatible_firmware', 'firmware chip is not compatible', {
        chip: device.identity.chip,
        requiredChip: compatibility.chip
      })
    }
  }

  createProvisioningToken (body) {
    const ttlMs = Number(body.ttlMs || 10 * 60 * 1000)
    const token = body.token || randomId('prov')
    const record = {
      id: randomId('prov-token'),
      token,
      createdAt: now(),
      expiresAt: new Date(Date.now() + ttlMs).toISOString(),
      usesRemaining: Number(body.uses || 1),
      note: body.note || ''
    }
    this.store.provisioning.tokens.push(record)
    this.store.saveProvisioning()
    this.store.audit('provisioning.token.created', record.id, { expiresAt: record.expiresAt })
    return record
  }

  listProvisioningTokens () {
    const nowMs = Date.now()
    return {
      tokens: this.store.provisioning.tokens.map((token) => ({
        id: token.id,
        createdAt: token.createdAt,
        expiresAt: token.expiresAt,
        usesRemaining: token.usesRemaining,
        expired: Date.parse(token.expiresAt) <= nowMs,
        note: token.note
      }))
    }
  }

  authStatus (id) {
    const device = this.getDevice(id)
    return {
      deviceId: id,
      tokenId: device.deviceTokenId || null,
      provisioned: Boolean(device.deviceToken),
      claimed: Boolean(device.claimed),
      authMode: this.options.auth.mode
    }
  }

  resolveNetworkIdentity (device, incoming) {
    const base = device.networkIdentity || {}
    const desiredHostname = hostnameFromPolicy({ ...device, ...incoming }, this.options.network)
    const desiredDomain = this.options.network.domain || 'local'
    return {
      desiredHostname,
      desiredDomain,
      desiredFqdn: `${desiredHostname}.${desiredDomain}`,
      currentHostname: base.currentHostname || incoming.hostname || null,
      currentDomain: base.currentDomain || null,
      currentFqdn: base.currentFqdn || null,
      mdnsEnabled: base.mdnsEnabled !== false,
      conflict: this.hostnameConflict(device.id, desiredHostname),
      lastResolvedAddress: base.lastResolvedAddress || null
    }
  }

  hostnameConflict (id, hostname) {
    return Object.values(this.store.registry.devices).some((device) => {
      return device.id !== id &&
        device.networkIdentity &&
        device.networkIdentity.desiredHostname === hostname
    })
  }

  configVersion (device) {
    const profile = this.store.profiles.profiles[device.assignedProfile || 'default'] ||
      this.store.profiles.profiles.default
    return Number(profile.version || 1)
  }

  requireDeviceAuth (id, auth, provisioningAllowed) {
    if (this.options.auth.mode === 'disabled') return
    if (this.options.auth.mode === 'dev-shared-token') {
      if (auth.bearer === this.options.auth.devToken) return
      if (provisioningAllowed && auth.provision === this.options.auth.devToken) return
      throw httpError(401, 'unauthorized', 'invalid development token')
    }
    const device = this.store.registry.devices[id]
    if (device && auth.bearer && auth.bearer === device.deviceToken) return
    if (provisioningAllowed && auth.provision && this.consumeProvisioningToken(auth.provision)) return
    if (provisioningAllowed && auth.provision && auth.provision === this.options.auth.provisionToken) return
    throw httpError(401, 'unauthorized', 'invalid device token')
  }

  consumeProvisioningToken (token) {
    const nowMs = Date.now()
    const record = this.store.provisioning.tokens.find((item) => {
      return item.token === token &&
        item.usesRemaining > 0 &&
        Date.parse(item.expiresAt) > nowMs
    })
    if (!record) return false
    record.usesRemaining -= 1
    this.store.saveProvisioning()
    this.store.audit('provisioning.token.used', record.id, { usesRemaining: record.usesRemaining })
    return true
  }
}

function normalizeOptions (options) {
  return {
    serverId: options.serverId || 'signalk-espdisp-manager',
    heartbeatMs: Number(options.heartbeatMs || 5000),
    commandPollMs: Number(options.commandPollMs || 1000),
    auth: {
      mode: options.auth && options.auth.mode ? options.auth.mode : 'dev-shared-token',
      devToken: options.auth && options.auth.devToken ? options.auth.devToken : 'espdisp-dev',
      provisionToken: options.auth && options.auth.provisionToken ? options.auth.provisionToken : 'espdisp-provision'
    },
    signalk: {
      host: options.signalk && options.signalk.host ? options.signalk.host : 'signalk.local',
      port: Number(options.signalk && options.signalk.port ? options.signalk.port : 3000)
    },
    network: {
      domain: options.network && options.network.domain ? options.network.domain : 'local',
      hostnamePrefix: options.network && options.network.hostnamePrefix ? options.network.hostnamePrefix : 'espdisp',
      namingPolicy: options.network && options.network.namingPolicy ? options.network.namingPolicy : 'device-id',
      mdns: {
        enabled: !(options.network && options.network.mdns && options.network.mdns.enabled === false)
      }
    }
  }
}

function summarizeDevice (device) {
  return {
    id: device.id,
    name: device.name,
    role: device.role,
    location: device.location,
    claimed: device.claimed,
    lastSeen: device.lastSeen,
    firmware: device.firmware,
    display: device.display,
    networkIdentity: device.networkIdentity,
    assignedProfile: device.assignedProfile,
    config: device.config,
    status: device.status && device.status.network
      ? { network: device.status.network, ui: device.status.ui }
      : {}
  }
}

function httpError (status, code, message, details) {
  const err = new Error(message)
  err.status = status
  err.payload = { error: { code, message, details: details || {} } }
  return err
}

module.exports = {
  EspDispManager,
  PROTOCOL,
  normalizeOptions,
  httpError
}
