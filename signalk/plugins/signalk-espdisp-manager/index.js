const { EspDispManager } = require('./lib/manager')

module.exports = function espdispManagerPlugin (app) {
  let manager

  const plugin = {
    id: 'espdisp-manager',
    name: 'ESP Display Manager',
    description: 'Registry, central configuration, command queue, and firmware management for ESP display devices.',
    schema: () => ({
      type: 'object',
      title: 'ESP Display Manager',
      properties: {
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
                enabled: { type: 'boolean', title: 'Enabled', default: true }
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
        version: '0.1.0'
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

  router.get('/ui/discovery', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'discovery', req))
  }))

  router.get('/ui/profiles', wrap(getManager, (manager, req, res) => {
    res.setHeader('content-type', 'text/html; charset=utf-8')
    res.end(renderUi(manager, 'profiles', req))
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

  router.get('/devices/:id/config', wrap(getManager, (manager, req, res) => {
    manager.requireDeviceAuth(req.params.id, authFrom(req))
    res.json(manager.generateConfig(req.params.id))
  }))

  router.get('/profiles', wrap(getManager, (manager, req, res) => {
    res.json(manager.listProfiles())
  }))

  router.post('/profiles', wrap(getManager, (manager, req, res) => {
    res.json(manager.upsertProfile(req.body || {}))
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
      handler(manager, req, res)
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
        req.body = JSON.parse(body)
      } catch (err) {
        res.status(400).json({ error: { code: 'invalid_json', message: 'invalid JSON body' } })
        return
      }
    }
    next()
  })
}

function renderUi (manager, page, req) {
  const dashboard = manager.dashboard()
  const title = {
    overview: 'Overview',
    devices: 'Devices',
    device: 'Device detail',
    discovery: 'Discovery',
    profiles: 'Profiles',
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
    .status { display: inline-block; min-width: 64px; padding: 2px 7px; border-radius: 999px; background: #eef3f5; text-align: center; }
    .ok { background: #d9f2e3; color: #145d32; }
    .bad { background: #ffe0df; color: #8a1f18; }
    @media (max-width: 850px) { .grid { grid-template-columns: repeat(2, minmax(120px, 1fr)); } table { font-size: 12px; } }
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
  if (page === 'discovery') return renderDiscoveryPage(manager.listDiscoveredDevices().devices)
  if (page === 'profiles') return renderProfilesPage(manager.listProfiles().profiles)
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
      <h2>Generated config preview</h2>
      <pre>${escapeHtml(JSON.stringify(config, null, 2))}</pre>
    </section>`
}

function renderDiscoveryPage (devices) {
  const rows = devices.map((device) => `
        <tr>
          <td><strong>${escapeHtml(device.name || device.deviceId)}</strong><br><span>${escapeHtml(device.deviceId)}</span></td>
          <td>${escapeHtml(device.address || '')}:${escapeHtml(device.port || '')}</td>
          <td>${escapeHtml(displayLabel(device.display))}</td>
          <td>${escapeHtml(firmwareLabel(device.firmware))}</td>
          <td>${device.registered ? '<span class="status ok">yes</span>' : '<span class="status">no</span>'}</td>
          <td>${device.stale ? '<span class="status bad">stale</span>' : '<span class="status ok">fresh</span>'}</td>
          <td>${escapeHtml(device.lastSeen || '')}</td>
        </tr>`).join('')
  return `
    <section class="panel">
      <h2>Discovered devices</h2>
      <p class="muted">Devices appear here after an mDNS/provisioning announcement posts to <code>/discovery/devices</code>.</p>
      <table>
        <thead><tr><th>Device</th><th>Address</th><th>Display</th><th>Firmware</th><th>Registered</th><th>Freshness</th><th>Last seen</th></tr></thead>
        <tbody>${rows || '<tr><td colspan="7">No discovered devices.</td></tr>'}</tbody>
      </table>
    </section>`
}

function renderProfilesPage (profiles) {
  const rows = profiles.map((profile) => `
        <tr>
          <td><strong>${escapeHtml(profile.name || profile.id)}</strong><br><span>${escapeHtml(profile.id)}</span></td>
          <td>${escapeHtml(profile.version)}</td>
          <td>${escapeHtml(profile.updatedAt || '')}</td>
          <td><code>${escapeHtml(profile.hash || '')}</code></td>
        </tr>`).join('')
  return `
    <section class="panel">
      <h2>Profiles</h2>
      <table>
        <thead><tr><th>Profile</th><th>Version</th><th>Updated</th><th>Hash</th></tr></thead>
        <tbody>${rows || '<tr><td colspan="4">No profiles configured.</td></tr>'}</tbody>
      </table>
    </section>`
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
          <td>${device.pendingCommands}</td>
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
    ['profiles', '/plugins/espdisp-manager/ui/profiles', 'Profiles'],
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
  return firmware.version || firmware.name || JSON.stringify(firmware)
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
