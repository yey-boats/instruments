const fs = require('fs')
const path = require('path')
const { EspDispManager } = require('./lib/manager')
const presets = require('./lib/screen-presets')
const pluginPackage = require('./package.json')

module.exports = function espdispManagerPlugin (app) {
  let manager

  const plugin = {
    id: 'espdisp-manager',
    name: 'ESP Display Manager',
    description: 'Registry, central configuration, command queue, and firmware management for ESP display devices.',
    schema: () => ({
      type: 'object',
      title: 'ESP Display Manager',
      description: 'Open the manager at /signalk-espdisp-manager/ or /plugins/espdisp-manager/ui. It is also discoverable from SignalK Admin > Webapps after the server is restarted.',
      properties: {
        links: {
          type: 'object',
          title: 'Operator Links',
          readOnly: true,
          default: {
            webapp: '/signalk-espdisp-manager/',
            pluginUi: '/plugins/espdisp-manager/ui',
            devices: '/plugins/espdisp-manager/ui/devices',
            discovery: '/plugins/espdisp-manager/ui/discovery'
          },
          properties: {
            webapp: {
              type: 'string',
              title: 'SignalK webapp',
              default: '/signalk-espdisp-manager/'
            },
            pluginUi: {
              type: 'string',
              title: 'Plugin UI',
              default: '/plugins/espdisp-manager/ui'
            },
            devices: {
              type: 'string',
              title: 'Devices',
              default: '/plugins/espdisp-manager/ui/devices'
            },
            discovery: {
              type: 'string',
              title: 'Discovery',
              default: '/plugins/espdisp-manager/ui/discovery'
            }
          }
        },
        serverId: {
          type: 'string',
          title: 'Server ID',
          default: 'signalk-espdisp-manager'
        },
        heartbeatMs: {
          type: 'number',
          title: 'Heartbeat interval, ms',
          default: 30000
        },
        commandPollMs: {
          type: 'number',
          title: 'Command poll interval, ms',
          default: 15000
        },
        auth: {
          type: 'object',
          title: 'Authentication',
          properties: {
            mode: {
              type: 'string',
              title: 'Mode',
              enum: ['dev-shared-token', 'provision-token', 'disabled'],
              default: 'dev-shared-token'
            },
            devToken: {
              type: 'string',
              title: 'Development shared token',
              default: 'espdisp-dev'
            },
            provisionToken: {
              type: 'string',
              title: 'Provisioning token',
              default: 'espdisp-provision'
            }
          }
        },
        signalk: {
          type: 'object',
          title: 'SignalK Target',
          properties: {
            host: { type: 'string', title: 'Host', default: 'signalk.local' },
            port: { type: 'number', title: 'Port', default: 3000 }
          }
        },
        deviceWebAuth: {
          type: 'object',
          title: 'Device Web API Basic Auth',
          description: 'Credentials pushed to devices and used by this plugin to read live status and logs.',
          properties: {
            enabled: { type: 'boolean', title: 'Enabled', default: true },
            username: { type: 'string', title: 'Username', default: 'espdisp' },
            password: { type: 'string', title: 'Password', default: 'espdisp-dev' }
          }
        },
        discoveryUdp: {
          type: 'object',
          title: 'SignalK UDP Discovery',
          description: 'LAN discovery responder used by ESP displays when mDNS is unavailable, for example Docker bridge networking.',
          properties: {
            enabled: { type: 'boolean', title: 'Enabled', default: true },
            bind: { type: 'string', title: 'Bind address', default: '0.0.0.0' },
            port: { type: 'number', title: 'UDP port', default: 34300 },
            host: {
              type: 'string',
              title: 'Advertised host',
              description: 'Leave empty to let the device use the UDP reply source address.',
              default: ''
            }
          }
        },
        deviceDiscoveryUdp: {
          type: 'object',
          title: 'Device UDP Discovery',
          description: 'Listener for ESP display presence announcements. Discovered devices appear in the Discovery page before they are claimed.',
          properties: {
            enabled: { type: 'boolean', title: 'Enabled', default: true },
            bind: { type: 'string', title: 'Bind address', default: '0.0.0.0' },
            port: { type: 'number', title: 'UDP port', default: 34301 }
          }
        },
        firmware: {
          type: 'object',
          title: 'Firmware Catalog',
          properties: {
            github: {
              type: 'object',
              title: 'GitHub release source',
              description: 'Imports firmware artifacts from GitHub release assets for software upgrades.',
              properties: {
                enabled: { type: 'boolean', title: 'Enabled', default: true },
                owner: { type: 'string', title: 'GitHub owner', default: 'navado' },
                repo: { type: 'string', title: 'GitHub repository', default: 'esp32-boat-mfd' },
                includePrereleases: { type: 'boolean', title: 'Include prereleases', default: false }
              }
            }
          }
        },
        network: {
          type: 'object',
          title: 'Network Identity',
          properties: {
            domain: { type: 'string', title: 'mDNS domain', default: 'local' },
            hostnamePrefix: { type: 'string', title: 'Hostname prefix', default: 'espdisp' },
            namingPolicy: {
              type: 'string',
              title: 'Naming policy',
              enum: ['device-id', 'role-location', 'manual'],
              default: 'device-id'
            },
            mdns: {
              type: 'object',
              title: 'mDNS',
              properties: {
                enabled: { type: 'boolean', title: 'Enabled', default: true },
                browser: {
                  type: 'boolean',
                  title: 'Discover ESP displays via Bonjour/mDNS',
                  default: true
                },
                advertiseManager: {
                  type: 'boolean',
                  title: 'Advertise manager via Bonjour/mDNS',
                  default: true
                },
                bind: { type: 'string', title: 'Bind address', default: '0.0.0.0' },
                port: { type: 'number', title: 'mDNS UDP port', default: 5353 },
                advertiseHost: {
                  type: 'string',
                  title: 'Advertised IPv4 address',
                  description: 'Leave empty to use the first non-internal IPv4 address visible to Node.',
                  default: ''
                },
                advertiseIntervalMs: {
                  type: 'number',
                  title: 'Manager advertisement interval, ms',
                  default: 60000
                }
              }
            }
          }
        }
      }
    }),
    start: (options) => {
      manager = new EspDispManager(app, options || {})
      registerAutopilotBridge(app)
      app.debug('espdisp-manager started')
    },
    stop: () => {
      if (manager && manager.close) manager.close()
      manager = undefined
    },
    statusMessage: () => {
      if (!manager) return 'stopped'
      return `${Object.keys(manager.store.registry.devices).length} device(s)`
    },
    registerWithRouter: (router) => {
      registerRoutes(router, () => manager)
    },
    getOpenApi: () => ({
      openapi: '3.0.0',
      info: {
        title: 'ESP Display Manager',
        version: pluginPackage.version
      },
      paths: {
        '/plugins/espdisp-manager/.well-known/espdisp-management': {
          get: { summary: 'Discover ESP display management API' }
        },
        '/plugins/espdisp-manager/devices/register': {
          post: { summary: 'Register or refresh an ESP display device' }
        },
        '/plugins/espdisp-manager/discovery/devices': {
          get: { summary: 'List discovered ESP display devices' },
          post: { summary: 'Announce a discovered ESP display device' }
        },
        '/plugins/espdisp-manager/discovery/scan': {
          post: { summary: 'Scan IP or BLE transports for ESP display devices' }
        },
        '/plugins/espdisp-manager/discovery/devices/{deviceId}/claim': {
          post: { summary: 'Claim a discovered ESP display device into the registry' }
        },
        '/plugins/espdisp-manager/capabilities': {
          get: { summary: 'Describe manager protocol capabilities' }
        },
        '/plugins/espdisp-manager/dashboard': {
          get: { summary: 'Summarise managed device health and operations' }
        },
        '/plugins/espdisp-manager/ui': {
          get: { summary: 'Built-in lightweight management console' }
        },
        '/plugins/espdisp-manager/devices/{deviceId}/status': {
          post: { summary: 'Update device status heartbeat' }
        },
        '/plugins/espdisp-manager/devices/{deviceId}/live/status': {
          get: { summary: 'Read live device /api/state' }
        },
        '/plugins/espdisp-manager/devices/{deviceId}/live/logs': {
          get: { summary: 'Read live device /api/logs' }
        },
        '/plugins/espdisp-manager/devices/{deviceId}/config': {
          get: { summary: 'Fetch generated device config' }
        },
        '/plugins/espdisp-manager/devices/{deviceId}/commands': {
          get: { summary: 'Poll pending commands' },
          post: { summary: 'Create a command' }
        },
        '/plugins/espdisp-manager/devices/{deviceId}/commands/{commandId}': {
          get: { summary: 'Read command state' }
        },
        '/plugins/espdisp-manager/provisioning/tokens': {
          get: { summary: 'List provisioning tokens' },
          post: { summary: 'Create provisioning token' }
        },
        '/plugins/espdisp-manager/devices/{deviceId}/profile': {
          post: { summary: 'Assign profile to a device' }
        },
        '/plugins/espdisp-manager/profiles/{profileId}/apply': {
          post: { summary: 'Apply profile to one or more devices' }
        },
        '/plugins/espdisp-manager/groups/{groupId}/command': {
          post: { summary: 'Create command for a device group' }
        },
        '/plugins/espdisp-manager/automation/event': {
          post: { summary: 'Submit automation event' }
        },
        '/plugins/espdisp-manager/firmware/catalog': {
          get: { summary: 'List firmware artifacts' }
        },
        '/plugins/espdisp-manager/firmware/catalog/refresh': {
          post: { summary: 'Refresh firmware artifacts from GitHub releases' }
        },
        '/plugins/espdisp-manager/devices/{deviceId}/firmware/jobs': {
          get: { summary: 'List firmware jobs' },
          post: { summary: 'Create firmware update job' }
        }
      }
    })
  }

  return plugin
}

// Autopilot bridge: the espdisp firmware drives the autopilot the
// signalk-autopilot / spec-16 way (PUT steering.autopilot.state =
// "<mode>", PUT steering.autopilot.actions.adjustHeading = <deg>, PUT
// steering.autopilot.target.headingTrue = <rad>). The KDCube simulator's
// autopilot instead listens for a `steering.autopilot.command` delta
// {action, value, nonce}. This bridge registers PUT handlers for the
// firmware's paths and re-emits them as the sim's command deltas, so the
// device's steering controls drive the modeled boat end-to-end.
function registerAutopilotBridge (app) {
  if (!app || typeof app.registerPutHandler !== 'function') return
  const CMD = 'steering.autopilot.command'
  let seq = 0
  const emit = (action, value) => {
    app.handleMessage('espdisp-autopilot-bridge', {
      updates: [{
        values: [{ path: CMD, value: { action, value, nonce: `b${++seq}` } }]
      }]
    })
  }
  const done = (cb) => {
    const r = { state: 'COMPLETED', statusCode: 200 }
    if (typeof cb === 'function') cb(r)
    return r
  }
  const reg = (path, fn) => {
    try { app.registerPutHandler('vessels.self', path, fn, 'espdisp-autopilot-bridge') } catch (e) {
      if (app.debug) app.debug(`autopilot bridge: could not register ${path}: ${e.message}`)
    }
  }
  // Mode: "auto"|"wind"|"route"|"standby" (firmware "track"/"pretrack" -> "route").
  reg('steering.autopilot.state', (ctx, path, value, cb) => {
    let mode = String(value == null ? '' : value).replace(/^"|"$/g, '')
    if (mode === 'track' || mode === 'pretrack') mode = 'route'
    emit('set_mode', mode)
    return done(cb)
  })
  // Heading nudge: firmware sends DEGREES; the sim's "adjust" action expects
  // radians (it converts back to degrees), so scale here.
  reg('steering.autopilot.actions.adjustHeading', (ctx, path, value, cb) => {
    const deg = Number(value) || 0
    emit('adjust', (deg * Math.PI) / 180)
    return done(cb)
  })
  // Absolute target heading (radians, passed through).
  reg('steering.autopilot.target.headingTrue', (ctx, path, value, cb) => {
    emit('set_heading', Number(value) || 0)
    return done(cb)
  })
}

function registerRoutes (router, getManager) {
  router.use(jsonBody)

  router.get('/.well-known/espdisp-management', wrap(getManager, (manager, req, res) => {
    res.json(manager.discovery())
  }))

  router.get('/devices', wrap(getManager, (manager, req, res) => {
    res.json(manager.listDevices(req.query || {}))
  }))

  // Lightweight summary list for the Waveshare knob's remote Select-Display
  // menu: [{ id, name, role, online, currentScreen }]. Registered before the
  // ':id' routes so the literal path is matched first.
  router.get('/devices/summary', wrap(getManager, (manager, req, res) => {
    res.json(manager.deviceSummaries())
  }))

  // Views (screens) a device can switch between, for the knob's Select-View
  // menu: { views: [{ id, title }], current }. Derived from the device's
  // resolved layout, falling back to the standard known view ids.
  router.get('/devices/:id/views', wrap(getManager, (manager, req, res) => {
    res.json(manager.deviceViews(req.params.id))
  }))

  // Device-reported capability manifest (ui.capabilities). { capabilities } is
  // null until the device reports one. The layout editor gates its options to
  // this so it only offers what the connected firmware can render.
  router.get('/devices/:id/capabilities', wrap(getManager, (manager, req, res) => {
    res.json({ capabilities: manager.deviceCapabilities(req.params.id) })
  }))

  // ---- Slice 5: manifest-gated layout editor CRUD (JSON) ----------------
  // The field editor (public/field-editor.js) drives these. Each returns the
  // fresh editorLayout { profileId, manifest, screens, items } so the UI can
  // re-render without a second round-trip. Manifest gating + persistence +
  // config.reload all happen in lib/manager.js.

  // Effective manifest the editor gates to (device-reported, or the built-in
  // default when offline / pre-manifest firmware) + the editable layout.
  router.get('/devices/:id/editor/layout', wrap(getManager, (manager, req, res) => {
    res.json(manager.editorLayout(req.params.id))
  }))

  router.post('/devices/:id/editor/screens', wrap(getManager, (manager, req, res) => {
    res.json(manager.addScreen(req.params.id, req.body || {}))
  }))

  router.patch('/devices/:id/editor/screens/:screenId', wrap(getManager, (manager, req, res) => {
    const b = req.body || {}
    if (typeof b.title === 'string') {
      res.json(manager.renameScreen(req.params.id, req.params.screenId, b.title))
    } else {
      res.json(manager.editorLayout(req.params.id))
    }
  }))

  router.post('/devices/:id/editor/screens/reorder', wrap(getManager, (manager, req, res) => {
    res.json(manager.reorderScreens(req.params.id, (req.body && req.body.order) || []))
  }))

  router.delete('/devices/:id/editor/screens/:screenId', wrap(getManager, (manager, req, res) => {
    res.json(manager.deleteScreen(req.params.id, req.params.screenId))
  }))

  router.post('/devices/:id/editor/screens/:screenId/fields', wrap(getManager, (manager, req, res) => {
    res.json(manager.addField(req.params.id, req.params.screenId, (req.body && req.body.field) || {}))
  }))

  router.patch('/devices/:id/editor/screens/:screenId/fields/:widgetId', wrap(getManager, (manager, req, res) => {
    res.json(manager.updateField(req.params.id, req.params.screenId, req.params.widgetId, (req.body && req.body.field) || {}))
  }))

  router.delete('/devices/:id/editor/screens/:screenId/fields/:widgetId', wrap(getManager, (manager, req, res) => {
    res.json(manager.removeField(req.params.id, req.params.screenId, req.params.widgetId))
  }))

  // "Save limits": persist configured range/zones onto the field, and OPTIONALLY
  // write them back to the SignalK path meta (opt-in via writeBack:true). The
  // SK meta write-back degrades gracefully — if SignalK rejects it (no perms),
  // we still persist onto the field and report metaWriteBack:'failed'.
  router.post('/devices/:id/editor/screens/:screenId/fields/:widgetId/limits',
    wrap(getManager, async (manager, req, res) => {
      const b = req.body || {}
      const layout = manager.saveFieldLimits(req.params.id, req.params.screenId, req.params.widgetId, {
        range: b.range, zones: b.zones
      })
      let metaWriteBack = 'skipped'
      if (b.writeBack === true && typeof b.path === 'string' && b.path) {
        metaWriteBack = await writeSignalKMeta(manager, b.path, { range: b.range, zones: b.zones })
          .then(() => 'ok').catch(() => 'failed')
      }
      res.json({ ...layout, metaWriteBack })
    }))

  router.get('/discovery/devices', wrap(getManager, (manager, req, res) => {
    res.json(manager.listDiscoveredDevices())
  }))

  router.post('/discovery/devices', wrap(getManager, (manager, req, res) => {
    res.json(manager.announceDiscoveredDevice(req.body || {}, authFrom(req)))
  }))

  router.post('/discovery/scan', wrap(getManager, async (manager, req, res) => {
    const result = await manager.scanForDevices(req.body || {})
    if (String(req.headers['content-type'] || '').includes('application/x-www-form-urlencoded')) {
      res.statusCode = 303
      res.setHeader('location', `/plugins/espdisp-manager/ui/discovery?scan=${encodeURIComponent(result.status)}&found=${result.found}&scanned=${result.scanned}`)
      res.end()
      return
    }
    res.json(result)
  }))

  router.post('/devices/register-from-signalk', wrap(getManager, async (manager, req, res) => {
    const body = req.body || {}
    const result = await manager.registerDeviceFromSignalK({
      ...body,
      sendReload: checkboxValue(body.sendReload),
      sendManagerRegister: checkboxValue(body.sendManagerRegister)
    })
    if (String(req.headers['content-type'] || '').includes('application/x-www-form-urlencoded')) {
      res.statusCode = 303
      res.setHeader('location', `/plugins/espdisp-manager/ui/devices/${encodeURIComponent(result.deviceId)}?status=registered-through-signalk`)
      res.end()
      return
    }
    res.json(result)
  }))

  router.post('/discovery/devices/:id/claim', wrap(getManager, (manager, req, res) => {
    const body = req.body || {}
    const result = manager.claimDiscoveredDevice(req.params.id, {
      ...body,
      sendReload: checkboxValue(body.sendReload),
      issueToken: checkboxValue(body.issueToken)
    })
    if (String(req.headers['content-type'] || '').includes('application/x-www-form-urlencoded')) {
      res.statusCode = 303
      res.setHeader('location', `/plugins/espdisp-manager/ui/devices/${encodeURIComponent(req.params.id)}`)
      res.end()
      return
    }
    res.json(result)
  }))

  router.get('/capabilities', wrap(getManager, (manager, req, res) => {
    res.json(manager.pluginCapabilities())
  }))

  // ---- screen-preset catalogue (read-only) ------------------------------
  // Powers the visual layout editor: GET /presets/screens?board=<id> (or
  // ?displayClass=<id>) returns a curated list of starter screens that
  // match the device's display geometry. The editor inserts the chosen
  // screen verbatim into the active profile.
  router.get('/presets/displays', (req, res) => {
    res.json({ displayClasses: presets.listDisplayClasses() })
  })
  router.get('/presets/widgets', (req, res) => {
    res.json({ widgetTypes: presets.listWidgetTypes(), paths: presets.ALL_PATHS })
  })
  router.get('/presets/screens', (req, res) => {
    const displayClass = req.query.displayClass ||
                        (req.query.board ? presets.classifyBoard(String(req.query.board)) : 'sunton-480')
    res.json({
      displayClass,
      screens: presets.getPresetsForClass(String(displayClass))
    })
  })

  // GET /devices/proxy/screenshot.png?url=http://<device-host>[:port]
  //
  // Pulls /api/screenshot.png from the device and streams the body back
  // to the editor. Lets the layout-editor UI work even when the browser
  // can't directly reach the device (e.g. browser on a different VLAN
  // than the device, but the SignalK host can route to both).
  //
  // Allows only http/https URLs to private-network targets (RFC 1918 +
  // link-local + loopback). No public-internet SSRF.
  function isPrivateHost (host) {
    if (!host) return false
    if (host === 'localhost' || host.endsWith('.local')) return true
    const v4 = host.match(/^(\d+)\.(\d+)\.(\d+)\.(\d+)$/)
    if (!v4) return false
    const o = v4.slice(1, 5).map((n) => parseInt(n, 10))
    if (o.some((x) => isNaN(x) || x < 0 || x > 255)) return false
    if (o[0] === 10) return true
    if (o[0] === 192 && o[1] === 168) return true
    if (o[0] === 172 && o[1] >= 16 && o[1] <= 31) return true
    if (o[0] === 169 && o[1] === 254) return true
    if (o[0] === 127) return true
    return false
  }
  router.get('/devices/proxy/screenshot.png', async (req, res) => {
    try {
      const raw = String(req.query.url || '').trim()
      if (!raw) { res.status(400).json({ error: 'missing url' }); return }
      const u = new URL(/^https?:\/\//i.test(raw) ? raw : 'http://' + raw)
      if (!/^https?:$/i.test(u.protocol)) {
        res.status(400).json({ error: 'unsupported scheme' })
        return
      }
      if (!isPrivateHost(u.hostname)) {
        res.status(403).json({ error: 'only private-network hosts allowed' })
        return
      }
      // node 18+ has global fetch; SignalK ships node 20.
      // redirect: 'manual' prevents an attacker-controlled device from
      // 302'ing this server-side fetch to an arbitrary host (the
      // private-host check above only validates the URL we typed,
      // not what the device might redirect us to).
      const upstream = await fetch(u.origin + '/api/screenshot.png', {
        signal: AbortSignal.timeout(10000),
        redirect: 'manual'
      })
      if (upstream.status >= 300 && upstream.status < 400) {
        res.status(502).json({ error: 'device tried to redirect; refusing' })
        return
      }
      if (!upstream.ok) {
        res.status(upstream.status).json({ error: 'device returned ' + upstream.status })
        return
      }
      res.setHeader('Content-Type', 'image/png')
      res.setHeader('Cache-Control', 'no-store')
      // No CORS wildcard: the editor reaches this proxy same-origin
      // (it's mounted under /plugins/espdisp-manager). Cross-origin
      // callers should not be able to use the SignalK host as an open
      // SSRF gateway into the LAN.
      const buf = Buffer.from(await upstream.arrayBuffer())
      res.end(buf)
    } catch (e) {
      res.status(502).json({ error: e.message || String(e) })
    }
  })

  router.get('/dashboard', wrap(getManager, (manager, req, res) => {
    res.json(manager.dashboard())
  }))

  router.get('/ui', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'overview', req))
  }))

  router.get('/ui/devices', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'devices', req))
  }))

  router.get('/ui/devices/:id', wrap(getManager, async (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    const dashboard = manager.dashboard()
    const live = {}
    try {
      live.status = await manager.getLiveStatus(req.params.id)
    } catch (err) {
      live.statusError = err
    }
    try {
      live.logs = await manager.getLiveLogs(req.params.id)
    } catch (err) {
      live.logsError = err
    }
    res.end(renderUiShell('Device detail', renderDevicePage(manager, req.params.id, live), dashboard, 'device'))
  }))

  router.get('/ui/devices/:id/config', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'deviceConfig', req))
  }))

  router.get('/ui/devices/:id/live/status', wrap(getManager, async (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    try {
      const status = await manager.getLiveStatus(req.params.id)
      res.end(renderLiveStatusPage(manager, req.params.id, status))
    } catch (err) {
      res.end(renderLiveErrorPage(manager, req.params.id, 'Live status', err))
    }
  }))

  router.get('/ui/devices/:id/live/logs', wrap(getManager, async (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    try {
      const logs = await manager.getLiveLogs(req.params.id, req.query.since)
      res.end(renderLiveLogsPage(manager, req.params.id, logs))
    } catch (err) {
      res.end(renderLiveErrorPage(manager, req.params.id, 'Live logs', err))
    }
  }))

  router.post('/ui/devices/:id/config', wrap(getManager, (manager, req, res) => {
    const result = saveDeviceConfigForm(manager, req.params.id, req.body || {})
    res.statusCode = 303
    res.setHeader('location', `/plugins/espdisp-manager/ui/devices/${encodeURIComponent(req.params.id)}/config?status=${encodeURIComponent(result.status)}`)
    res.end()
  }))

  router.get('/ui/discovery', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'discovery', req))
  }))

  // Cleanup endpoints: device removal + artifact removal. JSON for
  // automation, form-redirect for the UI buttons.
  router.delete('/devices/:id', wrap(getManager, (manager, req, res) => {
    res.json(manager.deleteDevice(req.params.id))
  }))
  router.post('/ui/devices/:id/delete', wrap(getManager, (manager, req, res) => {
    manager.deleteDevice(req.params.id)
    res.statusCode = 303
    res.setHeader('location', '/plugins/espdisp-manager/ui/devices')
    res.end()
  }))
  // Bulk cleanup of the registered-devices list.
  router.post('/ui/devices/clear-offline', wrap(getManager, (manager, req, res) => {
    const r = manager.deleteOfflineDevices()
    res.statusCode = 303
    res.setHeader('location', `/plugins/espdisp-manager/ui/devices?cleared=offline&removed=${r.removed}`)
    res.end()
  }))
  router.post('/ui/devices/clear-all', wrap(getManager, (manager, req, res) => {
    const r = manager.clearAllDevices()
    res.statusCode = 303
    res.setHeader('location', `/plugins/espdisp-manager/ui/devices?cleared=all&removed=${r.removed}`)
    res.end()
  }))
  router.delete('/firmware/artifacts/:artifactId', wrap(getManager, (manager, req, res) => {
    res.json(manager.deleteFirmwareArtifact(req.params.artifactId))
  }))
  router.post('/ui/firmware/artifacts/:artifactId/delete', wrap(getManager, (manager, req, res) => {
    manager.deleteFirmwareArtifact(req.params.artifactId)
    res.statusCode = 303
    res.setHeader('location', '/plugins/espdisp-manager/ui/firmware')
    res.end()
  }))

  router.get('/ui/profiles', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'profiles', req))
  }))

  // Layout editor lives in public/layout-editor.html. We serve it
  // inside the standard renderUiShell so the nav stays consistent
  // and operators see the same header/links as on every other page.
  // The editor iframe stretches to fill the panel; the editor itself
  // owns its own toolbar inside that.
  router.get('/ui/layout', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    const dashboard = manager.dashboard()
    const body = `
      <section class="panel" style="padding: 0; overflow: hidden;">
        <iframe src="/signalk-espdisp-manager/layout-editor.html"
                style="width: 100%; height: calc(100vh - 220px); border: 0; display: block;"
                title="Layout editor"></iframe>
      </section>`
    res.end(renderUiShell('Layout editor', body, dashboard, 'layout'))
  }))

  router.get('/ui/profiles/:id', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'preset', req))
  }))

  router.post('/ui/profiles/:id/apply', wrap(getManager, (manager, req, res) => {
    const result = applyPresetForm(manager, req.params.id, req.body || {})
    res.statusCode = 303
    res.setHeader('location', `/plugins/espdisp-manager/ui/profiles/${encodeURIComponent(req.params.id)}?status=${encodeURIComponent(result.status)}&count=${result.count}`)
    res.end()
  }))

  // Switch a single device to a selected view/profile and queue config.reload.
  router.post('/ui/devices/:id/switch-view', wrap(getManager, (manager, req, res) => {
    const profileId = (req.body && req.body.profileId) || 'default'
    const result = applyPresetForm(manager, profileId, {
      deviceIds: [req.params.id],
      clearOverrides: 'on',
      sendReload: 'on'
    })
    res.statusCode = 303
    res.setHeader('location',
      `/plugins/espdisp-manager/ui/devices/${encodeURIComponent(req.params.id)}` +
      `?status=${encodeURIComponent(result.status)}`)
    res.end()
  }))

  // Switch a single device to a specific screen (live), without changing its
  // assigned profile. Queues a `screen.set` command (createCommand also drives
  // the control-protocol path directly when the device speaks it); the firmware
  // maps screen.set -> show_by_id on its next command poll.
  router.post('/ui/devices/:id/switch-screen', wrap(getManager, (manager, req, res) => {
    const screenId = (req.body && req.body.screenId) || ''
    let status = 'no-screen'
    if (screenId) {
      manager.createCommand(req.params.id, { type: 'screen.set', payload: { screen: screenId } })
      status = 'screen-set'
    }
    res.statusCode = 303
    res.setHeader('location',
      `/plugins/espdisp-manager/ui/devices/${encodeURIComponent(req.params.id)}` +
      `?status=${encodeURIComponent(status)}`)
    res.end()
  }))

  // Save edited data-field bindings from the live preview. mode=switch queues a
  // screen.set; mode=update rewrites the assigned profile's widget paths and
  // reloads the device; mode=create saves the edited layout as a new profile.
  router.post('/ui/devices/:id/save-screen', wrap(getManager, (manager, req, res) => {
    const id = req.params.id
    const body = req.body || {}
    const mode = body.mode || 'update'
    let edits = []
    try { edits = JSON.parse(body.edits || '[]') } catch (e) { edits = [] }
    let status = 'noop'
    if (mode === 'switch') {
      if (body.screenId) {
        manager.createCommand(id, { type: 'screen.set', payload: { screen: body.screenId } })
        status = 'switched'
      }
    } else {
      const device = manager.getDevice(id)
      const baseId = device.assignedProfile || 'default'
      const base = manager.store.profiles.profiles[baseId]
      if (base) {
        const cfg = JSON.parse(JSON.stringify(base.config || {}))
        cfg.widgets = cfg.widgets || {}
        cfg.widgets.items = cfg.widgets.items || {}
        const items = cfg.widgets.items
        edits.forEach((e) => {
          if (!e || typeof e.widgetId !== 'string') return
          // Prototype-pollution guard: only rebind an EXISTING own widget key
          // (hasOwnProperty rejects __proto__/constructor/prototype, which are
          // not own keys), so a crafted widgetId can't write onto Object.prototype.
          if (!Object.prototype.hasOwnProperty.call(items, e.widgetId)) return
          items[e.widgetId].path = String(e.path || '')
        })
        if (mode === 'create') {
          const name = String(body.profileName || 'New View').trim()
          const newId = (name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/(^-|-$)/g, '')) ||
            ('view-' + Math.abs(sha256Json({ id, t: cfg }).length))
          manager.upsertProfile({ id: newId, name, config: cfg })
          status = 'created:' + newId
        } else {
          manager.upsertProfile({ id: baseId, name: base.name || baseId, config: cfg })
          try { manager.queueConfigReload(id) } catch (e) {}
          status = 'updated'
        }
      }
    }
    res.statusCode = 303
    res.setHeader('location',
      `/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}?status=${encodeURIComponent(status)}`)
    res.end()
  }))

  router.get('/ui/firmware', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'firmware', req))
  }))

  router.post('/ui/firmware/catalog/refresh', wrap(getManager, async (manager, req, res) => {
    await manager.refreshFirmwareFromGithub()
    res.statusCode = 303
    res.setHeader('location', '/plugins/espdisp-manager/ui/firmware')
    res.end()
  }))

  router.post('/ui/devices/:id/firmware/update', wrap(getManager, (manager, req, res) => {
    manager.createFirmwareJob(req.params.id, {
      artifactId: req.body && req.body.artifactId,
      policy: {
        reboot: req.body.reboot !== 'false',
        confirmAfterBoot: req.body.confirmAfterBoot !== 'false',
        rollbackOnFailure: true
      }
    })
    res.statusCode = 303
    res.setHeader('location', '/plugins/espdisp-manager/ui/firmware')
    res.end()
  }))

  router.get('/groups', wrap(getManager, (manager, req, res) => {
    res.json(manager.listGroups())
  }))

  router.get('/provisioning/tokens', wrap(getManager, (manager, req, res) => {
    res.json(manager.listProvisioningTokens())
  }))

  router.post('/provisioning/tokens', wrap(getManager, (manager, req, res) => {
    res.json(manager.createProvisioningToken(req.body || {}))
  }))

  router.post('/devices/register', wrap(getManager, (manager, req, res) => {
    res.json(manager.registerDevice(req.body || {}, authFrom(req)))
  }))

  router.get('/devices/:id', wrap(getManager, (manager, req, res) => {
    res.json(manager.getDevice(req.params.id))
  }))

  router.patch('/devices/:id', wrap(getManager, (manager, req, res) => {
    res.json(manager.patchDevice(req.params.id, req.body || {}))
  }))

  router.get('/devices/:id/auth/status', wrap(getManager, (manager, req, res) => {
    res.json(manager.authStatus(req.params.id))
  }))

  router.post('/devices/:id/profile', wrap(getManager, (manager, req, res) => {
    res.json(manager.assignProfile(req.params.id, req.body || {}))
  }))

  router.post('/devices/:id/status', wrap(getManager, (manager, req, res) => {
    res.json(manager.updateStatus(req.params.id, req.body || {}, authFrom(req)))
  }))

  router.get('/devices/:id/live/status', wrap(getManager, async (manager, req, res) => {
    res.json(await manager.getLiveStatus(req.params.id))
  }))

  router.get('/devices/:id/live/logs', wrap(getManager, async (manager, req, res) => {
    res.json(await manager.getLiveLogs(req.params.id, req.query.since))
  }))

  router.get('/devices/:id/config', wrap(getManager, (manager, req, res) => {
    manager.requireDeviceAuth(req.params.id, authFrom(req))
    res.json(manager.generateConfig(req.params.id))
  }))

  router.get('/profiles', wrap(getManager, (manager, req, res) => {
    res.json(manager.listProfiles())
  }))

  router.get('/profiles/:id/dashboard.json', wrap(getManager, (manager, req, res) => {
    res.json(dashboardPresetDocument(manager, req.params.id))
  }))

  router.get('/profiles/:id/dashboard.yaml', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'application/yaml; charset=utf-8')
    res.end(toYaml(dashboardPresetDocument(manager, req.params.id)))
  }))

  router.post('/profiles/import-dashboard', wrap(getManager, (manager, req, res) => {
    const imported = importDashboardPreset(manager, req.body || {}, req.headers || {})
    if (String(req.headers['content-type'] || '').includes('application/x-www-form-urlencoded')) {
      res.statusCode = 303
      res.setHeader('location', `/plugins/espdisp-manager/ui/profiles/${encodeURIComponent(imported.id)}`)
      res.end()
      return
    }
    res.json(imported)
  }))

  router.post('/profiles', wrap(getManager, (manager, req, res) => {
    res.json(manager.upsertProfile(req.body || {}))
  }))

  router.post('/profiles/:id/apply', wrap(getManager, (manager, req, res) => {
    res.json(manager.applyProfile(req.params.id, req.body || {}))
  }))

  router.post('/devices/:id/command', wrap(getManager, (manager, req, res) => {
    res.json(manager.createCommand(req.params.id, req.body || {}))
  }))

  router.post('/groups/:groupId/command', wrap(getManager, (manager, req, res) => {
    res.json(manager.createGroupCommand(req.params.groupId, req.body || {}))
  }))

  router.post('/automation/event', wrap(getManager, (manager, req, res) => {
    res.json(manager.automationEvent(req.body || {}))
  }))

  router.get('/devices/:id/commands', wrap(getManager, (manager, req, res) => {
    res.json(manager.getCommands(req.params.id, authFrom(req), req.query.limit))
  }))

  router.get('/devices/:id/commands/:commandId', wrap(getManager, (manager, req, res) => {
    res.json(manager.getCommand(req.params.id, req.params.commandId))
  }))

  router.post('/devices/:id/commands/:commandId/cancel', wrap(getManager, (manager, req, res) => {
    res.json(manager.cancelCommand(req.params.id, req.params.commandId, (req.body || {}).reason))
  }))

  router.post('/devices/:id/commands/:commandId/ack', wrap(getManager, (manager, req, res) => {
    res.json(manager.ackCommand(req.params.id, req.params.commandId, req.body || {}, authFrom(req)))
  }))

  router.post('/devices/:id/tokens/rotate', wrap(getManager, (manager, req, res) => {
    res.json(manager.rotateDeviceToken(req.params.id))
  }))

  router.post('/devices/:id/tokens/revoke', wrap(getManager, (manager, req, res) => {
    res.json(manager.revokeDeviceToken(req.params.id))
  }))

  router.get('/firmware/catalog', wrap(getManager, (manager, req, res) => {
    res.json(manager.listFirmware())
  }))

  router.post('/firmware/catalog/refresh', wrap(getManager, async (manager, req, res) => {
    res.json(await manager.refreshFirmwareFromGithub())
  }))

  router.post('/firmware/artifacts', wrap(getManager, (manager, req, res) => {
    res.json(manager.addFirmwareArtifact(req.body || {}))
  }))

  router.get('/firmware/artifacts/:artifactId', wrap(getManager, (manager, req, res) => {
    res.json(manager.getFirmwareArtifact(req.params.artifactId))
  }))

  router.get('/firmware/download/:jobId', wrap(getManager, (manager, req, res) => {
    const info = manager.firmwareDownloadInfo(req.params.jobId)
    const file = info.artifact && info.artifact.file ? info.artifact.file : {}
    if (!file.path) {
      res.json(info)
      return
    }
    res.setHeader('content-type', file.contentType || 'application/octet-stream')
    if (file.size) res.setHeader('content-length', String(file.size))
    res.setHeader('x-espdisp-artifact-id', info.artifact.artifactId)
    res.setHeader('x-espdisp-sha256', file.sha256 || '')
    res.setHeader('content-disposition', `attachment; filename="${path.basename(file.name || file.path)}"`)
    fs.createReadStream(file.path)
      .on('error', (err) => {
        if (!res.headersSent) {
          res.status(404).json({ error: { code: 'artifact_binary_missing', message: err.message } })
        } else {
          res.destroy(err)
        }
      })
      .pipe(res)
  }))

  router.get('/devices/:id/firmware/jobs', wrap(getManager, (manager, req, res) => {
    res.json(manager.listFirmwareJobs(req.params.id))
  }))

  router.post('/devices/:id/firmware/jobs', wrap(getManager, (manager, req, res) => {
    res.json(manager.createFirmwareJob(req.params.id, req.body || {}))
  }))

  router.get('/devices/:id/firmware/jobs/:jobId', wrap(getManager, (manager, req, res) => {
    res.json(manager.getFirmwareJob(req.params.id, req.params.jobId))
  }))

  router.post('/devices/:id/firmware/jobs/:jobId/progress', wrap(getManager, (manager, req, res) => {
    res.json(manager.updateFirmwareProgress(req.params.id, req.params.jobId, req.body || {}, authFrom(req)))
  }))

  router.post('/devices/:id/firmware/confirm', wrap(getManager, (manager, req, res) => {
    res.json(manager.confirmFirmware(req.params.id, req.body || {}, authFrom(req)))
  }))
}

function wrap (getManager, handler) {
  return (req, res) => {
    try {
      const manager = getManager()
      if (!manager) {
        res.status(503).json({ error: { code: 'plugin_stopped', message: 'plugin is not running' } })
        return
      }
      Promise.resolve(handler(manager, req, res)).catch((err) => {
        const status = err.status || 500
        res.status(status).json(err.payload || {
          error: {
            code: status === 500 ? 'internal_error' : 'request_failed',
            message: err.message
          }
        })
      })
    } catch (err) {
      const status = err.status || 500
      res.status(status).json(err.payload || {
        error: {
          code: status === 500 ? 'internal_error' : 'request_failed',
          message: err.message
        }
      })
    }
  }
}

function authFrom (req) {
  const value = (req.get && req.get('x-espdisp-authorization')) ||
    (req.headers && req.headers['x-espdisp-authorization']) ||
    (req.get ? req.get('authorization') : (req.headers.authorization || ''))
  const match = String(value || '').match(/^Bearer\s+(.+)$/i)
  const provision = String(value || '').match(/^EspDisp-Provision\s+(.+)$/i)
  return {
    bearer: match ? match[1] : null,
    provision: provision ? provision[1] : null
  }
}

function jsonBody (req, res, next) {
  if (req.body || req.method === 'GET' || req.method === 'HEAD') {
    next()
    return
  }
  let body = ''
  req.setEncoding('utf8')
  req.on('data', (chunk) => {
    body += chunk
    if (body.length > 1024 * 1024) req.destroy()
  })
  req.on('end', () => {
    if (!body) {
      req.body = {}
    } else {
      try {
        const contentType = req.headers['content-type'] || ''
        if (contentType.includes('application/x-www-form-urlencoded')) {
          req.body = parseUrlEncodedForm(body)
        } else if (contentType.includes('yaml') || contentType.includes('text/plain')) {
          req.body = { raw: body }
        } else {
          req.body = JSON.parse(body)
        }
      } catch (err) {
        res.status(400).json({ error: { code: 'invalid_body', message: 'invalid request body' } })
        return
      }
    }
    next()
  })
}

function parseUrlEncodedForm (body) {
  const parsed = {}
  for (const [key, value] of new URLSearchParams(body)) {
    if (Object.prototype.hasOwnProperty.call(parsed, key)) {
      parsed[key] = Array.isArray(parsed[key])
        ? parsed[key].concat(value)
        : [parsed[key], value]
    } else {
      parsed[key] = value
    }
  }
  return parsed
}

function dashboardPresetDocument (manager, profileId) {
  const profile = manager.store.profiles.profiles[profileId]
  if (!profile) throw statusError(404, 'preset not found')
  return {
    kind: 'espdisp.dashboard.v1',
    preset: {
      id: profile.id,
      name: profile.name || profile.id,
      version: Number(profile.version || 1),
      updatedAt: profile.updatedAt || null
    },
    dashboard: profile.config || {}
  }
}

function importDashboardPreset (manager, body, headers) {
  const contentType = String(headers['content-type'] || '')
  const doc = body.raw
    ? parseDashboardImport(body.raw, body.format === 'json' ? 'application/json' : contentType)
    : (body.dashboard || body.preset ? body : { dashboard: body })
  if (doc.kind && doc.kind !== 'espdisp.dashboard.v1') {
    throw statusError(400, 'unsupported dashboard config kind')
  }
  const preset = doc.preset || {}
  const id = sanitizePresetId(body.presetId || doc.presetId || preset.id || preset.name)
  if (!id) throw statusError(400, 'preset id is required')
  const config = doc.dashboard || doc.config
  if (!config || typeof config !== 'object' || Array.isArray(config)) {
    throw statusError(400, 'dashboard object is required')
  }
  return manager.upsertProfile({
    id,
    name: preset.name || doc.name || id,
    version: Number(preset.version || doc.version || 1),
    config
  })
}

function parseDashboardImport (raw, contentType) {
  const text = String(raw || '').trim()
  if (!text) throw statusError(400, 'empty dashboard import')
  if (contentType.includes('json') || text.startsWith('{') || text.startsWith('[')) {
    try {
      return JSON.parse(text)
    } catch (err) {
      throw statusError(400, 'invalid dashboard JSON')
    }
  }
  return fromYaml(text)
}

function renderUi (manager, page, req) {
  const dashboard = manager.dashboard()
  const title = {
    overview: 'Overview',
    devices: 'Devices',
    device: 'Device detail',
    deviceConfig: 'Device config',
    discovery: 'Discovery',
    profiles: 'Profiles',
    preset: 'Preset',
    firmware: 'Firmware'
  }[page] || 'Overview'
  return renderUiShell(title, renderPage(manager, dashboard, page, req), dashboard, page)
}

function renderUiShell (title, body, dashboard, page = '') {
  dashboard = dashboard || { serverId: 'signalk-espdisp-manager', generatedAt: new Date().toISOString() }
  return `<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP Display Manager · ${escapeHtml(title)}</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, sans-serif; }
    body { margin: 0; background: #f5f7f8; color: #172026; }
    header { padding: 18px 28px 0; background: #15323b; color: white; }
    main { padding: 24px 28px; }
    h1 { margin: 0; font-size: 24px; }
    h2 { margin: 0 0 12px; font-size: 18px; }
    .sub { color: #d6e2e6; margin-top: 4px; padding-bottom: 14px; }
    nav { display: flex; gap: 4px; flex-wrap: wrap; }
    nav a { color: #d6e2e6; text-decoration: none; padding: 10px 12px; border-radius: 6px 6px 0 0; font-size: 14px; }
    nav a.active { color: #172026; background: #f5f7f8; }
    .grid { display: grid; grid-template-columns: repeat(5, minmax(120px, 1fr)); gap: 12px; margin-bottom: 24px; }
    .metric, .panel { background: white; border: 1px solid #d9e0e3; border-radius: 6px; padding: 14px; }
    .metric b { display: block; font-size: 28px; line-height: 1; }
    .metric span, .muted { color: #60717a; font-size: 13px; }
    table { width: 100%; border-collapse: collapse; background: white; border: 1px solid #d9e0e3; margin-bottom: 20px; }
    th, td { text-align: left; border-bottom: 1px solid #e5eaed; padding: 10px 12px; font-size: 14px; vertical-align: top; }
    th { background: #eef3f5; color: #40515a; }
    td span { color: #60717a; font-size: 12px; }
    a { color: #116078; }
    code { background: #eef3f5; padding: 1px 4px; border-radius: 3px; }
    pre { overflow: auto; background: #172026; color: #e8f1f4; padding: 14px; border-radius: 6px; }
    .json-block { max-height: 520px; }
    .config-grid { display: grid; grid-template-columns: repeat(2, minmax(240px, 1fr)); gap: 14px; margin-bottom: 20px; }
    .config-section { background: white; border: 1px solid #d9e0e3; border-radius: 6px; padding: 14px; }
    .config-section.full { grid-column: 1 / -1; }
    .config-section table { margin-bottom: 0; border: 0; }
    .config-section th { width: 38%; }
    .config-form { background: white; border: 1px solid #d9e0e3; border-radius: 6px; padding: 14px; margin-bottom: 20px; }
    .form-grid { display: grid; grid-template-columns: repeat(4, minmax(150px, 1fr)); gap: 12px; margin-bottom: 14px; }
    label { display: block; color: #40515a; font-size: 12px; font-weight: 600; }
    input, select, textarea { box-sizing: border-box; width: 100%; min-height: 34px; margin-top: 4px; border: 1px solid #c6d0d5; border-radius: 4px; padding: 6px 8px; background: white; color: #172026; }
    textarea { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 12px; }
    input[type="checkbox"] { width: auto; min-height: auto; margin-right: 6px; }
    fieldset { border: 1px solid #d9e0e3; border-radius: 6px; margin: 0 0 14px; padding: 12px; }
    legend { color: #40515a; font-size: 13px; font-weight: 700; padding: 0 4px; }
    .actions { display: flex; flex-wrap: wrap; gap: 8px; }
    button { min-height: 36px; border: 1px solid #0e5c72; border-radius: 4px; padding: 7px 12px; background: #116078; color: white; font-weight: 600; cursor: pointer; }
    button[disabled] { background: #d9e0e3; border-color: #c6d0d5; color: #60717a; cursor: not-allowed; }
    button[value="save"], button[value="save-preset"] { background: white; color: #116078; }
    .pill { display: inline-block; padding: 2px 7px; border-radius: 999px; background: #eef3f5; color: #40515a; font-size: 12px; }
    .status { display: inline-block; min-width: 64px; padding: 2px 7px; border-radius: 999px; background: #eef3f5; text-align: center; }
    .ok { background: #d9f2e3; color: #145d32; }
    .bad { background: #ffe0df; color: #8a1f18; }
    @media (max-width: 850px) { .grid, .config-grid, .form-grid { grid-template-columns: 1fr; } table { font-size: 12px; } }
  </style>
</head>
<body>
  <header>
    <h1>ESP Display Manager</h1>
    <div class="sub">${escapeHtml(dashboard.serverId)} · ${escapeHtml(dashboard.generatedAt)}</div>
    ${nav(page)}
  </header>
  <main>
    ${body}
  </main>
</body>
</html>`
}

function renderPage (manager, dashboard, page, req) {
  if (page === 'devices') return renderDevicesPage(dashboard.devices, req, manager)
  if (page === 'device') return renderDevicePage(manager, req.params.id)
  if (page === 'deviceConfig') return renderDeviceConfigPage(manager, req.params.id)
  // Legacy /ui/discovery URL stays valid; it just renders the same
  // page as /ui/devices now. Old bookmarks keep working.
  if (page === 'discovery') return renderDevicesPage(dashboard.devices, req, manager)
  if (page === 'profiles') return renderProfilesPage(manager.listProfiles().profiles, dashboard.devices)
  if (page === 'preset') return renderPresetPage(manager, req.params.id, dashboard.devices)
  if (page === 'firmware') return renderFirmwarePage(manager.listFirmware(), dashboard.recentFirmwareJobs, manager.firmwareUpgradeMatrix())
  return renderOverviewPage(dashboard)
}

function renderOverviewPage (dashboard) {
  const counts = dashboard.counts
  return `
    <section class="grid">
      ${metric(counts.devices, 'Devices')}
      ${metric(counts.online, 'Online')}
      ${metric(counts.configDrift, 'Config drift')}
      ${metric(counts.pendingCommands, 'Pending commands')}
      ${metric(counts.firmwareJobs, 'Firmware jobs')}
    </section>
    <section class="panel">
      <h2>Recent devices</h2>
      ${deviceTable(dashboard.devices.slice(0, 8))}
    </section>`
}

function renderDevicesPage (devices, req, manager) {
  // Discovery + registered devices on one page. Previous two-page
  // layout forced operators to bounce between Discovery (to find
  // and claim a device) and Devices (to inspect/configure it).
  // Now the discovery list lives at the top of this same page,
  // grouped under "Pending" - anything not yet registered. Claimed
  // devices drop into the registered table without a navigation
  // switch.
  const host = req && req.headers && req.headers.host ? req.headers.host : ''
  const managerUrl = host ? `http://${host}/plugins/espdisp-manager` : '/plugins/espdisp-manager'
  const discovered = manager ? manager.listDiscoveredDevices().devices : []
  const pendingDevices = discovered.filter((d) => !d.registered)
  const offlineCount = devices.filter((d) => !d.online).length
  const q = (req && req.query) || {}
  const clearedBanner = q.cleared
    ? `<p class="muted" style="color:#2e8b57;">Removed ${escapeHtml(String(q.removed || 0))} ${q.cleared === 'all' ? 'device(s) — list cleared' : 'offline device(s)'}.</p>`
    : ''
  const act = (path, label, confirmMsg, danger) =>
    `<form method="post" action="/plugins/espdisp-manager/ui/devices/${path}" style="display:inline"
           onsubmit="return confirm('${escapeHtml(confirmMsg)}');">
      <button type="submit"${danger ? ' style="background:#c0392b;border-color:#a82716;"' : ''}>${escapeHtml(label)}</button>
    </form>`
  return `
    <section class="panel">
      <h2>Devices</h2>
      <p class="muted">${devices.length} registered · ${pendingDevices.length} pending</p>
      ${clearedBanner}
      <div class="actions">
        <a href="/plugins/espdisp-manager/ui/devices">Refresh</a>
        ${devices.length ? act('clear-offline', `Clear offline (${offlineCount})`, `Remove all ${offlineCount} offline device(s) from the list?`, false) : ''}
        ${devices.length ? act('clear-all', `Clear all (${devices.length})`, `Remove ALL ${devices.length} registered device(s)? This cannot be undone.`, true) : ''}
      </div>
      ${pendingDevices.length ? renderPendingDiscoverySection(manager, pendingDevices) : ''}
      <h3 style="margin-top:24px;">Registered (${devices.length})</h3>
      ${deviceTable(devices)}
      <details style="margin-top:20px;"><summary>Register through SignalK / scan network</summary>
        ${renderSignalKRegisterForm(managerUrl)}
        ${renderDiscoveryScanForm()}
      </details>
    </section>`
}

function renderPendingDiscoverySection (manager, pendingDevices) {
  const profiles = manager.listProfiles().profiles
  const rows = pendingDevices.map((device) => `
        <tr>
          <td><strong>${escapeHtml(device.name || device.deviceId)}</strong><br><span>${escapeHtml(device.deviceId)}</span></td>
          <td>${escapeHtml(device.address || '')}:${escapeHtml(device.port || '')}</td>
          <td>${escapeHtml(device.source || '')}</td>
          <td>${escapeHtml(firmwareLabel(device.firmware))}</td>
          <td>${device.stale ? '<span class="status bad">stale</span>' : '<span class="status ok">fresh</span>'}</td>
          <td>${renderDiscoveryClaimControl(device, profiles)}</td>
        </tr>`).join('')
  return `
      <h3>Pending (${pendingDevices.length})</h3>
      <p class="muted">Devices found on the network but not yet registered. Click "claim" to register.</p>
      <table>
        <thead><tr><th>Device</th><th>Address</th><th>Source</th><th>Firmware</th><th>Freshness</th><th>Action</th></tr></thead>
        <tbody>${rows}</tbody>
      </table>`
}

function renderSignalKRegisterForm (managerUrl) {
  return `
      <form class="config-form" method="post" action="/plugins/espdisp-manager/devices/register-from-signalk">
        <fieldset>
          <legend>Register through SignalK</legend>
          <div class="form-grid">
            ${field('Device address', input('address', '10.42.0.67'))}
            ${field('HTTP port', input('port', '80', 'number', '1', '65535', '1'))}
            ${field('Device ID', input('deviceId', ''))}
            ${field('Preset', input('profileId', 'default'))}
            ${field('Role', input('role', 'display'))}
            ${field('Location', input('location', ''))}
            ${field('Manager URL', input('managerUrl', managerUrl))}
            ${field('Send manager-register', checkbox('sendManagerRegister', true))}
            ${field('Queue reload', checkbox('sendReload', false))}
          </div>
          <div class="actions">
            <button type="submit" name="action" value="register">Register</button>
          </div>
        </fieldset>
      </form>`
}

function renderDevicePage (manager, id, live = {}) {
  const device = manager.getDevice(id)
  const config = manager.generateConfig(id)
  const commands = manager.store.commands.commands
    .filter((command) => command.deviceId === id)
    .slice(-10)
    .reverse()
  const jobs = manager.store.jobs.jobs
    .filter((job) => job.deviceId === id)
    .slice(-10)
    .reverse()
  const profiles = Object.values(manager.store.profiles.profiles)
  const assigned = device.assignedProfile || 'default'
  const profileOptions = profiles
    .map((p) => `<option value="${escapeHtml(p.id)}"${p.id === assigned ? ' selected' : ''}>` +
      `${escapeHtml(p.name || p.id)}</option>`)
    .join('')
  const views = manager.deviceViews(id)
  const currentScreen = views && views.current
  const screenOptions = (views && Array.isArray(views.views) ? views.views : [])
    .map((v) => `<option value="${escapeHtml(v.id)}"${v.id === currentScreen ? ' selected' : ''}>` +
      `${escapeHtml(v.title || v.id)}</option>`)
    .join('')
  // Live-preview data, driven by the device's REAL screen list (deviceViews)
  // so the preview tracks what the device actually has + which screen is
  // current. Each screen's tiles come from the assigned profile's authored
  // layout when managed, else the screen-presets catalogue that mirrors the
  // firmware's built-in screens — so built-in screens still render live
  // objects. Tiles are flattened to {widgetId,widget,title,path,unit,precision}
  // and bound to live SignalK data by public/live-preview.js.
  const previewData = (() => {
    const prof = manager.store.profiles.profiles[assigned] || {}
    const pcfg = prof.config || {}
    const items = (pcfg.widgets && pcfg.widgets.items) || {}
    const authored = {}
    ;((pcfg.layout && pcfg.layout.screens) || []).forEach((s) => { authored[s.id] = s })
    let presetById = {}
    try {
      const sp = require('./lib/screen-presets')
      const dc = (typeof sp.classifyBoard === 'function')
        ? sp.classifyBoard(String((device.board || (device.display && device.display.class) || '')))
        : 'sunton-480'
      ;(sp.getPresetsForClass(dc) || []).forEach((s) => { presetById[s.id] = s })
    } catch (e) { presetById = {} }
    const PRESET_ALIAS = { status: 'system' } // built-in id -> nearest preset id
    const presetTiles = (screenId) => {
      // exact id, alias, else a prefix match so built-in variants map to their
      // base (wind_classic / wind_steer -> "wind"); gives real SignalK bindings.
      let p = presetById[screenId] || presetById[PRESET_ALIAS[screenId]]
      if (!p) {
        const base = Object.keys(presetById).find((k) => screenId === k || screenId.indexOf(k + '_') === 0)
        if (base) p = presetById[base]
      }
      if (!p || !Array.isArray(p.tiles)) return null
      return p.tiles.map((t) => ({
        widgetId: null, editable: false,
        widget: t.widget || 'numeric', title: t.title || (t.primary ? String(t.primary).split('.').pop() : ''),
        path: t.primary || t.path || '', unit: t.unit || '', precision: t.precision != null ? t.precision : null
      }))
    }
    const tilesFor = (screenId) => {
      // A genuinely managed/edited screen (authored tiles WITH real bindings)
      // wins so edits show; an empty-path stub authored layout falls through to
      // the preset catalogue so the preview still shows live objects.
      const a = authored[screenId]
      if (a && Array.isArray(a.tiles) && a.tiles.length) {
        const mapped = a.tiles.map((t) => {
          const w = items[t.widget] || {}
          return {
            widgetId: t.widget, editable: true,
            widget: w.type || 'numeric', title: w.title || t.widget,
            path: w.path || '', unit: w.unit || '', precision: w.precision != null ? w.precision : null
          }
        })
        if (mapped.some((m) => m.path)) return mapped
      }
      return presetTiles(screenId) || []
    }
    const dvViews = (views && Array.isArray(views.views) ? views.views : [])
    // Device telemetry from the heartbeat status, for the System/status panel
    // (the parts that aren't SignalK: wifi/ip/rssi/ble/sk/heap/psram/uptime/build).
    const st = (device && device.status) || {}
    const telemetry = {
      wifiState: (st.network && st.network.state) || (st.network && st.network.wifi_up ? 'STA' : null),
      ip: st.network && st.network.ip,
      rssi: st.network && st.network.rssi,
      ssid: st.network && st.network.ssid,
      ble: (st.network && st.network.hostname) || device.id,
      signalk: (st.signalk && st.signalk.state) || (st.sk && st.sk.state),
      heapKb: st.memory && st.memory.heap_free_kb,
      psramKb: st.memory && st.memory.psram_free_kb,
      uptimeMs: st.ui && st.ui.uptime_ms,
      build: (st.firmware && (st.firmware.build_time || st.firmware.version)) || null
    }
    return {
      current: currentScreen,
      profileId: assigned,
      telemetry,
      screens: dvViews.map((v) => ({ id: v.id, title: v.title || v.id, tiles: tilesFor(v.id) }))
    }
  })()
  // SK path catalogue for the edit datalist (the layout-editor's curated list).
  let previewPaths = []
  try { previewPaths = require('./lib/screen-presets').ALL_PATHS || [] } catch (e) { previewPaths = [] }
  const previewScreenOptions = previewData.screens
    .map((s) => `<option value="${escapeHtml(s.id)}"${s.id === currentScreen ? ' selected' : ''}>` +
      `${escapeHtml(s.title)}</option>`)
    .join('')
  const previewJson = JSON.stringify(previewData).replace(/</g, '\\u003c')
  return `
    <section class="panel">
      <h2>${escapeHtml(device.name || device.id)}</h2>
      <p class="muted">${escapeHtml(device.id)} · ${escapeHtml(device.role)} · ${escapeHtml(device.location || 'unassigned')}</p>
      <p>
        <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/config">Open generated config</a>
        · <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/live/status">Live status</a>
        · <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/live/logs">Live logs</a>
      </p>
      <div class="lp-panel">
        <div class="lp-head">
          <div class="lp-head-title">
            <strong>Live view</strong>
            <span class="lp-now"><span class="lp-dot"></span>on device:&nbsp;<span id="lp-now-screen">…</span></span>
          </div>
        </div>
        <div class="lp-body">
          <div id="lp-root" class="lp-stage"></div>
          <div class="lp-side">
            <div class="lp-controls">
              <label class="lp-ctl lp-ctl-block">Preview screen
                <select id="lp-screen">${previewScreenOptions || '<option value="">(none)</option>'}</select></label>
              <label class="lp-ctl lp-edit-toggle"><input type="checkbox" id="lp-edit">&nbsp;Edit fields</label>
            </div>
            <form id="lp-form" method="post"
                  action="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/save-screen">
              <input type="hidden" name="screenId" id="lp-f-screen">
              <input type="hidden" name="edits" id="lp-f-edits">
              <input type="hidden" name="mode" id="lp-f-mode" value="update">
              <div class="lp-actions">
                <button type="button" class="primary" data-mode="switch" title="Show the selected screen on the device now">Show on device</button>
                <button type="button" data-mode="update" title="Save edits to the assigned view + reload the device">Save to view</button>
                <span class="lp-create">
                  <input type="text" name="profileName" id="lp-f-name" placeholder="new view name">
                  <button type="button" data-mode="create" title="Save the edited layout as a new view">Save as new</button>
                </span>
              </div>
            </form>
            <p class="muted lp-note" id="lp-note"><strong>Show on device</strong> switches the
              device to the selected screen now; tick <strong>Edit fields</strong> to rebind a
              tile's SignalK path, then <strong>Save to view</strong> or <strong>Save as new</strong>.</p>
          </div>
        </div>
        <datalist id="lp-paths">${previewPaths.map((p) => `<option value="${escapeHtml(p)}"></option>`).join('')}</datalist>
      </div>
      <form class="config-form lp-assign" method="post"
            action="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/switch-view">
        <label for="switch-view-profile">Assigned profile</label>
        <div class="actions">
          <select id="switch-view-profile" name="profileId">${profileOptions}</select>
          <button type="submit">Assign + reload</button>
        </div>
      </form>
      <style>
        .lp-panel{background:linear-gradient(180deg,#0e1a26,#0b1019);border:1px solid #1e3142;border-radius:12px;padding:16px;margin-bottom:18px;box-shadow:0 1px 0 #1a2c3c inset,0 6px 22px rgba(0,0,0,.28)}
        .lp-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:12px;color:#cfe0f0;flex-wrap:wrap}
        .lp-head-title{display:flex;align-items:baseline;gap:12px}
        .lp-head-title strong{font-size:15px;letter-spacing:.02em}
        .lp-now{display:inline-flex;align-items:center;font-size:12px;color:#8fb8da;background:#0a1420;border:1px solid #1c3043;border-radius:999px;padding:3px 10px}
        .lp-now #lp-now-screen{color:#eaf2fb;font-weight:600}
        .lp-dot{width:8px;height:8px;border-radius:50%;background:#36d399;margin-right:7px;box-shadow:0 0 0 0 rgba(54,211,153,.6);animation:lp-pulse 2s infinite}
        @keyframes lp-pulse{0%{box-shadow:0 0 0 0 rgba(54,211,153,.55)}70%{box-shadow:0 0 0 6px rgba(54,211,153,0)}100%{box-shadow:0 0 0 0 rgba(54,211,153,0)}}
        .lp-head-controls{display:flex;align-items:center;gap:14px;flex-wrap:wrap}
        .lp-ctl{font-size:12px;color:#8fb8da;display:inline-flex;align-items:center}
        .lp-ctl select{margin-left:4px;background:#0a1420;color:#eaf2fb;border:1px solid #2a4156;border-radius:6px;padding:4px 6px}
        /* Side-by-side: square stage on the left, all controls grouped on the
           right so the whole live view fits one screen without scrolling. */
        .lp-body{display:flex;gap:16px;align-items:flex-start;flex-wrap:wrap}
        .lp-side{flex:1;min-width:230px;display:flex;flex-direction:column;gap:12px}
        .lp-controls{display:flex;flex-direction:column;gap:10px;background:#0a1420;border:1px solid #1c3043;border-radius:9px;padding:12px}
        .lp-ctl-block{flex-direction:column;align-items:stretch;gap:5px;color:#9fc0dd;font-size:11px;letter-spacing:.04em;text-transform:uppercase}
        .lp-ctl-block select{margin-left:0;font-size:14px;padding:7px 8px;text-transform:none}
        .lp-stage{background:radial-gradient(120% 120% at 50% 0%,#0c151f,#070b11);border:1px solid #16242f;border-radius:10px;aspect-ratio:1/1;width:360px;max-width:46vw;min-width:300px;margin:0;padding:10px;display:flex;align-items:center;justify-content:center;flex:0 0 auto}
        .lp-hud{width:100%;height:100%;display:flex;align-items:center;justify-content:center}
        .lp-hud .hud-svg{width:100%;height:100%;display:block}
        .lp-compass{width:100%;height:100%;display:flex;align-items:center;justify-content:center}
        .lp-compass .hud-tile-svg{width:100%;height:100%;max-height:130px}
        .lp-val-pos{font-size:18px;line-height:1.25;white-space:pre-line;font-weight:600}
        .lp-grid{display:grid;grid-template-columns:1fr 1fr;grid-auto-rows:1fr;gap:10px;width:100%;height:100%}
        .lp-tile{background:#11202f;border:1px solid #1c2f42;border-radius:9px;padding:10px;display:flex;flex-direction:column;justify-content:center;align-items:flex-start;min-height:80px}
        .lp-cap{font-size:11px;letter-spacing:.1em;color:#6f97ba;text-transform:uppercase}
        .lp-val{font-size:34px;font-weight:650;color:#eaf2fb;line-height:1.05;align-self:flex-start}
        .lp-val-sm{font-size:18px}
        .lp-unit{font-size:14px;color:#9bb6d0;margin-left:5px;font-weight:400}
        .lp-bar{width:20px;flex:1;background:#0a1420;border-radius:5px;display:flex;align-items:flex-end;margin:8px 0;min-height:40px;align-self:center}
        .lp-bar-fill{width:100%;background:linear-gradient(180deg,#52e0a8,#2bb47e);border-radius:5px;transition:height .25s ease}
        .lp-empty{color:#6f97ba;font-size:13px;text-align:center;line-height:1.5}
        .lp-edit-path{width:100%;margin-top:8px;font-size:11px;background:#0a1420;color:#cfe0f0;border:1px solid #2a4156;border-radius:5px;padding:4px 5px}
        .lp-edit-path:focus{outline:none;border-color:#36d399}
        .lp-actions{display:flex;gap:8px;align-items:stretch;flex-direction:column}
        .lp-actions button{border-radius:7px;padding:9px 13px;border:1px solid #2a4156;background:#13283a;color:#dbe9f6;cursor:pointer;font-size:13px}
        .lp-actions button:hover{border-color:#3a607e;background:#173248}
        .lp-actions button.primary{background:#1f8f5f;border-color:#27a86e;color:#eafff5;font-weight:600}
        .lp-actions button.primary:hover{background:#27a86e}
        .lp-create{display:flex;gap:6px;align-items:center}
        .lp-create input{flex:1;background:#0a1420;color:#eaf2fb;border:1px solid #2a4156;border-radius:6px;padding:8px;font-size:12px;min-width:0}
        .lp-note{font-size:12px;line-height:1.5;margin:2px 0 0}
        .lp-assign{margin-bottom:18px}
        .lp-assign label{font-size:12px;color:#8fb8da}
      </style>
      <script>window.__espdispPreview=${previewJson};window.__espdispDeviceId=${JSON.stringify(id)};</script>
      <script src="/signalk-espdisp-manager/device-hud.js"></script>
      <script src="/signalk-espdisp-manager/live-preview.js"></script>
      <div class="config-grid">
        ${renderLiveStatusWidget(live.status, live.statusError)}
        ${renderLiveLogsWidget(live.logs, live.logsError)}
      </div>
      <table>
        <tbody>
          <tr><th>Profile</th><td>${escapeHtml(device.assignedProfile || 'default')}</td></tr>
          <tr><th>Last seen</th><td>${escapeHtml(device.lastSeen || 'never')}</td></tr>
          <tr><th>Display</th><td>${escapeHtml(displayLabel(manager.resolveDisplay(device)))}</td></tr>
          <tr><th>Firmware</th><td>${escapeHtml(firmwareLabel(device.firmware))}</td></tr>
          <tr><th>Desired hostname</th><td>${escapeHtml(device.networkIdentity && device.networkIdentity.desiredFqdn)}</td></tr>
          <tr><th>Config hash</th><td><code>${escapeHtml(config.hash)}</code></td></tr>
        </tbody>
      </table>
      <h2>Recent commands</h2>
      ${commandTable(commands)}
      <h2>Firmware jobs</h2>
      ${firmwareJobTable(jobs)}
    </section>`
}

function renderLiveStatusWidget (status, err) {
  if (err) return liveErrorWidget('Live status', err)
  return `
    <section class="config-section">
      <h2>Live status</h2>
      ${keyValueTable([
        ['Device', status.device && status.device.id],
        ['WiFi', status.wifi ? `${status.wifi.state || ''} ${status.wifi.ip || ''}`.trim() : ''],
        ['SignalK', status.sk && status.sk.state],
        ['Manager', status.manager && status.manager.health],
        ['Screen', status.screen && status.screen.id],
        ['Touch', status.touch && status.touch.mode]
      ])}
    </section>`
}

function renderLiveLogsWidget (logs, err) {
  if (err) return liveErrorWidget('Live logs', err)
  const entries = Array.isArray(logs.entries)
    ? logs.entries
    : Array.isArray(logs.logs)
      ? logs.logs
      : []
  const recent = entries.slice(-5).map((entry) => [
    entry.seq != null ? entry.seq : '',
    entry.line || entry.message || JSON.stringify(entry)
  ])
  return `
    <section class="config-section">
      <h2>Live logs</h2>
      ${simpleTable(['Seq', 'Message'], recent, 'No live log entries returned.')}
    </section>`
}

function liveErrorWidget (title, err) {
  const message = err && err.payload && err.payload.error
    ? err.payload.error.message
    : err && err.message
      ? err.message
      : 'live device request failed'
  const code = err && err.payload && err.payload.error
    ? err.payload.error.code
    : 'live_request_failed'
  return `
    <section class="config-section">
      <h2>${escapeHtml(title)}</h2>
      ${keyValueTable([
        ['Status', { __html: '<span class="status bad">unreachable</span>' }],
        ['Error', code],
        ['Message', message]
      ])}
    </section>`
}

function renderLiveStatusPage (manager, id, status) {
  const device = manager.getDevice(id)
  const rows = [
    ['Device ID', status.device && status.device.id],
    ['Uptime', status.device && status.device.uptime_ms != null ? `${status.device.uptime_ms} ms` : ''],
    ['WiFi', status.wifi ? `${status.wifi.state || ''} ${status.wifi.ip || ''}`.trim() : ''],
    ['RSSI', status.wifi && status.wifi.rssi],
    ['SignalK', status.sk && status.sk.state],
    ['Manager', status.manager && status.manager.health],
    ['Screen', status.screen && `${status.screen.id || ''} (${status.screen.index != null ? status.screen.index : '?'}/${status.screen.count != null ? status.screen.count : '?'})`],
    ['Theme', status.ui && status.ui.theme],
    ['Brightness', status.display && status.display.brightness],
    ['Touch', status.touch && `${status.touch.mode || ''} ${status.touch.pressed ? 'pressed' : ''}`.trim()]
  ]
  return renderUiShell('Live status', `
    <section class="panel">
      <h2>${escapeHtml(device.name || device.id)} live status</h2>
      <p>
        <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}">Back to device</a>
        · <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/live/status">Refresh</a>
        · <a href="/plugins/espdisp-manager/devices/${encodeURIComponent(id)}/live/status">Raw JSON</a>
      </p>
      ${keyValueTable(rows)}
      ${status.manager && Array.isArray(status.manager.recentErrors) && status.manager.recentErrors.length
        ? `<h2>Recent device errors</h2>${simpleTable(['Time', 'Message'], status.manager.recentErrors.map((entry) => [entry.t_ms, entry.msg]))}`
        : ''}
      <h2>Full status</h2>
      <pre class="json-block">${escapeHtml(JSON.stringify(status, null, 2))}</pre>
    </section>`)
}

function renderLiveLogsPage (manager, id, logs) {
  const device = manager.getDevice(id)
  const entries = Array.isArray(logs.entries)
    ? logs.entries
    : Array.isArray(logs.logs)
      ? logs.logs
      : []
  const rows = entries.map((entry) => [
    entry.seq != null ? entry.seq : '',
    entry.t_ms != null ? entry.t_ms : (entry.time || ''),
    entry.level || '',
    entry.line || entry.message || JSON.stringify(entry)
  ])
  return renderUiShell('Live logs', `
    <section class="panel">
      <h2>${escapeHtml(device.name || device.id)} live logs</h2>
      <p>
        <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}">Back to device</a>
        · <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/live/logs${logs.lastSeq != null ? `?since=${encodeURIComponent(logs.lastSeq)}` : ''}">Refresh</a>
        · <a href="/plugins/espdisp-manager/devices/${encodeURIComponent(id)}/live/logs">Raw JSON</a>
      </p>
      ${simpleTable(['Seq', 'Time', 'Level', 'Message'], rows, 'No live log entries returned.')}
      <h2>Full log response</h2>
      <pre class="json-block">${escapeHtml(JSON.stringify(logs, null, 2))}</pre>
    </section>`)
}

function renderLiveErrorPage (manager, id, title, err) {
  const device = manager.getDevice(id)
  const message = err && err.payload && err.payload.error
    ? err.payload.error.message
    : err && err.message
      ? err.message
      : 'live device request failed'
  const code = err && err.payload && err.payload.error
    ? err.payload.error.code
    : 'live_request_failed'
  return renderUiShell(title, `
    <section class="panel">
      <h2>${escapeHtml(device.name || device.id)} ${escapeHtml(title.toLowerCase())}</h2>
      <p>
        <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}">Back to device</a>
        · <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/live/status">Live status</a>
        · <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/live/logs">Live logs</a>
      </p>
      <table>
        <tbody>
          <tr><th>Status</th><td><span class="status bad">unreachable</span></td></tr>
          <tr><th>Error</th><td>${escapeHtml(code)}</td></tr>
          <tr><th>Message</th><td>${escapeHtml(message)}</td></tr>
          <tr><th>Address source</th><td>${escapeHtml(manager.deviceHttpBase ? 'registered device network identity' : '')}</td></tr>
        </tbody>
      </table>
    </section>`)
}

function renderDeviceConfigPage (manager, id) {
  const device = manager.getDevice(id)
  const config = manager.generateConfig(id)
  const profiles = manager.listProfiles().profiles
  // The device self-reports its real switchable screens (heartbeat ui.screens);
  // drive the default-screen picker and the Screens list from that instead of
  // the manager's generated preset catalogue.
  let views = { views: [], current: null }
  try { views = manager.deviceViews(id) || views } catch (e) { /* offline: leave empty */ }
  return `
    <section class="panel">
      <h2>${escapeHtml(device.name || device.id)} config</h2>
      <p class="muted">
        Operator preview for ${escapeHtml(device.id)}. The device pull endpoint
        still requires <code>X-EspDisp-Authorization</code>.
      </p>
      <p>
        <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}">Back to device</a>
      </p>
      ${renderDeviceConfigForm(device, config, profiles, views)}
      ${renderDeviceConfigWidget(config, views)}
    </section>`
}

// Built-in screen ids the firmware renders as fullscreen HUDs (not tile grids).
const FULLSCREEN_SCREEN_IDS = new Set(['autopilot', 'ap_hud', 'wind', 'wind_classic', 'wind_steer', 'knob_wind', 'knob_compass', 'knob_big', 'zoom'])
function screenKindLabel (id) {
  return FULLSCREEN_SCREEN_IDS.has(String(id)) ? 'fullscreen' : 'grid'
}

function saveDeviceConfigForm (manager, id, body) {
  const device = manager.getDevice(id)
  const profileId = String(body.profileId || device.assignedProfile || 'default')
  if (!manager.store.profiles.profiles[profileId]) throw statusError(400, 'unknown preset')
  const overrides = configOverridesFromForm(body)
  let assignedProfile = profileId

  if (body.action === 'save-preset' || body.action === 'save-send-preset') {
    const presetId = sanitizePresetId(body.presetId || body.presetName)
    if (!presetId) throw statusError(400, 'preset id is required')
    const existing = manager.store.profiles.profiles[presetId]
    manager.upsertProfile({
      id: presetId,
      name: body.presetName || presetId,
      version: existing ? Number(existing.version || 1) + 1 : 1,
      config: overrides
    })
    assignedProfile = presetId
  }

  manager.patchDevice(id, {
    assignedProfile,
    overrides
  })

  if (body.action === 'save-send' || body.action === 'save-send-preset') {
    const config = manager.generateConfig(id)
    manager.createCommand(id, {
      type: 'config.reload',
      payload: {
        version: config.version,
        hash: config.hash,
        url: `/plugins/espdisp-manager/devices/${id}/config`
      }
    })
  }

  return { status: body.action || 'saved' }
}

function applyPresetForm (manager, profileId, body) {
  const profile = manager.store.profiles.profiles[profileId]
  if (!profile) throw statusError(404, 'preset not found')
  const deviceIds = arrayValue(body.deviceIds)
  if (deviceIds.length === 0) throw statusError(400, 'select at least one device')

  deviceIds.forEach((deviceId) => {
    manager.assignProfile(deviceId, {
      profileId,
      overrides: checkboxValue(body.clearOverrides) ? {} : manager.getDevice(deviceId).overrides
    })
    if (checkboxValue(body.sendReload)) {
      const config = manager.generateConfig(deviceId)
      manager.createCommand(deviceId, {
        type: 'config.reload',
        payload: {
          version: config.version,
          hash: config.hash,
          url: `/plugins/espdisp-manager/devices/${deviceId}/config`
        }
      })
    }
  })

  return { status: checkboxValue(body.sendReload) ? 'applied-and-sent' : 'applied', count: deviceIds.length }
}

function configOverridesFromForm (body) {
  const widgets = widgetsFromForm(body)
  const layout = layoutFromForm(body)
  return {
    settings: {
      defaultScreen: cleanString(body.defaultScreen) || 'dashboard',
      theme: cleanString(body.theme) || 'day',
      brightness: numberValue(body.brightness, 0.8),
      demoMode: checkboxValue(body.demoMode)
    },
    nmea0183Wifi: {
      enabled: checkboxValue(body.nmeaEnabled),
      mode: cleanString(body.nmeaMode) || 'tcp',
      host: cleanString(body.nmeaHost) || 'signalk.local',
      port: integerValue(body.nmeaPort, 10110)
    },
    autopilot: {
      enabled: checkboxValue(body.autopilotEnabled),
      allowEngage: checkboxValue(body.allowEngage),
      allowStandby: checkboxValue(body.allowStandby),
      allowHeadingAdjust: checkboxValue(body.allowHeadingAdjust),
      backend: cleanString(body.autopilotBackend) || 'signalk'
    },
    widgets: {
      defaults: {
        fontSize: integerValue(body.fontSize, 18),
        labelFontSize: integerValue(body.labelFontSize, 12),
        valueFontSize: integerValue(body.valueFontSize, 32),
        unitFontSize: integerValue(body.unitFontSize, 14)
      },
      items: widgets
    },
    layout,
    debug: {
      logLevel: cleanString(body.logLevel) || 'info',
      touchMode: cleanString(body.touchMode) || 'irq'
    }
  }
}

function renderDeviceConfigForm (device, config, profiles, views) {
  const settings = config.settings || {}
  const nmea = config.nmea0183Wifi || {}
  const autopilot = config.autopilot || {}
  const widgets = (config.widgets && config.widgets.defaults) || {}
  const widgetItems = (config.widgets && config.widgets.items) || {}
  const layout = config.layout || {}
  const debug = config.debug || {}
  // Default-screen picker: a dropdown of the device's own reported screens when
  // we have them, so the operator can only pick a screen that actually exists.
  // Falls back to a free-text input when the device is offline / unseen.
  const dvViews = (views && Array.isArray(views.views)) ? views.views : []
  const current = settings.defaultScreen || (views && views.current) || ''
  const defaultScreenField = dvViews.length
    ? select('defaultScreen', current, dvViews.map((v) => [v.id, `${v.title || v.id} (${screenKindLabel(v.id)})`]))
    : input('defaultScreen', current || 'dashboard')
  return `
    <form class="config-form" method="post" action="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.id)}/config">
      <h2>Configure device</h2>
      <div class="form-grid">
        ${field('Preset', profileSelect(profiles, device.assignedProfile || config.profile))}
        ${field('Default screen', defaultScreenField)}
        ${field('Theme', select('theme', settings.theme || 'day', [['day', 'Day'], ['night', 'Night'], ['high-contrast', 'High contrast']]))}
        ${field('Brightness', input('brightness', settings.brightness == null ? 0.8 : settings.brightness, 'number', '0', '1', '0.05'))}
        ${field('Demo mode', checkbox('demoMode', settings.demoMode))}
        ${field('NMEA WiFi', checkbox('nmeaEnabled', nmea.enabled))}
        ${field('NMEA mode', select('nmeaMode', nmea.mode || 'tcp', [['tcp', 'TCP'], ['udp', 'UDP']]))}
        ${field('NMEA host', input('nmeaHost', nmea.host || 'signalk.local'))}
        ${field('NMEA port', input('nmeaPort', nmea.port || 10110, 'number', '1', '65535', '1'))}
        ${field('Autopilot', checkbox('autopilotEnabled', autopilot.enabled))}
        ${field('Allow engage', checkbox('allowEngage', autopilot.allowEngage))}
        ${field('Allow standby', checkbox('allowStandby', autopilot.allowStandby))}
        ${field('Heading adjust', checkbox('allowHeadingAdjust', autopilot.allowHeadingAdjust))}
        ${field('AP backend', input('autopilotBackend', autopilot.backend || 'signalk'))}
        ${field('Base font', input('fontSize', widgets.fontSize || 18, 'number', '8', '80', '1'))}
        ${field('Label font', input('labelFontSize', widgets.labelFontSize || 12, 'number', '8', '80', '1'))}
        ${field('Value font', input('valueFontSize', widgets.valueFontSize || 32, 'number', '8', '120', '1'))}
        ${field('Unit font', input('unitFontSize', widgets.unitFontSize || 14, 'number', '8', '80', '1'))}
        ${field('Log level', select('logLevel', debug.logLevel || 'info', [['debug', 'Debug'], ['info', 'Info'], ['warn', 'Warn'], ['error', 'Error']]))}
        ${field('Touch mode', select('touchMode', debug.touchMode || 'irq', [['irq', 'IRQ'], ['poll', 'Poll'], ['disabled', 'Disabled']]))}
      </div>
      ${renderWidgetEditor(widgetItems)}
      ${renderLayoutEditor(layout)}
      <fieldset>
        <legend>Save as preset</legend>
        <div class="form-grid">
          ${field('Preset id', input('presetId', ''))}
          ${field('Preset name', input('presetName', ''))}
        </div>
      </fieldset>
      <div class="actions">
        <button type="submit" name="action" value="save">Save device</button>
        <button type="submit" name="action" value="save-send">Save and send to device</button>
        <button type="submit" name="action" value="save-preset">Save as preset</button>
        <button type="submit" name="action" value="save-send-preset">Save preset and send</button>
      </div>
    </form>`
}

function widgetsFromForm (body) {
  const ids = arrayValue(body.widgetId)
  const titles = arrayValue(body.widgetTitle)
  const types = arrayValue(body.widgetType)
  const paths = arrayValue(body.widgetPath)
  const units = arrayValue(body.widgetUnit)
  const precisions = arrayValue(body.widgetPrecision)
  const valueFonts = arrayValue(body.widgetValueFontSize)
  const remove = new Set(arrayValue(body.removeWidget).map(cleanString))
  const items = {}
  ids.forEach((rawId, index) => {
    const id = sanitizeWidgetId(rawId)
    if (!id || remove.has(id)) return
    const type = cleanString(types[index]) || 'numeric'
    const path = cleanString(paths[index])
    if (!path) return
    const widget = {
      type,
      title: cleanString(titles[index]) || id,
      path
    }
    const unit = cleanString(units[index])
    if (unit) widget.unit = unit
    const precision = integerValue(precisions[index], null)
    if (precision != null) widget.precision = precision
    const valueFontSize = integerValue(valueFonts[index], null)
    if (valueFontSize != null) widget.valueFontSize = valueFontSize
    items[id] = widget
  })
  return items
}

function layoutFromForm (body) {
  const screenIds = arrayValue(body.screenId)
  const screenTypes = arrayValue(body.screenType)
  const tileWidgets = arrayValue(body.tileWidget)
  const tileScreens = arrayValue(body.tileScreen)
  const tileCols = arrayValue(body.tileCol)
  const tileRows = arrayValue(body.tileRow)
  const removeTiles = new Set(arrayValue(body.removeTile).map(cleanString))
  const screenById = {}
  screenIds.forEach((rawId, index) => {
    const id = sanitizeWidgetId(rawId)
    if (!id) return
    screenById[id] = {
      id,
      type: cleanString(screenTypes[index]) || 'grid',
      tiles: []
    }
  })
  tileWidgets.forEach((rawWidget, index) => {
    const widget = sanitizeWidgetId(rawWidget)
    const screenId = sanitizeWidgetId(tileScreens[index] || screenIds[0] || 'dashboard')
    if (!widget || !screenId || removeTiles.has(`${screenId}:${widget}:${index}`)) return
    if (!screenById[screenId]) {
      screenById[screenId] = { id: screenId, type: 'grid', tiles: [] }
    }
    const tile = { widget }
    const col = integerValue(tileCols[index], null)
    const row = integerValue(tileRows[index], null)
    if (col != null || row != null) {
      tile.area = {}
      if (col != null) tile.area.col = col
      if (row != null) tile.area.row = row
    }
    screenById[screenId].tiles.push(tile)
  })
  return {
    version: 1,
    screens: Object.values(screenById)
  }
}

function renderWidgetEditor (items) {
  const rows = Object.entries(items || {}).map(([id, widget]) => renderWidgetEditorRow(id, widget))
  rows.push(renderWidgetEditorRow('', {}))
  return `
      <fieldset>
        <legend>Widgets</legend>
        <table>
          <thead><tr><th>ID</th><th>Title</th><th>Type</th><th>SignalK path</th><th>Unit</th><th>Precision</th><th>Value font</th><th>Remove</th></tr></thead>
          <tbody>${rows.join('')}</tbody>
        </table>
      </fieldset>`
}

function renderWidgetEditorRow (id, widget) {
  return `
    <tr>
      <td>${input('widgetId', id)}</td>
      <td>${input('widgetTitle', widget.title || '')}</td>
      <td>${select('widgetType', widget.type || 'numeric', widgetTypeOptions())}</td>
      <td>${input('widgetPath', widget.path || '')}</td>
      <td>${input('widgetUnit', widget.unit || '')}</td>
      <td>${input('widgetPrecision', widget.precision == null ? '' : widget.precision, 'number', '0', '6', '1')}</td>
      <td>${input('widgetValueFontSize', widget.valueFontSize == null ? '' : widget.valueFontSize, 'number', '8', '120', '1')}</td>
      <td>${id ? `<input type="checkbox" name="removeWidget" value="${escapeHtml(id)}">` : ''}</td>
    </tr>`
}

function renderLayoutEditor (layout) {
  const screens = Array.isArray(layout.screens) && layout.screens.length
    ? layout.screens
    : [{ id: 'dashboard', type: 'grid', tiles: [] }]
  const screenRows = screens.map((screen) => `
    <tr>
      <td>${input('screenId', screen.id || 'dashboard')}</td>
      <td>${select('screenType', screen.type || 'grid', [['grid', 'Grid']])}</td>
    </tr>`).join('')
  const tileRows = []
  screens.forEach((screen) => {
    ;(screen.tiles || []).forEach((tile, index) => {
      tileRows.push(renderTileEditorRow(screen.id || 'dashboard', tile, index))
    })
  })
  tileRows.push(renderTileEditorRow(screens[0].id || 'dashboard', {}, tileRows.length))
  return `
      <fieldset>
        <legend>Screens</legend>
        <table>
          <thead><tr><th>Screen ID</th><th>Type</th></tr></thead>
          <tbody>${screenRows}</tbody>
        </table>
        <table>
          <thead><tr><th>Screen</th><th>Widget</th><th>Column</th><th>Row</th><th>Remove</th></tr></thead>
          <tbody>${tileRows.join('')}</tbody>
        </table>
      </fieldset>`
}

function renderTileEditorRow (screenId, tile, index) {
  const area = tile.area || {}
  const removeValue = `${screenId}:${tile.widget || ''}:${index}`
  return `
    <tr>
      <td>${input('tileScreen', screenId || 'dashboard')}</td>
      <td>${input('tileWidget', tile.widget || '')}</td>
      <td>${input('tileCol', area.col == null ? '' : area.col, 'number', '0', '15', '1')}</td>
      <td>${input('tileRow', area.row == null ? '' : area.row, 'number', '0', '15', '1')}</td>
      <td>${tile.widget ? `<input type="checkbox" name="removeTile" value="${escapeHtml(removeValue)}">` : ''}</td>
    </tr>`
}

function widgetTypeOptions () {
  return [
    ['numeric', 'Numeric'],
    ['text', 'Text'],
    ['bar', 'Bar'],
    ['gauge', 'Gauge'],
    ['compass', 'Compass'],
    ['windRose', 'Wind rose'],
    ['trend', 'Trend'],
    ['button', 'Button'],
    ['autopilot', 'Autopilot']
  ]
}

function field (labelText, control) {
  return `<label>${escapeHtml(labelText)}${control}</label>`
}

function input (name, value, type, min, max, step) {
  const attrs = [
    `type="${escapeHtml(type || 'text')}"`,
    `name="${escapeHtml(name)}"`,
    `value="${escapeHtml(value)}"`
  ]
  if (min != null) attrs.push(`min="${escapeHtml(min)}"`)
  if (max != null) attrs.push(`max="${escapeHtml(max)}"`)
  if (step != null) attrs.push(`step="${escapeHtml(step)}"`)
  return `<input ${attrs.join(' ')}>`
}

function checkbox (name, checked) {
  return `<span><input type="checkbox" name="${escapeHtml(name)}" value="1"${checked ? ' checked' : ''}>enabled</span>`
}

function select (name, value, options) {
  return `<select name="${escapeHtml(name)}">${options.map(([optionValue, label]) => {
    const selected = String(optionValue) === String(value) ? ' selected' : ''
    return `<option value="${escapeHtml(optionValue)}"${selected}>${escapeHtml(label)}</option>`
  }).join('')}</select>`
}

function profileSelect (profiles, selectedProfile) {
  return select('profileId', selectedProfile || 'default', profiles.map((profile) => [
    profile.id,
    `${profile.name || profile.id} (${profile.id})`
  ]))
}

function cleanString (value) {
  return String(value == null ? '' : value).trim()
}

function checkboxValue (value) {
  return value === '1' || value === 'on' || value === true
}

function arrayValue (value) {
  if (Array.isArray(value)) return value.filter((item) => item != null && item !== '')
  if (value == null || value === '') return []
  return [value]
}

function numberValue (value, fallback) {
  const parsed = Number(value)
  return Number.isFinite(parsed) ? parsed : fallback
}

function integerValue (value, fallback) {
  const parsed = Number.parseInt(value, 10)
  return Number.isFinite(parsed) ? parsed : fallback
}

function sanitizePresetId (value) {
  return cleanString(value)
    .toLowerCase()
    .replace(/[^a-z0-9._-]+/g, '-')
    .replace(/^-+|-+$/g, '')
}

function sanitizeWidgetId (value) {
  return cleanString(value)
    .replace(/[^A-Za-z0-9._-]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .slice(0, 31)
}

function statusError (status, message) {
  const err = new Error(message)
  err.status = status
  err.payload = { error: { code: 'invalid_request', message } }
  return err
}

function toYaml (value, indent = 0) {
  const pad = ' '.repeat(indent)
  if (Array.isArray(value)) {
    if (value.length === 0) return '[]\n'
    return value.map((item) => {
      if (item && typeof item === 'object') {
        return `${pad}-\n${toYaml(item, indent + 2)}`
      }
      return `${pad}- ${yamlScalar(item)}\n`
    }).join('')
  }
  if (value && typeof value === 'object') {
    const keys = Object.keys(value)
    if (keys.length === 0) return '{}\n'
    return keys.map((key) => {
      const item = value[key]
      if (item && typeof item === 'object') {
        return `${pad}${key}:\n${toYaml(item, indent + 2)}`
      }
      return `${pad}${key}: ${yamlScalar(item)}\n`
    }).join('')
  }
  return `${pad}${yamlScalar(value)}\n`
}

function yamlScalar (value) {
  if (value == null) return 'null'
  if (typeof value === 'number' || typeof value === 'boolean') return String(value)
  const s = String(value)
  if (/^[A-Za-z0-9_.:/@-]+$/.test(s) && !['true', 'false', 'null'].includes(s)) return s
  return JSON.stringify(s)
}

function fromYaml (text) {
  const lines = text.split(/\r?\n/)
    .map((raw) => ({ raw, indent: raw.match(/^ */)[0].length, text: raw.trim() }))
    .filter((line) => line.text && !line.text.startsWith('#'))
  const [value] = parseYamlBlock(lines, 0, 0)
  return value
}

function parseYamlBlock (lines, index, indent) {
  if (index >= lines.length) return [{}, index]
  if (lines[index].text.startsWith('-')) return parseYamlArray(lines, index, indent)
  return parseYamlObject(lines, index, indent)
}

function parseYamlObject (lines, index, indent) {
  const out = {}
  while (index < lines.length) {
    const line = lines[index]
    if (line.indent < indent || line.text.startsWith('-')) break
    if (line.indent > indent) throw statusError(400, 'invalid dashboard YAML indentation')
    const split = line.text.indexOf(':')
    if (split <= 0) throw statusError(400, 'invalid dashboard YAML mapping')
    const key = line.text.slice(0, split).trim()
    const rest = line.text.slice(split + 1).trim()
    if (rest) {
      out[key] = parseYamlScalar(rest)
      index++
    } else {
      const parsed = parseYamlBlock(lines, index + 1, indent + 2)
      out[key] = parsed[0]
      index = parsed[1]
    }
  }
  return [out, index]
}

function parseYamlArray (lines, index, indent) {
  const out = []
  while (index < lines.length) {
    const line = lines[index]
    if (line.indent < indent || !line.text.startsWith('-')) break
    if (line.indent > indent) throw statusError(400, 'invalid dashboard YAML indentation')
    const rest = line.text.slice(1).trim()
    if (rest) {
      out.push(parseYamlScalar(rest))
      index++
    } else {
      const parsed = parseYamlBlock(lines, index + 1, indent + 2)
      out.push(parsed[0])
      index = parsed[1]
    }
  }
  return [out, index]
}

function parseYamlScalar (value) {
  if (value === 'null') return null
  if (value === 'true') return true
  if (value === 'false') return false
  if (/^-?\d+(\.\d+)?$/.test(value)) return Number(value)
  if ((value.startsWith('"') && value.endsWith('"')) ||
      (value.startsWith('\'') && value.endsWith('\''))) {
    try {
      return JSON.parse(value.startsWith('"') ? value : JSON.stringify(value.slice(1, -1)))
    } catch (err) {
      return value.slice(1, -1)
    }
  }
  if (value === '[]') return []
  if (value === '{}') return {}
  return value
}

function renderDeviceConfigWidget (config, views) {
  const services = (((config.network || {}).mdns || {}).services || [])
    .map((service) => `${service.type}:${service.port}`)
    .join(', ')
  return `
    <div class="config-grid">
      ${configSection('Config', keyValueTable([
        ['Profile', config.profile],
        ['Version', config.version],
        ['Hash', code(config.hash)],
        ['Generated', config.generatedAt]
      ]))}
      ${configSection('Display', keyValueTable([
        ['Size', `${valueOr(config.display && config.display.width, '?')}x${valueOr(config.display && config.display.height, '?')}`],
        ['Shape', config.display && config.display.shape],
        ['Rotation', config.display && config.display.rotation],
        ['Selected variant', config.display && config.display.selectedVariant]
      ]))}
      ${configSection('Settings', keyValueTable([
        ['Default screen', config.settings && config.settings.defaultScreen],
        ['Theme', config.settings && config.settings.theme],
        ['Brightness', config.settings && config.settings.brightness],
        ['Demo mode', yesNo(config.settings && config.settings.demoMode)]
      ]))}
      ${configSection('Network', keyValueTable([
        ['Hostname', config.network && config.network.hostname],
        ['Domain', config.network && config.network.domain],
        ['FQDN', config.network && config.network.fqdn],
        ['mDNS', yesNo(config.network && config.network.mdns && config.network.mdns.enabled)],
        ['Services', services || 'none']
      ]))}
      ${configSection('SignalK and NMEA', keyValueTable([
        ['SignalK host', config.signalk && `${config.signalk.host}:${config.signalk.port}`],
        ['SignalK mDNS', yesNo(config.signalk && config.signalk.useMdns)],
        ['Source priority', config.sources && Array.isArray(config.sources.priority) ? config.sources.priority.join(', ') : ''],
        ['NMEA 0183 WiFi', nmeaLabel(config.nmea0183Wifi)]
      ]))}
      ${configSection('Device Web API', keyValueTable([
        ['Basic auth', yesNo(config.webAuth && config.webAuth.enabled)],
        ['Username', config.webAuth && config.webAuth.username],
        ['Password set', yesNo(config.webAuth && config.webAuth.password)]
      ]))}
      ${configSection('OTA', keyValueTable([
        ['Enabled', yesNo(config.ota && config.ota.enabled)],
        ['Mode', config.ota && config.ota.mode],
        ['Address', config.ota && `${config.ota.address}:${config.ota.port}`],
        ['Password set', yesNo(config.ota && config.ota.passwordSet)]
      ]))}
      ${configSection('Autopilot', keyValueTable([
        ['Enabled', yesNo(config.autopilot && config.autopilot.enabled)],
        ['Backend', config.autopilot && config.autopilot.backend],
        ['Engage allowed', yesNo(config.autopilot && config.autopilot.allowEngage)],
        ['Standby allowed', yesNo(config.autopilot && config.autopilot.allowStandby)],
        ['Heading adjust', yesNo(config.autopilot && config.autopilot.allowHeadingAdjust)]
      ]))}
      ${configSection('Debug', keyValueTable([
        ['Log level', config.debug && config.debug.logLevel],
        ['Touch mode', config.debug && config.debug.touchMode],
        ['Heartbeat', config.management && `${config.management.heartbeatMs} ms`],
        ['Command poll', config.management && `${config.management.commandPollMs} ms`]
      ]))}
      ${configSection('Widgets', renderWidgetsTable(config.widgets), true)}
      ${configSection('Screens', renderScreensTable(config.layout, views), true)}
    </div>`
}

function configSection (title, body, full) {
  return `<section class="config-section${full ? ' full' : ''}"><h2>${escapeHtml(title)}</h2>${body}</section>`
}

function keyValueTable (rows) {
  return `<table><tbody>${rows.map(([key, value]) => `
    <tr><th>${escapeHtml(key)}</th><td>${formatConfigValue(value)}</td></tr>`).join('')}</tbody></table>`
}

function simpleTable (headers, rows, emptyMessage) {
  const body = rows && rows.length
    ? rows.map((row) => `<tr>${row.map((value) => `<td>${formatConfigValue(value)}</td>`).join('')}</tr>`).join('')
    : `<tr><td colspan="${headers.length}">${escapeHtml(emptyMessage || 'No rows.')}</td></tr>`
  return `
    <table>
      <thead><tr>${headers.map((header) => `<th>${escapeHtml(header)}</th>`).join('')}</tr></thead>
      <tbody>${body}</tbody>
    </table>`
}

function renderWidgetsTable (widgets) {
  const items = widgets && widgets.items ? Object.entries(widgets.items) : []
  const rows = items.map(([id, widget]) => `
    <tr>
      <td><strong>${escapeHtml(widget.title || id)}</strong><br><span>${escapeHtml(id)}</span></td>
      <td><span class="pill">${escapeHtml(widget.type || '')}</span></td>
      <td>${escapeHtml(widget.path || '')}</td>
      <td>${fontSummary(widget)}</td>
    </tr>`).join('')
  const defaults = widgets && widgets.defaults ? `Defaults: ${escapeHtml(fontSummary(widgets.defaults))}` : ''
  return `
    <p class="muted">Variant ${escapeHtml(widgets && widgets.variant ? widgets.variant : 'default')}. ${defaults}</p>
    <table>
      <thead><tr><th>Widget</th><th>Type</th><th>SignalK path</th><th>Fonts</th></tr></thead>
      <tbody>${rows || '<tr><td colspan="4">No widgets selected for this device.</td></tr>'}</tbody>
    </table>`
}

function renderScreensTable (layout, views) {
  // Prefer the device's own reported screens (heartbeat ui.screens) so the list
  // reflects what the firmware actually renders, not the manager's generated
  // preset catalogue. Fall back to the generated layout when offline.
  const dvViews = (views && Array.isArray(views.views)) ? views.views : []
  if (dvViews.length) {
    const current = views.current
    const rows = dvViews.map((v) => `
      <tr>
        <td><strong>${escapeHtml(v.title || v.id)}</strong><br><span>${escapeHtml(v.id)}</span></td>
        <td><span class="pill">${escapeHtml(screenKindLabel(v.id))}</span></td>
        <td>${v.id === current ? '<span class="status good">on device</span>' : ''}</td>
      </tr>`).join('')
    return `
      <p class="muted">Discovered from the device (${dvViews.length} switchable screen${dvViews.length === 1 ? '' : 's'}). Built-in screens are rendered by the firmware.</p>
      <table>
        <thead><tr><th>Screen</th><th>Type</th><th>Current</th></tr></thead>
        <tbody>${rows}</tbody>
      </table>`
  }
  const screens = layout && Array.isArray(layout.screens) ? layout.screens : []
  const rows = screens.map((screen) => {
    const tiles = Array.isArray(screen.tiles) ? screen.tiles : []
    return `
      <tr>
        <td><strong>${escapeHtml(screen.id || '')}</strong><br><span>${escapeHtml(screen.type || '')}</span></td>
        <td>${escapeHtml(tiles.length)}</td>
        <td>${escapeHtml(tiles.map((tile) => tile.widget).filter(Boolean).join(', '))}</td>
      </tr>`
  }).join('')
  return `
    <p class="muted">Device offline — showing the manager's generated layout (variant ${escapeHtml(layout && layout.variant ? layout.variant : 'default')}).</p>
    <table>
      <thead><tr><th>Screen</th><th>Tiles</th><th>Widgets</th></tr></thead>
      <tbody>${rows || '<tr><td colspan="3">No screens selected for this device.</td></tr>'}</tbody>
    </table>`
}

function formatConfigValue (value) {
  if (value && value.__html) return value.__html
  if (Array.isArray(value)) return escapeHtml(value.join(', '))
  if (value == null || value === '') return '<span class="muted">unset</span>'
  return escapeHtml(value)
}

function code (value) {
  return { __html: `<code>${escapeHtml(value)}</code>` }
}

function valueOr (value, fallback) {
  return value == null || value === '' ? fallback : value
}

function yesNo (value) {
  return value ? 'yes' : 'no'
}

function nmeaLabel (nmea) {
  if (!nmea) return 'disabled'
  return `${nmea.enabled ? 'enabled' : 'disabled'} ${nmea.mode || ''} ${nmea.host || ''}:${nmea.port || ''}`.trim()
}

function fontSummary (settings) {
  return ['fontSize', 'labelFontSize', 'valueFontSize', 'unitFontSize', 'titleFontSize', 'buttonFontSize']
    .filter((key) => settings && settings[key] != null)
    .map((key) => `${key.replace('FontSize', '')}: ${settings[key]}`)
    .join(', ') || 'defaults'
}

function renderDiscoveryPage (manager, devices) {
  const profiles = manager.listProfiles().profiles
  const rows = devices.map((device) => `
        <tr>
          <td><strong>${escapeHtml(device.name || device.deviceId)}</strong><br><span>${escapeHtml(device.deviceId)}</span></td>
          <td>${escapeHtml(device.address || '')}:${escapeHtml(device.port || '')}</td>
          <td>${escapeHtml(device.source || '')}</td>
          <td>${escapeHtml(displayLabel(device.display))}</td>
          <td>${escapeHtml(firmwareLabel(device.firmware))}</td>
          <td>${device.registered ? '<span class="status ok">yes</span>' : '<span class="status">no</span>'}</td>
          <td>${device.stale ? '<span class="status bad">stale</span>' : '<span class="status ok">fresh</span>'}</td>
          <td>${device.conflict ? `<span class="status bad">address conflict</span><br><span>${escapeHtml(device.conflict.deviceIds.join(', '))}</span>` : '<span class="status ok">none</span>'}</td>
          <td>${escapeHtml(device.lastSeen || '')}</td>
          <td>${renderDiscoveryClaimControl(device, profiles)}</td>
        </tr>`).join('')
  return `
    <section class="panel">
      <h2>Discovered devices</h2>
      <p class="muted">Devices appear here from Bonjour/mDNS, UDP announcements, IP scans, or authenticated provisioning posts.</p>
      <div class="actions">
        <a href="/plugins/espdisp-manager/ui/discovery">Refresh</a>
      </div>
      ${renderDiscoveryScanForm()}
      <table>
        <thead><tr><th>Device</th><th>Address</th><th>Source</th><th>Display</th><th>Firmware</th><th>Registered</th><th>Freshness</th><th>Conflict</th><th>Last seen</th><th>Action</th></tr></thead>
        <tbody>${rows || '<tr><td colspan="10">No discovered devices.</td></tr>'}</tbody>
      </table>
    </section>`
}

function renderDiscoveryScanForm () {
  return `
      <form class="config-form" method="post" action="/plugins/espdisp-manager/discovery/scan">
        <fieldset>
          <legend>Scan network</legend>
          <div class="form-grid">
            ${field('Method', select('method', 'ip', [['ip', 'IP'], ['ble', 'BLE']]))}
            ${field('Target', input('target', ''))}
            ${field('Ports', input('ports', '80'))}
            ${field('Timeout ms', input('timeoutMs', '900', 'number', '250', '5000', '50'))}
          </div>
          <div class="actions">
            <button type="submit" name="action" value="scan">Scan</button>
          </div>
        </fieldset>
      </form>`
}

function renderDiscoveryClaimControl (device, profiles) {
  if (device.registered) {
    return `<a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.deviceId)}">Open</a>`
  }
  const blocked = []
  if (device.stale) blocked.push('stale')
  if (device.conflict) blocked.push('address conflict')
  if (!device.address) blocked.push('missing address')
  if (blocked.length) {
    return `
    <form method="post" action="/plugins/espdisp-manager/discovery/devices/${encodeURIComponent(device.deviceId)}/claim">
      ${profileSelect(profiles, 'default')}
      <button type="submit" disabled>Claim</button>
      <p class="muted">Resolve ${escapeHtml(blocked.join(', '))} before claiming.</p>
    </form>`
  }
  return `
    <form method="post" action="/plugins/espdisp-manager/discovery/devices/${encodeURIComponent(device.deviceId)}/claim">
      <input type="hidden" name="role" value="${escapeHtml(device.role || 'display')}">
      <input type="hidden" name="location" value="${escapeHtml(device.location || '')}">
      <input type="hidden" name="sendReload" value="1">
      ${profileSelect(profiles, 'default')}
      <button type="submit">Claim</button>
    </form>`
}

function renderProfilesPage (profiles, devices) {
  const rows = profiles.map((profile) => `
        <tr>
          <td><strong><a href="/plugins/espdisp-manager/ui/profiles/${encodeURIComponent(profile.id)}">${escapeHtml(profile.name || profile.id)}</a></strong><br><span>${escapeHtml(profile.id)}</span></td>
          <td>${escapeHtml(profile.version)}</td>
          <td>${escapeHtml(profile.updatedAt || '')}</td>
          <td>${escapeHtml(devices.filter((device) => device.profile === profile.id).length)}</td>
          <td>${escapeHtml(configSummary(profile.config || {}))}</td>
          <td><code>${escapeHtml(profile.hash || '')}</code><br><span><a href="/plugins/espdisp-manager/profiles/${encodeURIComponent(profile.id)}/dashboard.json">json</a> · <a href="/plugins/espdisp-manager/profiles/${encodeURIComponent(profile.id)}/dashboard.yaml">yaml</a></span></td>
        </tr>`).join('')
  return `
    <section class="panel">
      <h2>Device presets</h2>
      <p class="muted">Presets are shared profiles. Assign them from a device config page, then save per-device overrides or save changes back as a new preset.</p>
      <form class="config-form" method="post" action="/plugins/espdisp-manager/profiles/import-dashboard">
        <h2>Import dashboard preset</h2>
        <div class="form-grid">
          ${field('Preset id', input('presetId', 'imported-dashboard'))}
          ${field('Format', select('format', 'json', [['json', 'JSON'], ['yaml', 'YAML']]))}
        </div>
        <textarea name="raw" rows="10" placeholder="Paste espdisp.dashboard.v1 JSON or YAML here"></textarea>
        <div class="actions"><button type="submit">Import preset</button></div>
      </form>
      <table>
        <thead><tr><th>Preset</th><th>Version</th><th>Updated</th><th>Devices</th><th>Summary</th><th>Hash</th></tr></thead>
        <tbody>${rows || '<tr><td colspan="6">No presets configured.</td></tr>'}</tbody>
      </table>
    </section>`
}

function renderPresetPage (manager, profileId, devices) {
  const profile = manager.store.profiles.profiles[profileId]
  if (!profile) throw statusError(404, 'preset not found')
  const assigned = devices.filter((device) => device.profile === profile.id)
  const rows = devices.map((device) => {
    const checked = device.profile === profile.id ? ' checked' : ''
    return `
      <tr>
        <td><input type="checkbox" name="deviceIds" value="${escapeHtml(device.id)}"${checked}></td>
        <td><strong><a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.id)}/config">${escapeHtml(device.name || device.id)}</a></strong><br><span>${escapeHtml(device.id)}</span></td>
        <td>${escapeHtml(device.profile)}</td>
        <td>${escapeHtml(`${device.display.width}x${device.display.height}`)}</td>
        <td>${device.configDrift ? 'yes' : 'no'}</td>
        <td>${device.pendingCommands}</td>
      </tr>`
  }).join('')
  return `
    <section class="panel">
      <h2>${escapeHtml(profile.name || profile.id)}</h2>
      <p class="muted">${escapeHtml(profile.id)} · version ${escapeHtml(profile.version)} · ${escapeHtml(assigned.length)} device(s)</p>
      <p><a href="/plugins/espdisp-manager/ui/profiles">Back to presets</a></p>
      <table>
        <tbody>
          <tr><th>Updated</th><td>${escapeHtml(profile.updatedAt || '')}</td></tr>
          <tr><th>Summary</th><td>${escapeHtml(configSummary(profile.config || {}))}</td></tr>
          <tr><th>Hash</th><td><code>${escapeHtml(profile.hash || '')}</code></td></tr>
        </tbody>
      </table>
      <form class="config-form" method="post" action="/plugins/espdisp-manager/ui/profiles/${encodeURIComponent(profile.id)}/apply">
        <h2>Apply preset</h2>
        <table>
          <thead><tr><th></th><th>Device</th><th>Current preset</th><th>Display</th><th>Drift</th><th>Pending</th></tr></thead>
          <tbody>${rows || '<tr><td colspan="6">No devices registered.</td></tr>'}</tbody>
        </table>
        <fieldset>
          <legend>Apply options</legend>
          <div class="form-grid">
            ${field('Clear device overrides', checkbox('clearOverrides', true))}
            ${field('Send reload command', checkbox('sendReload', true))}
          </div>
        </fieldset>
        <div class="actions">
          <button type="submit">Apply to selected devices</button>
        </div>
      </form>
    </section>`
}

function configSummary (config) {
  const parts = []
  if (config.settings && config.settings.theme) parts.push(`theme ${config.settings.theme}`)
  if (config.settings && config.settings.brightness != null) parts.push(`brightness ${config.settings.brightness}`)
  if (config.widgets && config.widgets.defaults && config.widgets.defaults.valueFontSize) parts.push(`value font ${config.widgets.defaults.valueFontSize}`)
  if (config.nmea0183Wifi) parts.push(`NMEA ${config.nmea0183Wifi.enabled ? 'on' : 'off'}`)
  return parts.join(', ') || 'base layout'
}

function renderFirmwarePage (catalog, jobs, upgrades) {
  const artifactRows = (catalog.artifacts || []).map((artifact) => `
        <tr>
          <td><strong>${escapeHtml(artifact.firmware && artifact.firmware.version)}</strong><br><span>${escapeHtml(artifact.artifactId)}</span></td>
          <td>${escapeHtml(artifact.vendor && artifact.vendor.id)}</td>
          <td>${escapeHtml(artifact.product && artifact.product.id)}</td>
          <td>${escapeHtml(firmwareSourceLabel(artifact))}</td>
          <td><code>${escapeHtml(artifact.file && artifact.file.sha256)}</code></td>
          <td>
            <!-- Same JS-attribute-context XSS risk as the device Remove
                 button above. Firmware version + artifactId come from
                 GitHub release metadata in the common case but the
                 catalogue also accepts operator-uploaded artifacts
                 with arbitrary version strings, so we can't trust them
                 inside an inline onsubmit. Static prompt; the row
                 itself shows which artifact is being deleted. -->
            <form method="post" action="/plugins/espdisp-manager/ui/firmware/artifacts/${encodeURIComponent(artifact.artifactId)}/delete"
                  onsubmit="return confirm('Remove this artifact from catalogue?')"
                  style="margin:0;display:inline;">
              <button type="submit" style="background:#c0392b;border-color:#a82716;">Delete</button>
            </form>
          </td>
        </tr>`).join('')
  const upgradeRows = ((upgrades && upgrades.devices) || []).map((device) => {
    const versions = device.compatibleArtifacts.length > 0
      ? device.compatibleArtifacts.map((artifact) => {
          const version = artifact.firmware && artifact.firmware.version ? artifact.firmware.version : artifact.artifactId
          const marker = artifact.sameVersion ? ' current' : ''
          return `<span class="pill">${escapeHtml(`${version}${marker} · ${firmwareSourceLabel(artifact)}`)}</span>`
        }).join(' ')
      : '<span>None for this board/chip.</span>'
    const action = device.availableArtifacts.length > 0
      ? device.availableArtifacts.map((artifact) => `
          <form method="post" action="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.deviceId)}/firmware/update" style="display:inline-block; margin:0 6px 6px 0;">
            <input type="hidden" name="artifactId" value="${escapeHtml(artifact.artifactId)}">
            <input type="hidden" name="reboot" value="true">
            <input type="hidden" name="confirmAfterBoot" value="true">
            <button type="submit">Queue update ${escapeHtml(artifact.firmware && artifact.firmware.version)}</button>
          </form>`).join('')
      : '<span>No update action.</span>'
    const jobText = device.activeJobs.length > 0
      ? `<br><span>${escapeHtml(device.activeJobs.length)} active firmware job(s)</span>`
      : ''
    return `
        <tr>
          <td><strong><a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.deviceId)}">${escapeHtml(device.name)}</a></strong><br><span>${escapeHtml(device.deviceId)}</span></td>
          <td>${escapeHtml(device.board || '')}<br><span>${escapeHtml(device.chip || '')}</span></td>
          <td>${escapeHtml(device.currentVersion || 'unknown')}</td>
          <td>${versions}</td>
          <td><span class="status ${device.upgradable ? 'ok' : ''}">${escapeHtml(device.status)}</span>${jobText}</td>
          <td>${action}</td>
        </tr>`
  }).join('')
  const github = catalog.github || {}
  return `
    <section class="panel">
      <h2>Device upgrade status</h2>
      <form method="post" action="/plugins/espdisp-manager/ui/firmware/catalog/refresh" class="actions" style="margin-bottom: 12px;">
        <button type="submit">Refresh catalog from GitHub</button>
        <span class="muted">Last GitHub check: ${escapeHtml(github.checkedAt || 'never')} · release ${escapeHtml(github.release || 'unknown')}</span>
      </form>
      <table>
        <thead><tr><th>Device</th><th>Board</th><th>Current firmware</th><th>Available versions</th><th>Status</th><th>Action</th></tr></thead>
        <tbody>${upgradeRows || '<tr><td colspan="6">No devices registered.</td></tr>'}</tbody>
      </table>
      <h2>Firmware catalogue (${(catalog.artifacts || []).length})</h2>
      <p class="muted">Build artefacts available for OTA. Use Delete to remove an old version from this catalogue (does not delete the binary; active jobs keep running).</p>
      <table>
        <thead><tr><th>Firmware</th><th>Vendor</th><th>Product</th><th>Source</th><th>SHA-256</th><th></th></tr></thead>
        <tbody>${artifactRows || '<tr><td colspan="6">No firmware artifacts.</td></tr>'}</tbody>
      </table>
      <h2>Recent jobs</h2>
      ${firmwareJobTable(jobs || [])}
    </section>`
}

function deviceTable (devices) {
  const rows = devices.map((device) => `
        <tr>
          <td><strong><a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.id)}">${escapeHtml(device.name || device.id)}</a></strong><br><span>${escapeHtml(device.id)}</span></td>
          <td><span class="status ${device.online ? 'ok' : 'bad'}">${escapeHtml(device.health)}</span></td>
          <td>${escapeHtml(device.profile)}</td>
          <td>${escapeHtml(`${device.display.width}x${device.display.height}`)}</td>
          <td>${escapeHtml(device.desiredConfig.layoutVariant || '')}</td>
          <td>${escapeHtml(device.desiredConfig.widgetVariant || '')}</td>
          <td>${device.configDrift ? 'yes' : 'no'}</td>
          <td>${device.pendingCommands}<br><span><a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.id)}/config">config</a></span></td>
          <td>
            <!-- Static confirm prompt - device.name is attacker-controlled
                 in principle (registers via authenticated POST but still
                 untrusted free-form text), and escapeHtml does NOT escape
                 the single-quote-context inside an inline JS attribute.
                 A name like \`'); alert(1); //\` would execute. The device
                 row above already labels which device this Remove button
                 acts on, so a generic prompt is no UX loss. -->
            <form method="post" action="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.id)}/delete"
                  onsubmit="return confirm('Remove this device? Pending commands are dropped.')"
                  style="margin:0;display:inline;">
              <button type="submit" style="background:#c0392b;border-color:#a82716;">Remove</button>
            </form>
          </td>
        </tr>`).join('')
  return `
    <table>
      <thead>
        <tr>
          <th>Device</th>
          <th>Health</th>
          <th>Profile</th>
          <th>Display</th>
          <th>Layout</th>
          <th>Widgets</th>
          <th>Drift</th>
          <th>Pending</th>
          <th></th>
        </tr>
      </thead>
      <tbody>${rows || '<tr><td colspan="9">No devices registered.</td></tr>'}</tbody>
    </table>`
}

function commandTable (commands) {
  const rows = commands.map((command) => `
        <tr>
          <td><code>${escapeHtml(command.id)}</code></td>
          <td>${escapeHtml(command.type)}</td>
          <td>${escapeHtml(command.status)}</td>
          <td>${escapeHtml(command.createdAt)}</td>
        </tr>`).join('')
  return `<table><thead><tr><th>ID</th><th>Type</th><th>Status</th><th>Created</th></tr></thead><tbody>${rows || '<tr><td colspan="4">No commands.</td></tr>'}</tbody></table>`
}

function firmwareJobTable (jobs) {
  const rows = jobs.map((job) => `
        <tr>
          <td><code>${escapeHtml(job.jobId)}</code></td>
          <td>${escapeHtml(job.deviceId)}</td>
          <td>${escapeHtml(job.artifactId)}</td>
          <td>${escapeHtml(job.status)}</td>
          <td>${escapeHtml(job.createdAt)}</td>
        </tr>`).join('')
  return `<table><thead><tr><th>Job</th><th>Device</th><th>Artifact</th><th>Status</th><th>Created</th></tr></thead><tbody>${rows || '<tr><td colspan="5">No firmware jobs.</td></tr>'}</tbody></table>`
}

function nav (active) {
  // Reorganised 2026-06-04: discovery folded into devices, layout
  // editor surfaced as a top-level nav item. Editor link uses the
  // /plugins/.../ui/layout route which serves the editor INSIDE
  // the same iframe shell as the rest of the UI - linking directly
  // to /signalk-espdisp-manager/layout-editor.html would break out
  // of the iframe and confuse the SK admin sidebar's "back to
  // plugin" affordance.
  const items = [
    ['devices', '/plugins/espdisp-manager/ui/devices', 'Devices'],
    ['profiles', '/plugins/espdisp-manager/ui/profiles', 'Presets'],
    ['layout', '/plugins/espdisp-manager/ui/layout', 'Layout editor'],
    ['firmware', '/plugins/espdisp-manager/ui/firmware', 'Firmware'],
    ['overview', '/plugins/espdisp-manager/ui', 'Overview']
  ]
  return `<nav>${items.map(([id, href, label]) => `<a class="${active === id ? 'active' : ''}" href="${href}">${label}</a>`).join('')}</nav>`
}

function displayLabel (display) {
  if (!display) return ''
  return `${display.width || '?'}x${display.height || '?'}${display.shape ? ` ${display.shape}` : ''}`
}

function firmwareLabel (firmware) {
  if (!firmware) return ''
  return firmware.version || firmware.name || firmware.id || 'custom firmware'
}

function firmwareSourceLabel (artifact) {
  if (artifact && artifact.file && artifact.file.url) return 'GitHub'
  if (artifact && artifact.file && artifact.file.path) return 'SignalK'
  return 'SignalK metadata'
}

function metric (value, label) {
  return `<div class="metric"><b>${value}</b><span>${escapeHtml(label)}</span></div>`
}

function escapeHtml (value) {
  return String(value == null ? '' : value)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

// Opt-in SignalK meta write-back for "save limits". Emits a SignalK `meta`
// delta (displayScale + zones) onto the bound path so the source and other
// clients share the operator's configured limits. Best-effort: resolves on a
// successful emit, rejects when the SignalK app can't accept it (caller maps
// the rejection to metaWriteBack:'failed' and still persists onto the field).
function writeSignalKMeta (manager, path, limits) {
  return new Promise((resolve, reject) => {
    const app = manager && manager.app
    if (!app || typeof app.handleMessage !== 'function') {
      return reject(new Error('signalk_app_unavailable'))
    }
    const value = {}
    if (limits && limits.range && typeof limits.range.min === 'number' && typeof limits.range.max === 'number') {
      value.displayScale = { lower: limits.range.min, upper: limits.range.max }
    }
    if (limits && Array.isArray(limits.zones) && limits.zones.length) {
      value.zones = limits.zones
    }
    if (!Object.keys(value).length) return reject(new Error('no_limits_to_write'))
    try {
      app.handleMessage('espdisp-manager', {
        updates: [{ meta: [{ path: String(path), value }] }]
      })
      resolve()
    } catch (e) {
      reject(e)
    }
  })
}

module.exports._test = {
  toYaml,
  fromYaml,
  renderUi,
  importDashboardPreset,
  applyPresetForm,
  configOverridesFromForm
}
