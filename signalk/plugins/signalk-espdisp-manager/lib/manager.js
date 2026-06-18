const dgram = require('dgram')
const crypto = require('crypto')
const http = require('http')
const os = require('os')
const { JsonStore } = require('./store')
const { ProtoControl } = require('./proto-control')
const fieldSchema = require('./field-schema')
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
const SIGNALK_DISCOVERY_QUERY = 'espdisp.signalk.discover.v1'
const SIGNALK_DISCOVERY_PROTOCOL = 'espdisp.signalk.discovery.v1'
const DEVICE_ANNOUNCE_PROTOCOL = 'espdisp.device.announce.v1'
// Standard known view ids a display falls back to when it has no stored
// screen list yet (e.g. freshly registered, layout not seeded). Mirrors the
// default preset screen ids so the knob's Select-View menu is never empty.
const KNOWN_VIEW_IDS = ['dashboard', 'wind', 'nav', 'depth', 'autopilot', 'system']
const KNOWN_VIEW_TITLES = {
  dashboard: 'Dashboard',
  wind: 'Wind',
  nav: 'Nav',
  depth: 'Depth',
  autopilot: 'Autopilot',
  system: 'System'
}
const ESPDISP_MDNS_SERVICE = '_espdisp._tcp.local'
const ESPDISP_MGMT_MDNS_SERVICE = '_espdisp-mgmt._tcp.local'
const MDNS_MULTICAST = '224.0.0.251'
const MDNS_PORT = 5353
const SUPPORTED_FIRMWARE_TARGETS = [
  { target: 'esp32-4848s040', board: 'sunton_4848s040' },
  { target: 'waveshare-touch-lcd-4', board: 'waveshare_touch_lcd_4' },
  { target: 'waveshare-touch-lcd-4_3', board: 'waveshare_touch_lcd_4_3' },
  { target: 'waveshare-touch-lcd-4_3b', board: 'waveshare_touch_lcd_4_3b' },
  { target: 'waveshare-touch-lcd-5_800x480', board: 'waveshare_touch_lcd_5_800x480' },
  { target: 'waveshare-touch-lcd-5_1024x600', board: 'waveshare_touch_lcd_5_1024x600' },
  { target: 'waveshare-touch-lcd-7_800x480', board: 'waveshare_touch_lcd_7_800x480' },
  { target: 'waveshare-touch-lcd-7b_1024x600', board: 'waveshare_touch_lcd_7b_1024x600' }
]

class EspDispManager {
  constructor (app, options) {
    this.app = app
    this.options = normalizeOptions(options || {})
    this.store = new JsonStore(app.getDataDirPath())
    this.store.init()
    this.discoverySocket = null
    this.deviceDiscoverySocket = null
    this.mdnsDiscoverySocket = null
    this.mdnsAdvertiseTimer = null
    // Control-protocol client: the registry/naming layer now *speaks the
    // protocol*. It attaches to targets as a controller (carrying this
    // plugin's controllerId/name/color and optional shared key) to discover
    // DeviceRecords and to drive on-demand screen switches (attach->switch->
    // detach). Version negotiation, auth, and message validation are all the
    // shared @espdisp/proto lib — never re-implemented here.
    this.protoControl = new ProtoControl({
      controllerId: this.options.control.controllerId,
      name: this.options.control.name,
      color: this.options.control.color,
      key: this.options.control.key,
      debug: this.app && this.app.debug ? (msg) => this.app.debug(msg) : undefined
    })
    this.startSignalKDiscovery()
    this.startDeviceDiscovery()
    this.startMdnsDiscovery()
    if (this.options.firmware.github.enabled) {
      this.refreshFirmwareFromGithub().catch((err) => {
        if (this.app && this.app.debug) {
          this.app.debug(`espdisp GitHub firmware refresh failed: ${err.message}`)
        }
      })
    }
  }

  close () {
    if (this.discoverySocket) {
      this.discoverySocket.close()
      this.discoverySocket = null
    }
    if (this.deviceDiscoverySocket) {
      this.deviceDiscoverySocket.close()
      this.deviceDiscoverySocket = null
    }
    if (this.mdnsDiscoverySocket) {
      this.mdnsDiscoverySocket.close()
      this.mdnsDiscoverySocket = null
    }
    if (this.mdnsAdvertiseTimer) {
      clearInterval(this.mdnsAdvertiseTimer)
      this.mdnsAdvertiseTimer = null
    }
  }

  startSignalKDiscovery () {
    const cfg = this.options.discoveryUdp
    if (!cfg.enabled) return

    const socket = dgram.createSocket('udp4')
    socket.on('error', (err) => {
      if (this.app && this.app.debug) {
        this.app.debug(`espdisp SignalK discovery UDP error: ${err.message}`)
      }
    })
    socket.on('message', (msg, rinfo) => {
      const text = msg.toString('utf8').trim()
      if (text !== SIGNALK_DISCOVERY_QUERY) return

      const host = cfg.host || this.options.signalk.host || 'auto'
      const reply = Buffer.from(JSON.stringify({
        protocol: SIGNALK_DISCOVERY_PROTOCOL,
        serverId: this.options.serverId,
        host,
        port: this.options.signalk.port,
        http: {
          path: '/signalk',
          stream: '/signalk/v1/stream?subscribe=none'
        },
        manager: {
          basePath: '/plugins/espdisp-manager'
        }
      }))
      socket.send(reply, rinfo.port, rinfo.address)
    })
    socket.bind(cfg.port, cfg.bind, () => {
      socket.setBroadcast(true)
      if (this.app && this.app.debug) {
        this.app.debug(`espdisp SignalK discovery UDP listening on ${cfg.bind}:${cfg.port}`)
      }
    })
    this.discoverySocket = socket
  }

  startDeviceDiscovery () {
    const cfg = this.options.deviceDiscoveryUdp
    if (!cfg.enabled) return

    const socket = dgram.createSocket('udp4')
    socket.on('error', (err) => {
      if (this.app && this.app.debug) {
        this.app.debug(`espdisp device discovery UDP error: ${err.message}`)
      }
    })
    socket.on('message', (msg, rinfo) => {
      let body
      try {
        body = JSON.parse(msg.toString('utf8'))
      } catch (err) {
        return
      }
      if (!body || body.protocol !== DEVICE_ANNOUNCE_PROTOCOL) return
      try {
        this.recordDiscoveredDevice({
          ...body,
          address: body.address || rinfo.address,
          transport: 'http',
          services: [
            { type: '_espdisp._tcp', port: Number(body.port || 80) }
          ]
        })
      } catch (err) {
        if (this.app && this.app.debug) {
          this.app.debug(`espdisp device discovery ignored packet: ${err.message}`)
        }
      }
    })
    socket.bind(cfg.port, cfg.bind, () => {
      socket.setBroadcast(true)
      if (this.app && this.app.debug) {
        this.app.debug(`espdisp device discovery UDP listening on ${cfg.bind}:${cfg.port}`)
      }
    })
    this.deviceDiscoverySocket = socket
  }

  startMdnsDiscovery () {
    const cfg = this.options.network.mdns
    if (!cfg.enabled || (!cfg.browser && !cfg.advertiseManager)) return

    const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true })
    socket.on('error', (err) => {
      if (this.app && this.app.debug) {
        this.app.debug(`espdisp mDNS discovery error: ${err.message}`)
      }
    })
    socket.on('message', (msg, rinfo) => {
      if (cfg.browser) {
        for (const device of devicesFromMdnsPacket(msg, rinfo)) {
          try {
            this.recordDiscoveredDevice(device)
          } catch (err) {
            if (this.app && this.app.debug) {
              this.app.debug(`espdisp mDNS discovery ignored packet: ${err.message}`)
            }
          }
        }
      }
      if (cfg.advertiseManager && mdnsPacketHasQuestion(msg, ESPDISP_MGMT_MDNS_SERVICE)) {
        this.sendManagerMdnsAdvertisement('query')
      }
    })
    socket.bind(cfg.port, cfg.bind, () => {
      try {
        socket.addMembership(MDNS_MULTICAST)
      } catch (err) {
        if (this.app && this.app.debug) {
          this.app.debug(`espdisp mDNS discovery multicast join failed: ${err.message}`)
        }
      }
      if (this.app && this.app.debug) {
        this.app.debug(`espdisp mDNS discovery listening on ${cfg.bind}:${cfg.port}`)
      }
      if (cfg.advertiseManager) {
        this.sendManagerMdnsAdvertisement('boot')
        this.mdnsAdvertiseTimer = setInterval(() => {
          this.sendManagerMdnsAdvertisement('periodic')
        }, cfg.advertiseIntervalMs)
      }
    })
    this.mdnsDiscoverySocket = socket
  }

  sendManagerMdnsAdvertisement (reason) {
    if (!this.mdnsDiscoverySocket) return
    const cfg = this.options.network.mdns
    const packet = buildManagerMdnsPacket(this.options, cfg)
    this.mdnsDiscoverySocket.send(packet, 0, packet.length, MDNS_PORT, MDNS_MULTICAST, (err) => {
      if (err && this.app && this.app.debug) {
        this.app.debug(`espdisp mDNS manager advertise ${reason} failed: ${err.message}`)
      } else if (this.app && this.app.debug) {
        this.app.debug(`espdisp mDNS manager advertise ${reason}`)
      }
    })
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
      board: incoming.board || incoming.board_id || existing.board || null,
      mac: incoming.mac || existing.mac || null,
      assignedProfile: existing.assignedProfile || null,
      overrides: existing.overrides || {},
      networkIdentity: this.resolveNetworkIdentity(existing, incoming),
      status: existing.status || {}
    }
    if (!merged.assignedProfile) {
      merged.assignedProfile = this.matchProfileForDevice(merged) || 'default'
    }

    let issuedToken = null
    if (!deviceHasToken(merged) && this.options.auth.mode === 'dev-shared-token') {
      issuedToken = this.options.auth.devToken
      merged.deviceTokenHash = hashToken(issuedToken)
      merged.deviceTokenId = 'dev'
      delete merged.deviceToken
    } else if (!deviceHasToken(merged)) {
      issuedToken = randomId('dev')
      merged.deviceTokenHash = hashToken(issuedToken)
      merged.deviceTokenId = randomId('tok')
      delete merged.deviceToken
    } else if (merged.deviceToken) {
      merged.deviceTokenHash = hashToken(merged.deviceToken)
      delete merged.deviceToken
    }

    this.store.registry.devices[id] = merged
    this.store.saveRegistry()
    this.store.audit(created ? 'device.registered' : 'device.reregistered', id)

    return {
      status: created ? 'registered' : 'updated',
      deviceId: id,
      claimed: merged.claimed,
      assignedProfile: merged.assignedProfile,
      deviceToken: created ? issuedToken : undefined,
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

  // Lightweight summary list for remote enumeration (the Waveshare knob's
  // Select-Display menu). Each entry is the minimum the knob needs to render
  // a row and target a screen.set: id, label, role, online flag and the
  // device's current on-screen view. Distinct from listDevices() (which
  // returns the full management projection) so the firmware fetch stays small.
  deviceSummaries () {
    const nowMs = Date.now()
    const onlineWindowMs = Math.max(this.options.heartbeatMs * 3, 15000)
    const devices = Object.values(this.store.registry.devices)
      .map((device) => {
        const lastSeenMs = device.lastSeen ? Date.parse(device.lastSeen) : 0
        const online = lastSeenMs > 0 && nowMs - lastSeenMs <= onlineWindowMs
        return {
          id: device.id,
          name: device.name || device.id,
          role: device.role || 'display',
          online,
          currentScreen: (device.status && device.status.ui && device.status.ui.screen) || null
        }
      })
      .sort((a, b) => a.id.localeCompare(b.id))
    return { devices }
  }

  // The set of views (screens) a device can switch between, for the knob's
  // Select-View menu. Derived from the device's generated layout screen list;
  // each view is { id, title }. `current` is the device's active screen id (if
  // reported via heartbeat). When the device has no stored/resolved screen
  // list, fall back to the standard known view ids so the menu is still usable.
  deviceViews (id) {
    const device = this.getDevice(id)
    let views = []
    // 1. Prefer the device's self-reported screen list (heartbeat ui.screens).
    // The firmware's actually-loaded layout can diverge from the manager's
    // generated config, so the device's own list is authoritative when present
    // — this is what makes switch-to-screen land on a real screen id.
    const reported = device.status && device.status.ui && device.status.ui.screens
    if (Array.isArray(reported) && reported.length > 0) {
      views = reported
        .filter((screen) => screen && screen.id)
        .map((screen) => ({ id: String(screen.id), title: String(screen.title || screen.id) }))
    }
    // 2. Fall back to the manager-generated config's screens.
    if (views.length === 0) {
      try {
        const config = this.generateConfig(id)
        const screens = (config.layout && Array.isArray(config.layout.screens))
          ? config.layout.screens
          : []
        views = screens
          .filter((screen) => screen && screen.id)
          .map((screen) => ({ id: String(screen.id), title: String(screen.title || screen.id) }))
      } catch (err) {
        views = []
      }
    }
    // 3. Last resort: the standard known view ids (so the menu is never empty).
    if (views.length === 0) {
      views = KNOWN_VIEW_IDS.map((id) => ({ id, title: KNOWN_VIEW_TITLES[id] || id }))
    }
    const current = (device.status && device.status.ui && device.status.ui.screen) || null
    return { views, current }
  }

  // The device's self-reported capability manifest (heartbeat ui.capabilities):
  // per-view-type limits/attrs the layout editor gates to. Returns null when the
  // device hasn't reported one yet (offline, or pre-manifest firmware).
  deviceCapabilities (id) {
    const device = this.getDevice(id)
    const caps = device.status && device.status.ui && device.status.ui.capabilities
    return caps && typeof caps === 'object' ? caps : null
  }

  // The manifest the layout editor gates to: the device's reported
  // `ui.capabilities`, or the built-in default set when the device is offline /
  // running pre-manifest firmware, so the editor still works.
  effectiveManifest (id) {
    return fieldSchema.resolveManifest(this.deviceCapabilities(id))
  }

  // ---- Slice 5: manifest-gated layout editor CRUD ------------------------
  //
  // The editor authors a device's screens (views) + per-field bindings into the
  // assigned profile's config: `layout.screens[*].tiles[*]` reference a key in
  // `widgets.items`, and each item holds the typed-tuple field (type, paths,
  // format, size, unit, color, range, zones, zoom). These methods are the
  // single write path; index.js routes call them. Every write is gated to the
  // device manifest (view/tile counts, field validity) and persisted, then a
  // config.reload is queued (poll + configPush) so the device fetches it.

  // The profile the editor mutates for a device (its assigned profile, default).
  editorProfile (id) {
    const device = this.getDevice(id)
    const profileId = device.assignedProfile || 'default'
    const profile = this.store.profiles.profiles[profileId]
    if (!profile) throw httpError(404, 'profile_not_found', `profile ${profileId} not found`)
    return profile
  }

  // The editable layout for a device: { manifest, screens:[{id,title,tiles}],
  // items } drawn from the assigned profile's raw config (NOT the resolved one —
  // the editor authors source, generateConfig resolves it for the device).
  editorLayout (id) {
    const profile = this.editorProfile(id)
    const cfg = profile.config || {}
    const screens = (cfg.layout && Array.isArray(cfg.layout.screens)) ? clone(cfg.layout.screens) : []
    const items = (cfg.widgets && cfg.widgets.items) ? clone(cfg.widgets.items) : {}
    return {
      profileId: profile.id,
      manifest: this.effectiveManifest(id),
      screens,
      items
    }
  }

  // Mutate the assigned profile's config under a guarded transaction: clone,
  // apply `mutate(cfg, manifest)`, validate counts, persist, reload. `mutate`
  // throws httpError on a manifest violation. Returns the fresh editorLayout.
  mutateEditorLayout (id, mutate) {
    const profile = this.editorProfile(id)
    const manifest = this.effectiveManifest(id)
    const cfg = clone(profile.config || {})
    cfg.layout = cfg.layout || { version: 1, screens: [] }
    if (!Array.isArray(cfg.layout.screens)) cfg.layout.screens = []
    cfg.widgets = cfg.widgets || { version: 1, items: {} }
    if (!cfg.widgets.items || typeof cfg.widgets.items !== 'object') cfg.widgets.items = {}

    mutate(cfg, manifest)

    // Enforce the manifest caps after the mutation (defence in depth).
    const maxViews = Number(manifest.maxViews || 8)
    if (cfg.layout.screens.length > maxViews) {
      throw httpError(409, 'max_views_exceeded', `max views reached (${maxViews})`)
    }
    const maxTiles = Number(manifest.maxTilesPerScreen || 4)
    for (const s of cfg.layout.screens) {
      if (Array.isArray(s.tiles) && s.tiles.length > maxTiles) {
        throw httpError(409, 'max_tiles_exceeded', `screen "${s.id}" exceeds ${maxTiles} tiles`)
      }
    }
    // Drop orphan widget items not referenced by any tile (keeps the config
    // tidy and bounds growth). Built-in/legacy items referenced by preset tiles
    // stay because their tiles stay.
    const referenced = new Set()
    for (const s of cfg.layout.screens) {
      for (const t of (s.tiles || [])) if (t && t.widget) referenced.add(t.widget)
    }
    for (const key of Object.keys(cfg.widgets.items)) {
      if (!referenced.has(key)) delete cfg.widgets.items[key]
    }

    profile.config = cfg
    this.upsertProfile({ id: profile.id, name: profile.name || profile.id, config: cfg })
    try { this.queueConfigReload(id) } catch (e) {}
    return this.editorLayout(id)
  }

  // Reject reserved/dangerous object keys so a crafted id can never write onto
  // Object.prototype via our config maps.
  static safeKey (key) {
    const k = String(key || '')
    return k && k !== '__proto__' && k !== 'prototype' && k !== 'constructor'
  }

  // Stable slug for a new screen/widget id derived from a label, deduped.
  static slugify (label, prefix, taken) {
    let base = String(label || '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/(^-|-$)/g, '')
    if (!base) base = prefix
    let candidate = base
    let n = 2
    while (taken.has(candidate)) { candidate = `${base}-${n++}` }
    return candidate
  }

  addScreen (id, opts) {
    return this.mutateEditorLayout(id, (cfg, manifest) => {
      const maxViews = Number(manifest.maxViews || 8)
      if (cfg.layout.screens.length >= maxViews) {
        throw httpError(409, 'max_views_exceeded', `max views reached (${maxViews})`)
      }
      const taken = new Set(cfg.layout.screens.map((s) => s.id))
      const title = String((opts && opts.title) || 'New View').slice(0, 64)
      const screenId = EspDispManager.slugify(title, 'view', taken)
      cfg.layout.screens.push({ id: screenId, type: 'grid', title, tiles: [] })
    })
  }

  renameScreen (id, screenId, title) {
    return this.mutateEditorLayout(id, (cfg) => {
      const s = cfg.layout.screens.find((x) => x.id === screenId)
      if (!s) throw httpError(404, 'screen_not_found', `screen ${screenId} not found`)
      s.title = String(title || s.title).slice(0, 64)
    })
  }

  reorderScreens (id, order) {
    return this.mutateEditorLayout(id, (cfg) => {
      if (!Array.isArray(order)) throw httpError(400, 'invalid_request', 'order must be an array of screen ids')
      const byId = new Map(cfg.layout.screens.map((s) => [s.id, s]))
      const next = []
      for (const sid of order) { if (byId.has(sid)) { next.push(byId.get(sid)); byId.delete(sid) } }
      // Append any screens not named in `order` (never silently drop one).
      for (const s of byId.values()) next.push(s)
      cfg.layout.screens = next
    })
  }

  deleteScreen (id, screenId) {
    return this.mutateEditorLayout(id, (cfg) => {
      const before = cfg.layout.screens.length
      cfg.layout.screens = cfg.layout.screens.filter((s) => s.id !== screenId)
      if (cfg.layout.screens.length === before) {
        throw httpError(404, 'screen_not_found', `screen ${screenId} not found`)
      }
    })
  }

  // Add a field (tile + its widget item) to a screen. `field` is the typed
  // tuple; it is coerced + validated against the manifest before persisting.
  addField (id, screenId, field) {
    return this.mutateEditorLayout(id, (cfg, manifest) => {
      const s = cfg.layout.screens.find((x) => x.id === screenId)
      if (!s) throw httpError(404, 'screen_not_found', `screen ${screenId} not found`)
      if (!Array.isArray(s.tiles)) s.tiles = []
      const maxTiles = Number(manifest.maxTilesPerScreen || 4)
      if (s.tiles.length >= maxTiles) {
        throw httpError(409, 'max_tiles_exceeded', `max tiles per screen reached (${maxTiles}) on "${screenId}"`)
      }
      const coerced = fieldSchema.coerceField(field || {}, manifest)
      const result = fieldSchema.validateField(coerced, manifest)
      if (!result.ok) {
        throw httpError(422, 'field_invalid', 'field failed manifest validation', { errors: result.errors })
      }
      const taken = new Set(Object.keys(cfg.widgets.items))
      const widgetId = EspDispManager.slugify(`${screenId}-${coerced.type}`, 'field', taken)
      cfg.widgets.items[widgetId] = this._fieldToWidgetItem(coerced)
      s.tiles.push({ widget: widgetId, ...this._fieldToTile(coerced) })
    })
  }

  // Reconfigure an existing field in place (by its widget id on a screen).
  updateField (id, screenId, widgetId, field) {
    if (!EspDispManager.safeKey(widgetId)) throw httpError(400, 'invalid_request', 'invalid field id')
    return this.mutateEditorLayout(id, (cfg, manifest) => {
      const s = cfg.layout.screens.find((x) => x.id === screenId)
      if (!s) throw httpError(404, 'screen_not_found', `screen ${screenId} not found`)
      const tile = (s.tiles || []).find((t) => t && t.widget === widgetId)
      if (!tile) throw httpError(404, 'field_not_found', `field ${widgetId} not on screen ${screenId}`)
      if (!Object.prototype.hasOwnProperty.call(cfg.widgets.items, widgetId)) {
        throw httpError(404, 'field_not_found', `widget item ${widgetId} not found`)
      }
      const coerced = fieldSchema.coerceField(field || {}, manifest)
      const result = fieldSchema.validateField(coerced, manifest)
      if (!result.ok) {
        throw httpError(422, 'field_invalid', 'field failed manifest validation', { errors: result.errors })
      }
      cfg.widgets.items[widgetId] = this._fieldToWidgetItem(coerced)
      Object.assign(tile, { widget: widgetId, ...this._fieldToTile(coerced) })
    })
  }

  removeField (id, screenId, widgetId) {
    return this.mutateEditorLayout(id, (cfg) => {
      const s = cfg.layout.screens.find((x) => x.id === screenId)
      if (!s) throw httpError(404, 'screen_not_found', `screen ${screenId} not found`)
      const before = (s.tiles || []).length
      s.tiles = (s.tiles || []).filter((t) => !(t && t.widget === widgetId))
      if (s.tiles.length === before) {
        throw httpError(404, 'field_not_found', `field ${widgetId} not on screen ${screenId}`)
      }
      // Orphan item cleanup happens in mutateEditorLayout.
    })
  }

  // Map a coerced field tuple to a `widgets.items[*]` entry. We keep the full
  // typed tuple plus a flattened `type`/`path` for back-compat with the
  // existing resolve/preview pipeline (live-preview flattens to {widget,path}).
  _fieldToWidgetItem (field) {
    const item = {
      type: field.type,
      paths: field.paths || {},
      // Flattened single path for legacy consumers (live-preview, resolveWidgets).
      path: (field.paths && field.paths.value) || ''
    }
    if (field.title != null) item.title = field.title
    if (field.format != null) item.format = field.format
    if (field.size != null) item.size = field.size
    if (field.unit != null) item.unit = field.unit
    if (field.color != null) item.color = field.color
    if (field.range != null) item.range = field.range
    if (field.zones != null) item.zones = field.zones
    if (field.zoomable != null) item.zoomable = field.zoomable
    if (field.zoom != null) item.zoom = field.zoom
    // Reserved compass marker fields (future slice) pass through untouched.
    if (field.reference != null) item.reference = field.reference
    if (Array.isArray(field.markers)) item.markers = field.markers
    return item
  }

  // The tile half of an authored field (the part the layout/preview reads).
  // Does NOT set `widget` — the caller owns the widget-item key so the tile
  // points at `widgets.items[widgetId]`, not at the bare type.
  _fieldToTile (field) {
    const tile = {}
    if (field.title != null) tile.title = field.title
    if (field.paths && field.paths.value) tile.path = field.paths.value
    if (field.unit != null) tile.unit = field.unit
    return tile
  }

  // Persist configured range/zones onto a field's metadata ("save limits").
  // Pure config write; the optional SK meta write-back is handled in index.js
  // (it needs the SignalK PUT transport). Returns the fresh editorLayout.
  saveFieldLimits (id, screenId, widgetId, limits) {
    if (!EspDispManager.safeKey(widgetId)) throw httpError(400, 'invalid_request', 'invalid field id')
    return this.mutateEditorLayout(id, (cfg, manifest) => {
      if (!Object.prototype.hasOwnProperty.call(cfg.widgets.items, widgetId)) {
        throw httpError(404, 'field_not_found', `field ${widgetId} not found`)
      }
      const item = cfg.widgets.items[widgetId]
      if (!fieldSchema.RANGED_TYPES.includes(item.type)) {
        throw httpError(409, 'limits_not_applicable', `${item.type} fields have no range/zones`)
      }
      if (limits && limits.range && typeof limits.range.min === 'number' && typeof limits.range.max === 'number') {
        item.range = { min: limits.range.min, max: limits.range.max }
      }
      if (limits && Array.isArray(limits.zones)) {
        item.zones = limits.zones
      }
      // Re-validate the whole field after applying limits.
      const field = Object.assign({}, item, { paths: item.paths || { value: item.path } })
      const result = fieldSchema.validateField(field, manifest)
      if (!result.ok) {
        throw httpError(422, 'field_invalid', 'limits failed manifest validation', { errors: result.errors })
      }
    })
  }

  listDiscoveredDevices () {
    const registered = this.store.registry.devices
    const addressOwners = {}
    const observedAt = now()
    for (const device of Object.values(this.store.discovery.devices || {})) {
      if (device.supersededBy) continue
      if (isStaleDiscovery(device, observedAt)) continue
      const key = discoveryAddressKey(device)
      if (!key) continue
      if (!addressOwners[key]) addressOwners[key] = []
      addressOwners[key].push(device.deviceId)
    }
    return {
      devices: Object.values(this.store.discovery.devices || {})
        .map((device) => {
          const ageMs = device.lastSeen
            ? Date.now() - Date.parse(device.lastSeen)
            : Number.POSITIVE_INFINITY
          const key = discoveryAddressKey(device)
          const superseded = Boolean(device.supersededBy)
          const duplicateIds = key && !superseded
            ? (addressOwners[key] || []).filter((id) => id !== device.deviceId)
            : []
          const conflict = duplicateIds.length > 0
            ? { type: 'address', deviceIds: duplicateIds }
            : null
          return {
            ...device,
            registered: Boolean(registered[device.deviceId]),
            ageMs,
            stale: !Number.isFinite(ageMs) || ageMs > 60000,
            conflict,
            duplicate: Boolean(conflict),
            superseded
          }
        })
        .sort((a, b) => {
          const aTime = Date.parse(a.lastSeen || 0)
          const bTime = Date.parse(b.lastSeen || 0)
          return bTime - aTime || a.deviceId.localeCompare(b.deviceId)
        })
    }
  }

  announceDiscoveredDevice (body, auth) {
    if (this.options.auth.mode !== 'disabled') {
      const ok = auth && (
        auth.bearer === this.options.auth.devToken ||
        auth.provision === this.options.auth.provisionToken
      )
      if (!ok) throw httpError(401, 'unauthorized', 'discovery announcement requires manager token')
    }
    return this.recordDiscoveredDevice(body)
  }

  async scanForDevices (body) {
    body = body || {}
    const method = String(body.method || 'ip').toLowerCase()
    if (method === 'ble') {
      return {
        method,
        status: 'unsupported',
        message: 'BLE scanning is not available in this SignalK Node runtime',
        scanned: 0,
        found: 0,
        devices: []
      }
    }
    if (method !== 'ip') {
      throw httpError(400, 'invalid_request', 'scan method must be ip or ble')
    }

    const ports = parseScanPorts(body.ports || body.port || 80)
    const timeoutMs = Math.max(250, Math.min(Number(body.timeoutMs || 900), 5000))
    const concurrency = Math.max(1, Math.min(Number(body.concurrency || 24), 64))
    const candidates = scanCandidates(body.target || body.targets, this.store, 512)
    const results = []
    let scanned = 0

    await mapLimit(candidates, concurrency, async (host) => {
      for (const port of ports) {
        scanned++
        const device = await this.probeHttpDevice(host, port, timeoutMs)
        if (!device) continue
        const recorded = this.recordDiscoveredDevice(device)
        results.push(recorded.device)
        break
      }
    })

    return {
      method,
      status: 'ok',
      scanned,
      found: results.length,
      devices: results
    }
  }

  async probeHttpDevice (host, port, timeoutMs) {
    const auth = this.deviceWebAuth()
    const base = `http://${host}:${port}`
    let state
    try {
      state = await httpGetJson(`${base}/api/state`, auth, timeoutMs)
    } catch (err) {
      return null
    }
    return discoveryDeviceFromState(state, host, port)
  }

  async registerDeviceFromSignalK (body) {
    body = body || {}
    const address = String(body.address || body.host || '').trim()
    const port = Number(body.port || 80)
    if (!address) throw httpError(400, 'invalid_request', 'device address is required')
    if (!Number.isInteger(port) || port <= 0 || port > 65535) {
      throw httpError(400, 'invalid_request', 'device port is invalid')
    }

    const probed = await this.probeHttpDevice(address, port, Number(body.timeoutMs || 1200))
    const deviceId = sanitizeDeviceId(
      body.deviceId ||
      body.id ||
      (probed && (probed.deviceId || probed.id))
    )
    if (!deviceId) {
      throw httpError(400, 'invalid_request', 'device id is required when probe cannot read /api/state')
    }

    this.recordDiscoveredDevice({
      ...(probed || {}),
      id: deviceId,
      deviceId,
      name: body.name || (probed && probed.name) || deviceId,
      role: body.role || (probed && probed.role) || 'display',
      location: body.location || (probed && probed.location) || null,
      source: 'signalk-manual',
      address,
      port,
      transport: 'http',
      authRequired: Boolean(this.options.deviceWebAuth.enabled)
    })

    const claimed = this.claimDiscoveredDevice(deviceId, {
      name: body.name,
      role: body.role || 'display',
      location: body.location,
      profileId: body.profileId || body.profile || 'default',
      sendReload: Boolean(body.sendReload),
      issueToken: true
    })

    let deviceCommand = null
    if (body.sendManagerRegister) {
      const managerUrl = String(body.managerUrl || '').trim()
      if (!managerUrl) {
        throw httpError(400, 'invalid_request', 'managerUrl is required when sending manager-register')
      }
      deviceCommand = await postDeviceCommand(address, port, `manager-register ${managerUrl}`, this.deviceWebAuth())
    }

    return {
      status: claimed.status,
      deviceId,
      claimed,
      deviceCommand
    }
  }

  recordDiscoveredDevice (body) {
    const incoming = normalizeDiscoveryBody(body)
    const id = sanitizeDeviceId(incoming.id || incoming.deviceId)
    if (!id) throw httpError(400, 'invalid_request', 'device.id is required')
    const existing = this.store.discovery.devices[id] || {}
    const address = incoming.address || incoming.ip || null
    const previousAddresses = Array.isArray(existing.previousAddresses)
      ? existing.previousAddresses.slice()
      : []
    if (existing.address && address && existing.address !== address &&
        !previousAddresses.includes(existing.address)) {
      previousAddresses.unshift(existing.address)
      previousAddresses.splice(5)
    }
    const observedAt = now()

    const record = {
      deviceId: id,
      name: incoming.name || id,
      role: incoming.role || 'display',
      location: incoming.location || null,
      firstSeen: existing.firstSeen || observedAt,
      lastSeen: observedAt,
      seenCount: Number(existing.seenCount || 0) + 1,
      source: incoming.source || incoming.discoverySource || 'announcement',
      address,
      previousAddresses,
      port: Number(incoming.port || 80),
      transport: incoming.transport || 'http',
      services: incoming.services || [],
      mdns: incoming.mdns || null,
      display: incoming.display || null,
      firmware: incoming.firmware || null,
      board: incoming.board || null,
      authRequired: Boolean(incoming.authRequired),
      auth: {
        required: Boolean(incoming.authRequired),
        mode: incoming.authRequired ? 'basic' : 'none'
      },
      capabilities: incoming.capabilities || {},
      replacedDeviceIds: Array.isArray(existing.replacedDeviceIds)
        ? existing.replacedDeviceIds.slice()
        : []
    }
    const replacedDeviceIds = this.supersedeStaleDiscoveryRecords(record, observedAt)
    for (const replacedId of replacedDeviceIds) {
      if (!record.replacedDeviceIds.includes(replacedId)) {
        record.replacedDeviceIds.push(replacedId)
      }
    }
    if (existing.supersededBy === id) {
      delete record.supersededBy
      delete record.supersededAt
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

  supersedeStaleDiscoveryRecords (fresh, observedAt) {
    const key = discoveryAddressKey(fresh)
    if (!key) return []
    const replaced = []
    for (const [otherId, other] of Object.entries(this.store.discovery.devices || {})) {
      if (otherId === fresh.deviceId || other.supersededBy) continue
      if (discoveryAddressKey(other) !== key) continue
      if (!isStaleDiscovery(other, observedAt)) continue
      other.supersededBy = fresh.deviceId
      other.supersededAt = observedAt
      for (const priorId of other.replacedDeviceIds || []) {
        if (!replaced.includes(priorId)) replaced.push(priorId)
        const prior = this.store.discovery.devices[priorId]
        if (prior) {
          prior.supersededBy = fresh.deviceId
          prior.supersededAt = observedAt
        }
      }
      replaced.push(otherId)
    }
    return replaced
  }

  claimDiscoveredDevice (id, body) {
    body = body || {}
    const deviceId = sanitizeDeviceId(id)
    if (!deviceId) throw httpError(400, 'invalid_request', 'device id is required')
    const discovered = this.store.discovery.devices[deviceId]
    if (!discovered) throw httpError(404, 'device_not_found', 'discovered device not found')

    const existing = this.store.registry.devices[deviceId] || {}
    const created = !existing.id
    const identity = {
      ...(existing.identity || {}),
      id: deviceId,
      name: body.name || discovered.name || existing.name || deviceId,
      board: discovered.board || discovered.board_id || existing.board || null,
      display: discovered.display || existing.display || null,
      firmware: discovered.firmware || existing.firmware || null,
      capabilities: discovered.capabilities || existing.capabilities || {}
    }
    const claimed = {
      ...existing,
      id: deviceId,
      name: body.name || existing.name || discovered.name || deviceId,
      claimed: true,
      claim: {
        claimedAt: existing.claim && existing.claim.claimedAt
          ? existing.claim.claimedAt
          : now(),
        updatedAt: now(),
        source: 'discovery',
        profileId: body.profileId || existing.assignedProfile || null
      },
      role: body.role || existing.role || discovered.role || 'display',
      location: body.location || existing.location || discovered.location || null,
      firstSeen: existing.firstSeen || discovered.firstSeen || now(),
      lastSeen: existing.lastSeen || discovered.lastSeen || now(),
      identity,
      capabilities: discovered.capabilities || existing.capabilities || {},
      display: discovered.display || existing.display || null,
      firmware: discovered.firmware || existing.firmware || null,
      board: discovered.board || existing.board || null,
      discovery: {
        source: discovered.source || 'announcement',
        address: discovered.address || null,
        port: Number(discovered.port || 80),
        lastSeen: discovered.lastSeen || null,
        services: discovered.services || [],
        authRequired: Boolean(discovered.authRequired)
      },
      auth: {
        ...(existing.auth || {}),
        web: {
          ...(existing.auth && existing.auth.web ? existing.auth.web : {}),
          required: Boolean(discovered.authRequired),
          mode: discovered.authRequired ? 'basic' : 'none',
          username: this.options.deviceWebAuth.username,
          passwordSet: Boolean(this.options.deviceWebAuth.password)
        },
        manager: {
          mode: this.options.auth.mode
        }
      },
      assignedProfile: body.profileId || existing.assignedProfile || null,
      overrides: existing.overrides || {},
      status: existing.status || {}
    }
    if (!claimed.assignedProfile) {
      claimed.assignedProfile = this.matchProfileForDevice(claimed) || 'default'
    }
    if (!this.store.profiles.profiles[claimed.assignedProfile]) {
      throw httpError(404, 'profile_not_found', 'profile not found')
    }
    let issuedToken = null
    if (body.issueToken && !created) {
      issuedToken = this.options.auth.mode === 'dev-shared-token'
        ? this.options.auth.devToken
        : randomId('dev')
      claimed.deviceTokenHash = hashToken(issuedToken)
      claimed.deviceTokenId = this.options.auth.mode === 'dev-shared-token' ? 'dev' : randomId('tok')
      delete claimed.deviceToken
    } else if (!deviceHasToken(claimed) && this.options.auth.mode === 'dev-shared-token') {
      issuedToken = this.options.auth.devToken
      claimed.deviceTokenHash = hashToken(issuedToken)
      claimed.deviceTokenId = 'dev'
      delete claimed.deviceToken
    } else if (!deviceHasToken(claimed)) {
      issuedToken = randomId('dev')
      claimed.deviceTokenHash = hashToken(issuedToken)
      claimed.deviceTokenId = randomId('tok')
      delete claimed.deviceToken
    } else if (claimed.deviceToken) {
      claimed.deviceTokenHash = hashToken(claimed.deviceToken)
      delete claimed.deviceToken
    }
    claimed.networkIdentity = this.resolveNetworkIdentity(claimed, identity)
    claimed.claim.profileId = claimed.assignedProfile
    claimed.updatedAt = now()

    this.store.registry.devices[deviceId] = claimed
    this.store.saveRegistry()
    this.store.audit(created ? 'device.claimed' : 'device.reclaimed', deviceId, {
      profileId: claimed.assignedProfile
    })

    let command = null
    if (body.sendReload) command = this.queueConfigReload(deviceId)

    return {
      status: created ? 'claimed' : 'updated',
      deviceId,
      claimed: true,
      assignedProfile: claimed.assignedProfile,
      deviceToken: created || body.issueToken ? issuedToken : undefined,
      tokenId: claimed.deviceTokenId,
      config: {
        version: this.configVersion(claimed),
        hash: this.generateConfig(deviceId).hash,
        url: `/plugins/espdisp-manager/devices/${deviceId}/config`
      },
      command
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
    const command = body.sendReload ? this.queueConfigReload(id) : null
    return {
      deviceId: id,
      assignedProfile: profileId,
      config: {
        version: this.configVersion(device),
        hash: this.generateConfig(id).hash
      },
      command
    }
  }

  applyProfile (profileId, body) {
    body = body || {}
    if (!this.store.profiles.profiles[profileId]) {
      throw httpError(404, 'profile_not_found', 'profile not found')
    }
    const deviceIds = Array.isArray(body.deviceIds)
      ? body.deviceIds
      : (body.deviceIds ? [body.deviceIds] : [])
    if (deviceIds.length === 0) throw httpError(400, 'invalid_request', 'deviceIds is required')
    const results = deviceIds.map((deviceId) => {
      const device = this.getDevice(deviceId)
      return this.assignProfile(deviceId, {
        profileId,
        overrides: body.clearOverrides ? {} : device.overrides,
        sendReload: Boolean(body.sendReload)
      })
    })
    return {
      profileId,
      count: results.length,
      results
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
    if (body.webAuth) device.webAuth = body.webAuth
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
      webAuth: {
        enabled: this.options.deviceWebAuth.enabled,
        username: this.options.deviceWebAuth.username,
        password: this.options.deviceWebAuth.password
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

  deviceWebAuth (device) {
    return {
      enabled: this.options.deviceWebAuth.enabled,
      username: this.options.deviceWebAuth.username,
      password: this.options.deviceWebAuth.password
    }
  }

  // All addresses we know for a device, in preference order, deduped. A stale
  // cached IP (status.network.ip / lastResolvedAddress) can outlive its DHCP
  // lease; keeping the mDNS FQDN as a fallback lets fetchDeviceJson recover
  // when the IP no longer resolves to the device.
  deviceHttpCandidates (device) {
    const ni = (device && device.networkIdentity) || {}
    const raw = [
      device && device.status && device.status.network && device.status.network.ip,
      ni.lastResolvedAddress,
      ni.currentFqdn,
      ni.desiredFqdn
    ].filter(Boolean)
    const seen = new Set()
    const urls = []
    for (const addr of raw) {
      const url = `http://${addr}:80`
      if (!seen.has(url)) { seen.add(url); urls.push(url) }
    }
    if (urls.length === 0) {
      throw httpError(409, 'device_address_unknown', 'device address is unknown')
    }
    return urls
  }

  deviceHttpBase (device) {
    return this.deviceHttpCandidates(device)[0]
  }

  // Record an address that just worked so the next fetch tries it first.
  noteResolvedAddress (device, host) {
    if (!device || !host) return
    device.networkIdentity = device.networkIdentity || {}
    device.networkIdentity.lastResolvedAddress = host
    // Devices live in the registry store; saveRegistry() is the persist call
    // used throughout this class.
    if (this.store && typeof this.store.saveRegistry === 'function') {
      this.store.saveRegistry()
    }
  }

  getLiveStatus (id) {
    return this.fetchDeviceJson(this.getDevice(id), '/api/state')
  }

  getLiveLogs (id, since) {
    const query = since ? `?since=${encodeURIComponent(since)}` : ''
    return this.fetchDeviceJson(this.getDevice(id), `/api/logs${query}`)
  }

  // Indirection so tests can stub the transport; defaults to the module
  // httpGetJson. Returns a Promise<json>.
  _httpGetJson (url, auth) {
    return httpGetJson(url, auth)
  }

  async fetchDeviceJson (device, path) {
    const auth = this.deviceWebAuth(device)
    const candidates = this.deviceHttpCandidates(device)
    let lastErr
    for (let i = 0; i < candidates.length; i++) {
      const base = candidates[i]
      try {
        const json = await this._httpGetJson(`${base}${path}`, auth)
        if (i > 0) {
          const host = base.replace(/^http:\/\//, '').replace(/:\d+$/, '')
          this.noteResolvedAddress(device, host)
        }
        return json
      } catch (err) {
        lastErr = err
        if (this.app && this.app.debug) {
          this.app.debug(`espdisp device fetch ${base}${path} failed: ${err.message}`)
        }
      }
    }
    throw lastErr
  }

  listProfiles () {
    // Auto-seed the default profile's layout on first inspection if
    // the operator hasn't touched it yet. Without this, every fresh
    // install shows an empty layout-editor page with no screens to
    // edit - exactly the "layouts not rendered" complaint. We only
    // seed if the layout is missing OR has no screens; once the
    // operator has any screens we never overwrite.
    const def = this.store.profiles.profiles.default
    if (def && this.shouldSeedDefaultLayout(def)) {
      this.seedDefaultLayout(def)
    }
    return { profiles: Object.values(this.store.profiles.profiles) }
  }

  shouldSeedDefaultLayout (profile) {
    if (!profile || !profile.config) return true
    const layout = profile.config.layout
    if (!layout) return true
    if (!Array.isArray(layout.screens)) return true
    return layout.screens.length === 0
  }

  seedDefaultLayout (profile) {
    // The default profile is display-class-agnostic at registration,
    // so we seed with the 4-tile sunton (square) catalogue - it
    // degrades gracefully on wide displays (still renders, just
    // empty space). Wide-display devices get their own profile
    // recommendation in the editor's display-class selector.
    const presets = require('./screen-presets')
    const screens = presets.getPresetsForClass('sunton-480')
    if (!profile.config) profile.config = {}
    profile.config.layout = { version: 1, screens }
    profile.version = Number(profile.version || 1) + 1
    profile.updatedAt = now()
    profile.hash = sha256Json(profile.config)
    this.store.profiles.profiles[profile.id] = profile
    this.store.saveProfiles()
    this.store.audit('profile.seeded', profile.id, { screens: screens.length })
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

  // Device cleanup: remove a device from the registry + any pending
  // commands + its discovery record. Idempotent - missing device is
  // not an error so the form can return the operator to a clean
  // list without races.
  deleteDevice (id) {
    if (!this.store.registry.devices[id]) return { id, removed: false }
    delete this.store.registry.devices[id]
    // Drop pending commands aimed at this device.
    if (this.store.commands && this.store.commands.queues) {
      delete this.store.commands.queues[id]
    }
    // Drop discovery record so it doesn't reappear with stale state.
    if (this.store.discovery && this.store.discovery.devices) {
      Object.keys(this.store.discovery.devices).forEach((key) => {
        const rec = this.store.discovery.devices[key]
        if (rec && rec.deviceId === id) delete this.store.discovery.devices[key]
      })
    }
    this.store.saveRegistry()
    if (this.store.saveCommands) this.store.saveCommands()
    if (this.store.saveDiscovery) this.store.saveDiscovery()
    this.store.audit('device.deleted', id)
    return { id, removed: true }
  }

  // Bulk cleanup: remove every registered device that isn't currently online
  // (registries accumulate stale/mock entries — see the network-conflict fix).
  deleteOfflineDevices () {
    const ids = Object.values(this.store.registry.devices)
      .filter((device) => !this.isDeviceOnline(device))
      .map((device) => device.id)
    ids.forEach((id) => this.deleteDevice(id))
    return { removed: ids.length, remaining: Object.keys(this.store.registry.devices).length }
  }

  // Bulk cleanup: remove ALL registered devices (full reset of the list).
  clearAllDevices () {
    const ids = Object.keys(this.store.registry.devices)
    ids.forEach((id) => this.deleteDevice(id))
    return { removed: ids.length, remaining: Object.keys(this.store.registry.devices).length }
  }

  // Firmware artifact cleanup: drop an artifact from the catalog.
  // Active jobs that referenced it keep going to completion; we
  // just stop offering it to new updates.
  deleteFirmwareArtifact (artifactId) {
    if (!this.store.firmware || !this.store.firmware.artifacts) {
      return { artifactId, removed: false }
    }
    const before = this.store.firmware.artifacts.length
    this.store.firmware.artifacts = this.store.firmware.artifacts
      .filter((a) => a.artifactId !== artifactId)
    const removed = before !== this.store.firmware.artifacts.length
    if (removed) {
      this.store.saveFirmware()
      this.store.audit('firmware.artifact.deleted', artifactId)
    }
    return { artifactId, removed }
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
    // Push-live: a config.reload command is normally picked up on the device's
    // command poll. Also emit a configPush SignalK delta so a subscribed device
    // fetches the new generated config immediately (the firmware's onText sees
    // it and requests a config fetch). Best-effort; the poll remains the
    // fallback.
    if (command.type === 'config.reload' && this.app && typeof this.app.handleMessage === 'function') {
      try {
        this.app.handleMessage('espdisp-manager', {
          updates: [{
            values: [{
              path: 'network.espdisp.configPush',
              value: { deviceId: id, at: command.createdAt, payload: command.payload }
            }]
          }]
        })
      } catch (e) {
        if (this.app.debug) this.app.debug(`configPush emit failed: ${e.message}`)
      }
    }
    // Protocol-routed screen change. A `screen.set` is the manager's
    // cross-device view switch; when the target speaks the espdisp control
    // protocol over IP we drive it directly with attach->switch->detach via
    // the shared @espdisp/proto client (so the target shows this plugin's
    // colored frame), in addition to queueing the command. The command queue
    // stays as the fallback for devices that do not yet speak the protocol.
    const device = this.store.registry.devices[id]
    if (command.type === 'screen.set' && this.options.control.enabled &&
        ProtoControl.speaksProtocol(device)) {
      const viewId = command.payload && (command.payload.screen || command.payload.viewId || command.payload.view)
      if (viewId) {
        // Fire-and-forget: never block command creation on a network round-trip.
        this.protoSetScreen(id, String(viewId)).catch((e) => {
          if (this.app && this.app.debug) this.app.debug(`proto screen.set failed: ${e.message}`)
        })
      }
    }
    return command
  }

  // Build the descriptor proto-control needs for a registered device: a base
  // URL and the device's advertised protocol version / transports. Returns
  // null when the device has no known address.
  protoDeviceDescriptor (device) {
    let base
    try {
      base = this.deviceHttpBase(device)
    } catch (err) {
      return null
    }
    return {
      deviceId: device.id,
      base,
      pv: (device.proto && device.proto.pv) || device.pv || null,
      transports: (device.proto && device.proto.transports) ||
        device.transports || ['ip']
    }
  }

  // Discover the protocol DeviceRecord for every registered device that has a
  // known address, filtering out version-incompatible targets. Returns the
  // array of successful describeDevice results (each annotated with pv +
  // transports), and records pv/transports back onto the registry so the
  // registry "speaks the protocol".
  async protoDiscover () {
    const descriptors = Object.values(this.store.registry.devices)
      .map((device) => this.protoDeviceDescriptor(device))
      .filter(Boolean)
    const found = await this.protoControl.discover(descriptors)
    let changed = false
    for (const desc of found) {
      const device = this.store.registry.devices[desc.record.deviceId] ||
        Object.values(this.store.registry.devices).find((d) => this.deviceBaseSafe(d) === desc.base)
      if (device) {
        device.proto = {
          pv: desc.pv,
          transports: desc.transports,
          discoveredAt: now()
        }
        changed = true
      }
    }
    if (changed) this.store.saveRegistry()
    return found
  }

  deviceBaseSafe (device) {
    try { return this.deviceHttpBase(device) } catch (e) { return null }
  }

  // Drive a single device's screen change through the control protocol
  // (attach -> switch -> detach). Returns the proto-control result.
  async protoSetScreen (id, viewId) {
    const device = this.getDevice(id)
    const descriptor = this.protoDeviceDescriptor(device)
    if (!descriptor) return { ok: false, reason: 'no_address' }
    const result = await this.protoControl.setScreen(descriptor, viewId)
    if (result.ok) {
      this.store.audit('device.screen.set', id, { viewId, via: 'control-protocol' })
    }
    return result
  }

  queueConfigReload (id) {
    const config = this.generateConfig(id)
    return this.createCommand(id, {
      type: 'config.reload',
      payload: {
        version: config.version,
        hash: config.hash,
        url: `/plugins/espdisp-manager/devices/${id}/config`
      }
    })
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
    device.deviceTokenHash = hashToken(token)
    delete device.deviceToken
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
    device.deviceTokenHash = null
    delete device.deviceToken
    device.deviceTokenId = null
    device.updatedAt = now()
    this.store.saveRegistry()
    this.store.audit('device.token.revoked', id)
    return { deviceId: id, revoked: true }
  }

  listFirmware () {
    return clone(this.store.firmware)
  }

  firmwareUpgradeMatrix () {
    const artifacts = this.store.firmware.artifacts
      .slice()
      .sort(compareFirmwareArtifacts)
    const jobs = this.store.jobs.jobs
    const devices = Object.values(this.store.registry.devices)
      .sort((a, b) => a.id.localeCompare(b.id))
      .map((device) => {
        const currentVersion = firmwareVersionFromDevice(device)
        const board = device.board ||
          (device.identity && (device.identity.board || device.identity.board_id)) ||
          null
        const chip = device.identity && device.identity.chip ? device.identity.chip : null
        const compatibleArtifacts = artifacts
          .map((artifact) => {
            const compatibility = this.firmwareCompatibility(device, artifact)
            return {
              ...clone(artifact),
              compatible: compatibility.compatible,
              reason: compatibility.reason,
              sameVersion: Boolean(currentVersion && artifact.firmware && artifact.firmware.version === currentVersion)
            }
          })
          .filter((artifact) => artifact.compatible)
        const availableArtifacts = compatibleArtifacts.filter((artifact) => !artifact.sameVersion)
        const activeJobs = jobs
          .filter((job) => job.deviceId === device.id && !['confirmed', 'failed', 'cancelled'].includes(job.status))
          .sort((a, b) => String(b.createdAt || '').localeCompare(String(a.createdAt || '')))
        const latest = availableArtifacts[0] || null
        return {
          deviceId: device.id,
          name: device.name || device.id,
          online: isDeviceOnline(device, this.options.heartbeatMs),
          board,
          chip,
          currentVersion,
          firmware: clone(device.firmware || null),
          compatibleArtifacts,
          availableArtifacts,
          latestArtifact: latest,
          upgradable: Boolean(latest),
          activeJobs,
          status: activeJobs.length > 0
            ? 'update queued'
            : latest
              ? 'upgrade available'
              : compatibleArtifacts.length > 0
                ? 'current'
                : 'no compatible artifact'
        }
      })
    return {
      generatedAt: now(),
      devices,
      artifacts: clone(artifacts),
      upgradable: devices.filter((device) => device.upgradable).length
    }
  }

  firmwareCompatibility (device, artifact) {
    try {
      this.validateFirmwareCompatibility(device, artifact)
      return { compatible: true }
    } catch (err) {
      return {
        compatible: false,
        reason: err && err.payload && err.payload.error
          ? err.payload.error.message
          : err.message
      }
    }
  }

  async refreshFirmwareFromGithub (fetchImpl) {
    const cfg = this.options.firmware.github
    if (!cfg.enabled) return this.listFirmware()
    const doFetch = fetchImpl || cfg.fetch || globalThis.fetch
    if (typeof doFetch !== 'function') {
      throw httpError(500, 'github_fetch_unavailable', 'fetch is not available in this Node runtime')
    }

    const release = await fetchGithubRelease(cfg, doFetch)
    const assets = Array.isArray(release.assets) ? release.assets : []
    const sumsAsset = assets.find((asset) => asset.name === 'SHA256SUMS')
    const sums = sumsAsset && sumsAsset.browser_download_url
      ? parseSha256Sums(await fetchText(sumsAsset.browser_download_url, doFetch))
      : {}

    const targets = cfg.targets && cfg.targets.length
      ? cfg.targets.map(normalizeFirmwareTarget)
      : SUPPORTED_FIRMWARE_TARGETS
    const imported = []
    for (const entry of targets) {
      const target = entry.target
      const assetName = `${target}-merged_firmware.bin`
      const asset = assets.find((candidate) => candidate.name === assetName)
      if (!asset || !asset.browser_download_url) continue
      const sha = sums[assetName]
      if (!sha) continue
      const tag = release.tag_name || release.name || 'unknown'
      const version = String(tag).replace(/^v/, '')
      const artifact = {
        artifactId: `github-${sanitizeArtifactId(tag)}-${target}`,
        source: {
          type: 'github-release',
          owner: cfg.owner,
          repo: cfg.repo,
          tag,
          htmlUrl: release.html_url || null,
          assetUrl: asset.browser_download_url
        },
        vendor: { id: 'navado', name: 'Navado', trust: { level: 'github-release', allowUnsigned: false } },
        product: { id: 'espdisp', name: 'ESP Display' },
        firmware: {
          name: 'espdisp',
          version,
          channel: release.prerelease ? 'prerelease' : 'stable'
        },
        compatibility: {
          boards: [entry.board],
          releaseTarget: target,
          chip: 'ESP32-S3',
          minFlashBytes: 16777216,
          requiresPsram: true,
          partitionScheme: 'ota_16mb'
        },
        file: {
          name: asset.name,
          size: Number(asset.size || 0),
          sha256: `sha256:${sha}`,
          url: asset.browser_download_url,
          contentType: asset.content_type || 'application/octet-stream'
        },
        signing: { signed: false, checksums: 'SHA256SUMS' },
        uploadedAt: now()
      }
      upsertFirmwareArtifact(this.store.firmware.artifacts, artifact)
      imported.push(artifact)
    }
    this.store.firmware.github = {
      owner: cfg.owner,
      repo: cfg.repo,
      release: release.tag_name || null,
      checkedAt: now(),
      imported: imported.length
    }
    this.store.saveFirmware()
    this.store.audit('firmware.github.refresh', `${cfg.owner}/${cfg.repo}`, {
      release: release.tag_name || null,
      imported: imported.map((artifact) => artifact.artifactId)
    })
    return { ...this.listFirmware(), refreshed: this.store.firmware.github }
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
        url: artifact.file && artifact.file.url
          ? artifact.file.url
          : `/plugins/espdisp-manager/firmware/download/${job.jobId}`,
        sha256: firmwarePayloadSha256(artifact.file && artifact.file.sha256),
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
      if (artifact.file && artifact.file.url) return { job, artifact }
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
      const board = device.board || (device.identity && (device.identity.board || device.identity.board_id))
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
      tokenHash: hashToken(token),
      createdAt: now(),
      expiresAt: new Date(Date.now() + ttlMs).toISOString(),
      usesRemaining: Number(body.uses || 1),
      note: body.note || ''
    }
    this.store.provisioning.tokens.push(record)
    this.store.saveProvisioning()
    this.store.audit('provisioning.token.created', record.id, { expiresAt: record.expiresAt })
    return { ...record, token }
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
      provisioned: deviceHasToken(device),
      claimed: Boolean(device.claimed),
      authMode: this.options.auth.mode,
      webAuth: device.auth && device.auth.web
        ? device.auth.web
        : {
            required: false,
            mode: 'none',
            username: this.options.deviceWebAuth.username,
            passwordSet: Boolean(this.options.deviceWebAuth.password)
          }
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

  // True only when the device was seen within the online window. Used so a
  // hostname "conflict" against a long-dead / mock registration doesn't flag a
  // live device as network-conflict (registries accumulate stale duplicates).
  isDeviceOnline (device) {
    if (!device || !device.lastSeen) return false
    const lastSeenMs = Date.parse(device.lastSeen)
    if (!(lastSeenMs > 0)) return false
    const onlineWindowMs = Math.max(this.options.heartbeatMs * 3, 15000)
    return Date.now() - lastSeenMs <= onlineWindowMs
  }

  hostnameConflict (id, hostname) {
    // A conflict is only real when ANOTHER currently-online device wants the
    // same hostname; stale/offline registrations are not actively competing for
    // it on the network.
    return Object.values(this.store.registry.devices).some((device) => {
      return device.id !== id &&
        device.networkIdentity &&
        device.networkIdentity.desiredHostname === hostname &&
        this.isDeviceOnline(device)
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
    if (device && auth.bearer && this.verifyDeviceToken(device, auth.bearer)) return
    if (provisioningAllowed && auth.provision && this.consumeProvisioningToken(auth.provision)) return
    if (provisioningAllowed && auth.provision && auth.provision === this.options.auth.provisionToken) return
    throw httpError(401, 'unauthorized', 'invalid device token')
  }

  verifyDeviceToken (device, token) {
    if (!device || !token) return false
    if (device.deviceTokenHash && timingSafeEqual(device.deviceTokenHash, hashToken(token))) return true
    if (device.deviceToken && timingSafeEqual(hashToken(device.deviceToken), hashToken(token))) {
      device.deviceTokenHash = hashToken(device.deviceToken)
      delete device.deviceToken
      this.store.saveRegistry()
      return true
    }
    return false
  }

  consumeProvisioningToken (token) {
    const nowMs = Date.now()
    const record = this.store.provisioning.tokens.find((item) => {
      return verifyStoredToken(item, token) &&
        item.usesRemaining > 0 &&
        Date.parse(item.expiresAt) > nowMs
    })
    if (!record) return false
    if (record.token) {
      record.tokenHash = hashToken(record.token)
      delete record.token
    }
    record.usesRemaining -= 1
    this.store.saveProvisioning()
    this.store.audit('provisioning.token.used', record.id, { usesRemaining: record.usesRemaining })
    return true
  }
}

function normalizeOptions (options) {
  return {
    serverId: options.serverId || 'signalk-espdisp-manager',
    heartbeatMs: Math.max(Number(options.heartbeatMs || 30000), 30000),
    commandPollMs: Math.max(Number(options.commandPollMs || 15000), 15000),
    auth: {
      mode: options.auth && options.auth.mode ? options.auth.mode : 'dev-shared-token',
      devToken: options.auth && options.auth.devToken ? options.auth.devToken : 'espdisp-dev',
      provisionToken: options.auth && options.auth.provisionToken ? options.auth.provisionToken : 'espdisp-provision'
    },
    signalk: {
      host: options.signalk && options.signalk.host ? options.signalk.host : 'signalk.local',
      port: Number(options.signalk && options.signalk.port ? options.signalk.port : 3000)
    },
    // Control protocol (espdisp control-1): how this plugin presents itself
    // when it attaches to a target as a controller (the colored "controlled"
    // frame on the display). `enabled` routes screen.set through the protocol
    // when the device speaks it; the registry/command-queue path remains as a
    // fallback for devices that do not.
    control: {
      enabled: !(options.control && options.control.enabled === false),
      controllerId: options.control && options.control.controllerId
        ? options.control.controllerId
        : 'plugin:espdisp-manager',
      name: options.control && options.control.name ? options.control.name : 'SignalK Manager',
      color: options.control && options.control.color ? options.control.color : '#ff9800',
      key: options.control && options.control.key ? options.control.key : ''
    },
    deviceWebAuth: {
      enabled: !(options.deviceWebAuth && options.deviceWebAuth.enabled === false),
      username: options.deviceWebAuth && options.deviceWebAuth.username ? options.deviceWebAuth.username : 'espdisp',
      password: options.deviceWebAuth && options.deviceWebAuth.password ? options.deviceWebAuth.password : 'espdisp-dev'
    },
    discoveryUdp: {
      enabled: !(options.discoveryUdp && options.discoveryUdp.enabled === false),
      bind: options.discoveryUdp && options.discoveryUdp.bind ? options.discoveryUdp.bind : '0.0.0.0',
      port: Number(options.discoveryUdp && Object.prototype.hasOwnProperty.call(options.discoveryUdp, 'port') ? options.discoveryUdp.port : 34300),
      host: options.discoveryUdp && options.discoveryUdp.host ? options.discoveryUdp.host : ''
    },
    deviceDiscoveryUdp: {
      enabled: !(options.deviceDiscoveryUdp && options.deviceDiscoveryUdp.enabled === false),
      bind: options.deviceDiscoveryUdp && options.deviceDiscoveryUdp.bind ? options.deviceDiscoveryUdp.bind : '0.0.0.0',
      port: Number(options.deviceDiscoveryUdp && Object.prototype.hasOwnProperty.call(options.deviceDiscoveryUdp, 'port') ? options.deviceDiscoveryUdp.port : 34301)
    },
    firmware: {
      github: {
        enabled: !(options.firmware && options.firmware.github && options.firmware.github.enabled === false),
        owner: options.firmware && options.firmware.github && options.firmware.github.owner ? options.firmware.github.owner : 'navado',
        repo: options.firmware && options.firmware.github && options.firmware.github.repo ? options.firmware.github.repo : 'esp32-boat-mfd',
        apiBase: options.firmware && options.firmware.github && options.firmware.github.apiBase ? options.firmware.github.apiBase : 'https://api.github.com',
        includePrereleases: Boolean(options.firmware && options.firmware.github && options.firmware.github.includePrereleases),
        targets: options.firmware && options.firmware.github && Array.isArray(options.firmware.github.targets) ? options.firmware.github.targets : SUPPORTED_FIRMWARE_TARGETS,
        fetch: options.firmware && options.firmware.github && options.firmware.github.fetch
      }
    },
    network: {
      domain: options.network && options.network.domain ? options.network.domain : 'local',
      hostnamePrefix: options.network && options.network.hostnamePrefix ? options.network.hostnamePrefix : 'espdisp',
      namingPolicy: options.network && options.network.namingPolicy ? options.network.namingPolicy : 'device-id',
      mdns: {
        enabled: !(options.network && options.network.mdns && options.network.mdns.enabled === false),
        browser: !(options.network && options.network.mdns && options.network.mdns.browser === false),
        advertiseManager: !(options.network && options.network.mdns && options.network.mdns.advertiseManager === false),
        bind: options.network && options.network.mdns && options.network.mdns.bind ? options.network.mdns.bind : '0.0.0.0',
        port: Number(options.network && options.network.mdns && Object.prototype.hasOwnProperty.call(options.network.mdns, 'port') ? options.network.mdns.port : MDNS_PORT),
        advertiseHost: options.network && options.network.mdns && options.network.mdns.advertiseHost ? options.network.mdns.advertiseHost : '',
        advertiseIntervalMs: Number(options.network && options.network.mdns && options.network.mdns.advertiseIntervalMs ? options.network.mdns.advertiseIntervalMs : 60000)
      }
    }
  }
}

async function fetchGithubRelease (cfg, doFetch) {
  const base = String(cfg.apiBase || 'https://api.github.com').replace(/\/$/, '')
  const path = cfg.includePrereleases
    ? `/repos/${cfg.owner}/${cfg.repo}/releases`
    : `/repos/${cfg.owner}/${cfg.repo}/releases/latest`
  const payload = await fetchJson(`${base}${path}`, doFetch)
  if (Array.isArray(payload)) {
    const release = payload.find((candidate) => !candidate.draft)
    if (!release) throw httpError(404, 'github_release_not_found', 'no GitHub release found')
    return release
  }
  return payload
}

async function fetchJson (url, doFetch) {
  const response = await doFetch(url, {
    headers: { Accept: 'application/vnd.github+json' }
  })
  if (!response || !response.ok) {
    throw httpError(502, 'github_request_failed', `GitHub request failed: ${url}`)
  }
  return response.json()
}

async function fetchText (url, doFetch) {
  const response = await doFetch(url)
  if (!response || !response.ok) {
    throw httpError(502, 'github_request_failed', `GitHub request failed: ${url}`)
  }
  return response.text()
}

function parseSha256Sums (text) {
  const sums = {}
  for (const line of String(text || '').split(/\r?\n/)) {
    const match = line.trim().match(/^([a-fA-F0-9]{64})\s+(.+)$/)
    if (!match) continue
    sums[match[2].replace(/^\.\//, '')] = match[1].toLowerCase()
  }
  return sums
}

function sanitizeArtifactId (value) {
  return String(value || 'unknown').replace(/[^a-zA-Z0-9_.-]/g, '-')
}

function normalizeFirmwareTarget (entry) {
  if (entry && typeof entry === 'object') {
    const target = entry.target || entry.env || entry.id
    return { target, board: entry.board || boardIdFromReleaseTarget(target) }
  }
  return { target: entry, board: boardIdFromReleaseTarget(entry) }
}

function boardIdFromReleaseTarget (target) {
  const known = SUPPORTED_FIRMWARE_TARGETS.find((entry) => entry.target === target)
  if (known) return known.board
  return String(target || '').replace(/-/g, '_')
}

function firmwarePayloadSha256 (value) {
  const sha = String(value || '')
  return sha.startsWith('sha256:') ? sha.slice('sha256:'.length) : sha
}

function upsertFirmwareArtifact (artifacts, artifact) {
  const index = artifacts.findIndex((existing) => existing.artifactId === artifact.artifactId)
  if (index >= 0) {
    artifacts[index] = { ...artifacts[index], ...artifact }
  } else {
    artifacts.push(artifact)
  }
}

function devicesFromMdnsPacket (packet, rinfo) {
  const msg = parseMdnsPacket(packet)
  if (!msg) return []

  const serviceInstances = new Set()
  const instances = new Map()
  const addresses = new Map()
  for (const rr of msg.records) {
    const name = rr.name.toLowerCase()
    if (rr.type === 12 && name === ESPDISP_MDNS_SERVICE) {
      serviceInstances.add(rr.ptr)
      if (!instances.has(rr.ptr)) instances.set(rr.ptr, { instance: rr.ptr })
    } else if (rr.type === 33) {
      const item = instances.get(rr.name) || { instance: rr.name }
      item.port = rr.port
      item.target = rr.target
      instances.set(rr.name, item)
    } else if (rr.type === 16) {
      const item = instances.get(rr.name) || { instance: rr.name }
      item.txt = { ...(item.txt || {}), ...rr.txt }
      instances.set(rr.name, item)
    } else if (rr.type === 1 && rr.address) {
      addresses.set(name, rr.address)
    }
  }

  const out = []
  for (const [instanceName, item] of instances.entries()) {
    const txt = item.txt || {}
    const isEspDisp = serviceInstances.has(instanceName) ||
      txt.proto === '1' ||
      instanceName.toLowerCase().endsWith(`.${ESPDISP_MDNS_SERVICE}`)
    if (!isEspDisp) continue

    const id = sanitizeDeviceId(txt.device_id || txt.deviceId ||
      instanceName.split('.')[0])
    if (!id) continue

    const target = item.target || ''
    const address = target
      ? addresses.get(target.toLowerCase()) || null
      : null
    const display = parseDisplay(txt.display)
    const firmware = txt.firmware || txt.version
      ? { name: txt.firmware || 'espdisp', version: txt.version || null }
      : null
    const authRequired = txt.auth === 'basic'
    out.push({
      id,
      deviceId: id,
      name: id,
      source: 'mdns',
      address: address || (rinfo && rinfo.address) || null,
      port: Number(item.port || 80),
      transport: 'http',
      services: [{ type: '_espdisp._tcp', port: Number(item.port || 80) }],
      mdns: {
        instance: instanceName,
        target: target || null,
        txt
      },
      board: txt.board || null,
      firmware,
      display,
      authRequired,
      capabilities: {}
    })
  }
  return out
}

function parseDisplay (value) {
  if (!value || typeof value !== 'string') return null
  const m = value.match(/^(\d+)x(\d+)$/)
  if (!m) return null
  return { width: Number(m[1]), height: Number(m[2]) }
}

function parseMdnsPacket (packet) {
  if (!Buffer.isBuffer(packet) || packet.length < 12) return null
  const qd = packet.readUInt16BE(4)
  const an = packet.readUInt16BE(6)
  const ns = packet.readUInt16BE(8)
  const ar = packet.readUInt16BE(10)
  let off = 12
  const questions = []
  for (let i = 0; i < qd; i++) {
    const q = readDnsName(packet, off)
    if (!q) return null
    if (q.offset + 4 > packet.length) return null
    questions.push({
      name: q.name,
      type: packet.readUInt16BE(q.offset),
      class: packet.readUInt16BE(q.offset + 2)
    })
    off = q.offset + 4
    if (off > packet.length) return null
  }

  const records = []
  const count = an + ns + ar
  for (let i = 0; i < count; i++) {
    const n = readDnsName(packet, off)
    if (!n || n.offset + 10 > packet.length) return null
    off = n.offset
    const type = packet.readUInt16BE(off)
    const klass = packet.readUInt16BE(off + 2)
    const ttl = packet.readUInt32BE(off + 4)
    const rdlen = packet.readUInt16BE(off + 8)
    off += 10
    if (off + rdlen > packet.length) return null
    const rdata = off
    const end = off + rdlen
    const rr = { name: n.name, type, class: klass, ttl }
    if (type === 12) {
      const ptr = readDnsName(packet, rdata)
      if (ptr) rr.ptr = ptr.name
    } else if (type === 33 && rdlen >= 6) {
      rr.priority = packet.readUInt16BE(rdata)
      rr.weight = packet.readUInt16BE(rdata + 2)
      rr.port = packet.readUInt16BE(rdata + 4)
      const target = readDnsName(packet, rdata + 6)
      if (target) rr.target = target.name
    } else if (type === 16) {
      rr.txt = parseTxtRecord(packet.subarray(rdata, end))
    } else if (type === 1 && rdlen === 4) {
      rr.address = `${packet[rdata]}.${packet[rdata + 1]}.${packet[rdata + 2]}.${packet[rdata + 3]}`
    }
    records.push(rr)
    off = end
  }
  return { questions, records }
}

function mdnsPacketHasQuestion (packet, name) {
  const msg = parseMdnsPacket(packet)
  if (!msg) return false
  const want = name.toLowerCase()
  return msg.questions.some((q) => {
    const qname = q.name.toLowerCase()
    return qname === want || qname === ESPDISP_MGMT_MDNS_SERVICE
  })
}

function buildManagerMdnsPacket (options, cfg) {
  const serverId = options.serverId || 'signalk-espdisp-manager'
  const instance = `${serverId}.${ESPDISP_MGMT_MDNS_SERVICE}`
  const targetHost = sanitizeMdnsHost(serverId)
  const target = `${targetHost}.local`
  const address = cfg.advertiseHost || firstLocalIpv4() || '127.0.0.1'
  const port = Number(options.signalk && options.signalk.port ? options.signalk.port : 3000)
  const txt = [
    'protocol=espdisp.management.v1',
    'path=/plugins/espdisp-manager',
    `server_id=${serverId}`,
    `auth=${options.auth && options.auth.mode ? options.auth.mode : 'dev-shared-token'}`,
    'tls=false',
    `signalk_port=${port}`,
    'nmea0183_tcp=10110'
  ]
  const answers = [
    dnsRecord(ESPDISP_MGMT_MDNS_SERVICE, 12, dnsName(instance), 120),
    dnsRecord(instance, 33, Buffer.concat([
      Buffer.from([0, 0, 0, 0, (port >> 8) & 0xff, port & 0xff]),
      dnsName(target)
    ]), 120),
    dnsRecord(instance, 16, dnsTxt(txt), 120),
    dnsRecord(target, 1, ipv4Bytes(address), 120)
  ]
  const header = Buffer.alloc(12)
  header.writeUInt16BE(0, 0)
  header.writeUInt16BE(0x8400, 2)
  header.writeUInt16BE(0, 4)
  header.writeUInt16BE(answers.length, 6)
  header.writeUInt16BE(0, 8)
  header.writeUInt16BE(0, 10)
  return Buffer.concat([header, ...answers])
}

function dnsRecord (name, type, rdata, ttl) {
  const head = Buffer.alloc(10)
  head.writeUInt16BE(type, 0)
  head.writeUInt16BE(1, 2)
  head.writeUInt32BE(ttl, 4)
  head.writeUInt16BE(rdata.length, 8)
  return Buffer.concat([dnsName(name), head, rdata])
}

function dnsName (name) {
  const parts = name.split('.').filter(Boolean)
  return Buffer.concat([
    ...parts.map((part) => {
      const buf = Buffer.from(part)
      return Buffer.concat([Buffer.from([buf.length]), buf])
    }),
    Buffer.from([0])
  ])
}

function dnsTxt (items) {
  return Buffer.concat(items.map((item) => {
    const buf = Buffer.from(item)
    return Buffer.concat([Buffer.from([buf.length]), buf])
  }))
}

function ipv4Bytes (address) {
  const parts = String(address).split('.').map((v) => Number(v))
  if (parts.length !== 4 || parts.some((v) => !Number.isInteger(v) || v < 0 || v > 255)) {
    return Buffer.from([127, 0, 0, 1])
  }
  return Buffer.from(parts)
}

function firstLocalIpv4 () {
  const nets = os.networkInterfaces()
  for (const entries of Object.values(nets)) {
    for (const entry of entries || []) {
      if (entry.family === 'IPv4' && !entry.internal) return entry.address
    }
  }
  return ''
}

function sanitizeMdnsHost (value) {
  return String(value || 'signalk-espdisp-manager')
    .toLowerCase()
    .replace(/[^a-z0-9-]/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 63) || 'signalk-espdisp-manager'
}

function readDnsName (packet, offset) {
  const labels = []
  let off = offset
  let jumped = false
  let next = offset
  let guard = 0
  for (;;) {
    if (off >= packet.length || ++guard > 64) return null
    const len = packet[off]
    if (len === 0) {
      off += 1
      if (!jumped) next = off
      return { name: labels.join('.'), offset: next }
    }
    if ((len & 0xc0) === 0xc0) {
      if (off + 1 >= packet.length) return null
      const ptr = ((len & 0x3f) << 8) | packet[off + 1]
      if (!jumped) next = off + 2
      off = ptr
      jumped = true
      continue
    }
    if ((len & 0xc0) !== 0 || off + 1 + len > packet.length) return null
    labels.push(packet.subarray(off + 1, off + 1 + len).toString('utf8'))
    off += 1 + len
    if (!jumped) next = off
  }
}

function parseTxtRecord (buf) {
  const txt = {}
  let off = 0
  while (off < buf.length) {
    const len = buf[off++]
    if (len === 0 || off + len > buf.length) break
    const item = buf.subarray(off, off + len).toString('utf8')
    off += len
    const eq = item.indexOf('=')
    if (eq < 0) {
      txt[item] = true
    } else {
      txt[item.slice(0, eq)] = item.slice(eq + 1)
    }
  }
  return txt
}

function discoveryAddressKey (device) {
  if (!device || !device.address) return ''
  return `${device.address}:${Number(device.port || 80)}`
}

function isStaleDiscovery (device, at) {
  const lastSeenMs = device && device.lastSeen ? Date.parse(device.lastSeen) : 0
  const atMs = Date.parse(at || now())
  return !lastSeenMs || !atMs || atMs - lastSeenMs > 60000
}

function normalizeDiscoveryBody (body) {
  body = body || {}
  const nested = body.device || {}
  return {
    ...nested,
    ...body,
    id: nested.id || body.id || body.deviceId,
    deviceId: body.deviceId || nested.deviceId || nested.id || body.id,
    name: body.name || nested.name || body.deviceId || nested.id,
    board: body.board || nested.board || nested.board_id || null,
    firmware: body.firmware || nested.firmware || null,
    display: body.display || nested.display || null,
    capabilities: body.capabilities || nested.capabilities || {},
    address: body.address || body.ip || nested.address || nested.ip || null,
    authRequired: Boolean(body.authRequired || nested.authRequired),
    port: body.port || nested.port || 80,
    services: body.services || nested.services || [],
    mdns: body.mdns || nested.mdns || null,
    transport: body.transport || nested.transport || 'http'
  }
}

function httpGetJson (url, auth, timeoutMs) {
  return new Promise((resolve, reject) => {
    const headers = {}
    if (auth && auth.enabled && auth.username && auth.password) {
      headers.Authorization = `Basic ${Buffer.from(`${auth.username}:${auth.password}`).toString('base64')}`
    }
    const req = http.get(url, { headers, timeout: timeoutMs || 3000 }, (res) => {
      let body = ''
      res.setEncoding('utf8')
      res.on('data', (chunk) => {
        body += chunk
        if (body.length > 128 * 1024) req.destroy(new Error('device response too large'))
      })
      res.on('end', () => {
        if (res.statusCode < 200 || res.statusCode >= 300) {
          reject(httpError(res.statusCode, 'device_http_error', `device returned HTTP ${res.statusCode}`))
          return
        }
        try {
          resolve(JSON.parse(body))
        } catch (err) {
          reject(httpError(502, 'device_bad_json', 'device returned invalid JSON'))
        }
      })
    })
    req.on('timeout', () => req.destroy(new Error('device request timeout')))
    req.on('error', (err) => reject(httpError(502, 'device_unreachable', err.message)))
  })
}

function postDeviceCommand (host, port, line, auth) {
  return new Promise((resolve, reject) => {
    const body = String(line || '')
    const headers = {
      'Content-Type': 'text/plain',
      'Content-Length': Buffer.byteLength(body)
    }
    if (auth && auth.enabled && auth.username && auth.password) {
      headers.Authorization = `Basic ${Buffer.from(`${auth.username}:${auth.password}`).toString('base64')}`
    }
    const req = http.request({
      host,
      port,
      path: '/api/cmd',
      method: 'POST',
      headers,
      timeout: 3000
    }, (res) => {
      let response = ''
      res.setEncoding('utf8')
      res.on('data', (chunk) => {
        response += chunk
        if (response.length > 64 * 1024) req.destroy(new Error('device response too large'))
      })
      res.on('end', () => {
        if (res.statusCode < 200 || res.statusCode >= 300) {
          reject(httpError(res.statusCode, 'device_http_error', `device returned HTTP ${res.statusCode}`))
          return
        }
        resolve({
          status: 'sent',
          httpStatus: res.statusCode,
          command: body,
          response
        })
      })
    })
    req.on('timeout', () => req.destroy(new Error('device request timeout')))
    req.on('error', (err) => reject(httpError(502, 'device_unreachable', err.message)))
    req.write(body)
    req.end()
  })
}

function discoveryDeviceFromState (state, host, port) {
  if (!state || !state.device || !state.device.id) return null
  if (!state.wifi && !state.manager && !state.screen) return null
  return {
    id: state.device.id,
    deviceId: state.device.id,
    name: state.device.name || state.device.id,
    source: 'ip-scan',
    address: host,
    port,
    transport: 'http',
    authRequired: Boolean(state.webAuth && state.webAuth.enabled),
    display: state.display
      ? {
          width: Number(state.display.width || state.display.w || 0) || undefined,
          height: Number(state.display.height || state.display.h || 0) || undefined,
          rotation: Number(state.display.rotation || 0) || 0,
          brightness: state.display.brightness
        }
      : null,
    firmware: state.device.build ? { build: state.device.build } : null,
    capabilities: {
      web: true,
      manager: Boolean(state.manager),
      touch: Boolean(state.touch)
    },
    services: [
      { type: '_espdisp._tcp', port }
    ]
  }
}

function scanCandidates (value, store, limit) {
  const seen = new Set()
  const out = []
  const add = (host) => {
    host = String(host || '').trim()
    if (!isIpv4(host) || seen.has(host) || out.length >= limit) return
    seen.add(host)
    out.push(host)
  }
  const raw = Array.isArray(value) ? value.join(',') : String(value || '').trim()
  const tokens = raw
    ? raw.split(/[\s,]+/).filter(Boolean)
    : defaultScanTargets(store)
  tokens.forEach((token) => expandScanToken(token, add, limit))
  return out
}

function defaultScanTargets (store) {
  const targets = []
  for (const iface of Object.values(os.networkInterfaces())) {
    for (const addr of iface || []) {
      if (addr.family !== 'IPv4' || addr.internal || !isIpv4(addr.address)) continue
      const parts = addr.address.split('.')
      targets.push(`${parts[0]}.${parts[1]}.${parts[2]}.0/24`)
    }
  }
  for (const device of Object.values((store.discovery && store.discovery.devices) || {})) {
    if (device.address) targets.push(device.address)
  }
  for (const device of Object.values((store.registry && store.registry.devices) || {})) {
    const ip = device.discovery && device.discovery.address
      ? device.discovery.address
      : device.networkIdentity && device.networkIdentity.lastResolvedAddress
    if (ip) targets.push(ip)
  }
  return targets
}

function expandScanToken (token, add, limit) {
  if (token.includes('/')) {
    const [base, prefixRaw] = token.split('/')
    const prefix = Number(prefixRaw)
    if (!isIpv4(base) || !Number.isInteger(prefix) || prefix < 24 || prefix > 32) return
    const count = Math.min(2 ** (32 - prefix), limit)
    const start = ipv4ToInt(base) & (0xffffffff << (32 - prefix))
    for (let i = 1; i < count - 1; i++) add(intToIpv4(start + i))
    if (prefix === 32) add(base)
    return
  }
  if (token.includes('-')) {
    const [startRaw, endRaw] = token.split('-')
    if (!isIpv4(startRaw)) return
    const startParts = startRaw.split('.').map(Number)
    const end = isIpv4(endRaw)
      ? Number(endRaw.split('.')[3])
      : Number(endRaw)
    if (!Number.isInteger(end) || end < 0 || end > 255) return
    const lo = Math.min(startParts[3], end)
    const hi = Math.max(startParts[3], end)
    for (let last = lo; last <= hi; last++) {
      add(`${startParts[0]}.${startParts[1]}.${startParts[2]}.${last}`)
    }
    return
  }
  add(token)
}

function parseScanPorts (value) {
  const ports = (Array.isArray(value) ? value : String(value).split(/[\s,]+/))
    .map((port) => Number(port))
    .filter((port) => Number.isInteger(port) && port > 0 && port <= 65535)
  return ports.length ? Array.from(new Set(ports)).slice(0, 8) : [80]
}

async function mapLimit (items, limit, fn) {
  let index = 0
  const workers = Array.from({ length: Math.min(limit, items.length) }, async () => {
    for (;;) {
      const current = index++
      if (current >= items.length) return
      await fn(items[current])
    }
  })
  await Promise.all(workers)
}

function isIpv4 (value) {
  const parts = String(value || '').split('.')
  return parts.length === 4 && parts.every((part) => {
    if (!/^\d+$/.test(part)) return false
    const n = Number(part)
    return n >= 0 && n <= 255
  })
}

function ipv4ToInt (ip) {
  return ip.split('.').reduce((acc, part) => ((acc << 8) + Number(part)) >>> 0, 0)
}

function intToIpv4 (value) {
  return [24, 16, 8, 0].map((shift) => (value >>> shift) & 255).join('.')
}

function firmwareVersionFromDevice (device) {
  const firmware = device.firmware || {}
  const identity = device.identity || {}
  return firmware.version ||
    firmware.semver ||
    firmware.build ||
    identity.firmware_version ||
    identity.firmwareVersion ||
    (identity.firmware && identity.firmware.version) ||
    null
}

function compareFirmwareArtifacts (a, b) {
  const uploaded = String(b.uploadedAt || '').localeCompare(String(a.uploadedAt || ''))
  if (uploaded !== 0) return uploaded
  const av = a.firmware && a.firmware.version ? String(a.firmware.version) : ''
  const bv = b.firmware && b.firmware.version ? String(b.firmware.version) : ''
  return bv.localeCompare(av, undefined, { numeric: true, sensitivity: 'base' })
}

function isDeviceOnline (device, heartbeatMs) {
  const lastSeenMs = device.lastSeen ? Date.parse(device.lastSeen) : 0
  if (!lastSeenMs) return false
  return Date.now() - lastSeenMs <= Math.max(Number(heartbeatMs || 0) * 3, 15000)
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
    auth: device.auth
      ? {
          web: device.auth.web || null,
          manager: device.auth.manager || null
        }
      : null,
    discovery: device.discovery || null,
    networkIdentity: device.networkIdentity,
    assignedProfile: device.assignedProfile,
    config: device.config,
    status: device.status && device.status.network
      ? { network: device.status.network, ui: device.status.ui }
      : {}
  }
}

function hashToken (token) {
  return `sha256:${crypto.createHash('sha256').update(String(token || '')).digest('hex')}`
}

function timingSafeEqual (a, b) {
  const aa = Buffer.from(String(a || ''))
  const bb = Buffer.from(String(b || ''))
  return aa.length === bb.length && crypto.timingSafeEqual(aa, bb)
}

function deviceHasToken (device) {
  return Boolean(device && (device.deviceTokenHash || device.deviceToken))
}

function verifyStoredToken (record, token) {
  if (!record || !token) return false
  if (record.tokenHash && timingSafeEqual(record.tokenHash, hashToken(token))) return true
  return Boolean(record.token && timingSafeEqual(hashToken(record.token), hashToken(token)))
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
  devicesFromMdnsPacket,
  buildManagerMdnsPacket,
  parseMdnsPacket,
  normalizeOptions,
  httpError
}
