const fs = require('fs')
const os = require('os')
const path = require('path')
const { EspDispManager } = require('../lib/manager')

function makeManager (options) {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'espdisp-manager-'))
  const app = {
    getDataDirPath: () => dataDir,
    debug: () => {}
  }
  const merged = mergeTestOptions({
    discoveryUdp: { enabled: false },
    deviceDiscoveryUdp: { enabled: false },
    firmware: { github: { enabled: false } },
    network: { mdns: { browser: false, advertiseManager: false } }
  }, options || {})
  return {
    dataDir,
    app,
    manager: new EspDispManager(app, merged),
    auth: { bearer: options && options.auth && options.auth.devToken ? options.auth.devToken : 'espdisp-dev' }
  }
}

function mergeTestOptions (base, override) {
  const out = { ...base }
  for (const [key, value] of Object.entries(override || {})) {
    if (value && typeof value === 'object' && !Array.isArray(value) &&
        out[key] && typeof out[key] === 'object' && !Array.isArray(out[key])) {
      out[key] = mergeTestOptions(out[key], value)
    } else {
      out[key] = value
    }
  }
  return out
}

module.exports = { makeManager }
