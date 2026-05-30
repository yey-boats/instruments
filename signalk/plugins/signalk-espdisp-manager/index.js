const { EspDispManager } = require('./lib/manager')
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
          default: 5000
        },
        commandPollMs: {
          type: 'number',
          title: 'Command poll interval, ms',
          default: 1000
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
        '/plugins/espdisp-manager/devices/{deviceId}/firmware/jobs': {
          get: { summary: 'List firmware jobs' },
          post: { summary: 'Create firmware update job' }
        }
      }
    })
  }

  return plugin
}

function registerRoutes (router, getManager) {
  router.use(jsonBody)

  router.get('/.well-known/espdisp-management', wrap(getManager, (manager, req, res) => {
    res.json(manager.discovery())
  }))

  router.get('/devices', wrap(getManager, (manager, req, res) => {
    res.json(manager.listDevices(req.query || {}))
  }))

  router.get('/discovery/devices', wrap(getManager, (manager, req, res) => {
    res.json(manager.listDiscoveredDevices())
  }))

  router.post('/discovery/devices', wrap(getManager, (manager, req, res) => {
    res.json(manager.announceDiscoveredDevice(req.body || {}, authFrom(req)))
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

  router.get('/ui/devices/:id', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'device', req))
  }))

  router.get('/ui/devices/:id/config', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'deviceConfig', req))
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

  router.get('/ui/profiles', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'profiles', req))
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

  router.get('/ui/firmware', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'firmware', req))
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

  router.post('/firmware/artifacts', wrap(getManager, (manager, req, res) => {
    res.json(manager.addFirmwareArtifact(req.body || {}))
  }))

  router.get('/firmware/artifacts/:artifactId', wrap(getManager, (manager, req, res) => {
    res.json(manager.getFirmwareArtifact(req.params.artifactId))
  }))

  router.get('/firmware/download/:jobId', wrap(getManager, (manager, req, res) => {
    const info = manager.firmwareDownloadInfo(req.params.jobId)
    res.json(info)
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
    ${renderPage(manager, dashboard, page, req)}
  </main>
</body>
</html>`
}

function renderPage (manager, dashboard, page, req) {
  if (page === 'devices') return renderDevicesPage(dashboard.devices)
  if (page === 'device') return renderDevicePage(manager, req.params.id)
  if (page === 'deviceConfig') return renderDeviceConfigPage(manager, req.params.id)
  if (page === 'discovery') return renderDiscoveryPage(manager, manager.listDiscoveredDevices().devices)
  if (page === 'profiles') return renderProfilesPage(manager.listProfiles().profiles, dashboard.devices)
  if (page === 'preset') return renderPresetPage(manager, req.params.id, dashboard.devices)
  if (page === 'firmware') return renderFirmwarePage(manager.listFirmware(), dashboard.recentFirmwareJobs)
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

function renderDevicesPage (devices) {
  return `
    <section class="panel">
      <h2>Registered devices</h2>
      ${deviceTable(devices)}
    </section>`
}

function renderDevicePage (manager, id) {
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
  return `
    <section class="panel">
      <h2>${escapeHtml(device.name || device.id)}</h2>
      <p class="muted">${escapeHtml(device.id)} · ${escapeHtml(device.role)} · ${escapeHtml(device.location || 'unassigned')}</p>
      <p>
        <a href="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(id)}/config">Open generated config</a>
        · <a href="/plugins/espdisp-manager/devices/${encodeURIComponent(id)}/live/status">Live status JSON</a>
        · <a href="/plugins/espdisp-manager/devices/${encodeURIComponent(id)}/live/logs">Live logs JSON</a>
      </p>
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

function renderDeviceConfigPage (manager, id) {
  const device = manager.getDevice(id)
  const config = manager.generateConfig(id)
  const profiles = manager.listProfiles().profiles
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
      ${renderDeviceConfigForm(device, config, profiles)}
      ${renderDeviceConfigWidget(config)}
    </section>`
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
      }
    },
    debug: {
      logLevel: cleanString(body.logLevel) || 'info',
      touchMode: cleanString(body.touchMode) || 'irq'
    }
  }
}

function renderDeviceConfigForm (device, config, profiles) {
  const settings = config.settings || {}
  const nmea = config.nmea0183Wifi || {}
  const autopilot = config.autopilot || {}
  const widgets = (config.widgets && config.widgets.defaults) || {}
  const debug = config.debug || {}
  return `
    <form class="config-form" method="post" action="/plugins/espdisp-manager/ui/devices/${encodeURIComponent(device.id)}/config">
      <h2>Configure device</h2>
      <div class="form-grid">
        ${field('Preset', profileSelect(profiles, device.assignedProfile || config.profile))}
        ${field('Default screen', input('defaultScreen', settings.defaultScreen || 'dashboard'))}
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

function renderDeviceConfigWidget (config) {
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
      ${configSection('Screens', renderScreensTable(config.layout), true)}
    </div>`
}

function configSection (title, body, full) {
  return `<section class="config-section${full ? ' full' : ''}"><h2>${escapeHtml(title)}</h2>${body}</section>`
}

function keyValueTable (rows) {
  return `<table><tbody>${rows.map(([key, value]) => `
    <tr><th>${escapeHtml(key)}</th><td>${formatConfigValue(value)}</td></tr>`).join('')}</tbody></table>`
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

function renderScreensTable (layout) {
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
    <p class="muted">Variant ${escapeHtml(layout && layout.variant ? layout.variant : 'default')}.</p>
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
      <p class="muted">Devices appear here from Bonjour/mDNS, UDP announcements, or authenticated provisioning posts.</p>
      <table>
        <thead><tr><th>Device</th><th>Address</th><th>Source</th><th>Display</th><th>Firmware</th><th>Registered</th><th>Freshness</th><th>Conflict</th><th>Last seen</th><th>Action</th></tr></thead>
        <tbody>${rows || '<tr><td colspan="10">No discovered devices.</td></tr>'}</tbody>
      </table>
    </section>`
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

function renderFirmwarePage (catalog, jobs) {
  const artifactRows = (catalog.artifacts || []).map((artifact) => `
        <tr>
          <td><strong>${escapeHtml(artifact.firmware && artifact.firmware.version)}</strong><br><span>${escapeHtml(artifact.artifactId)}</span></td>
          <td>${escapeHtml(artifact.vendor && artifact.vendor.id)}</td>
          <td>${escapeHtml(artifact.product && artifact.product.id)}</td>
          <td><code>${escapeHtml(artifact.file && artifact.file.sha256)}</code></td>
        </tr>`).join('')
  return `
    <section class="panel">
      <h2>Firmware artifacts</h2>
      <table>
        <thead><tr><th>Firmware</th><th>Vendor</th><th>Product</th><th>SHA-256</th></tr></thead>
        <tbody>${artifactRows || '<tr><td colspan="4">No firmware artifacts.</td></tr>'}</tbody>
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
        </tr>
      </thead>
      <tbody>${rows || '<tr><td colspan="8">No devices registered.</td></tr>'}</tbody>
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
  const items = [
    ['overview', '/plugins/espdisp-manager/ui', 'Overview'],
    ['devices', '/plugins/espdisp-manager/ui/devices', 'Devices'],
    ['discovery', '/plugins/espdisp-manager/ui/discovery', 'Discovery'],
    ['profiles', '/plugins/espdisp-manager/ui/profiles', 'Presets'],
    ['firmware', '/plugins/espdisp-manager/ui/firmware', 'Firmware']
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

module.exports._test = {
  toYaml,
  fromYaml,
  renderUi,
  importDashboardPreset,
  applyPresetForm
}
