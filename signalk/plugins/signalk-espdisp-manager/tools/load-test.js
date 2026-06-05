#!/usr/bin/env node
'use strict'

const { performance } = require('perf_hooks')

const DEFAULTS = {
  url: process.env.SIGNALK_URL || 'http://localhost:3000',
  devices: 20,
  durationSec: 120,
  heartbeatMs: 30000,
  commandPollMs: 15000,
  deviceToken: process.env.ESPDISP_MANAGER_TOKEN || 'espdisp-dev',
  signalkToken: process.env.SIGNALK_TOKEN || '',
  username: process.env.SIGNALK_USERNAME || 'admin',
  password: process.env.SIGNALK_PASSWORD || 'admin',
  prefix: 'loadtest',
  timeoutMs: 5000,
  configEvery: 0,
  jitterMs: 5000
}

function parseArgs (argv) {
  const out = { ...DEFAULTS }
  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i]
    if (arg === '--help' || arg === '-h') {
      usage()
      process.exit(0)
    }
    if (!arg.startsWith('--')) throw new Error(`unexpected argument: ${arg}`)
    const key = arg.slice(2)
    const next = argv[i + 1]
    if (next == null || next.startsWith('--')) throw new Error(`missing value for --${key}`)
    i++
    if (key === 'url') out.url = next
    else if (key === 'token' || key === 'device-token') out.deviceToken = next
    else if (key === 'signalk-token') out.signalkToken = next
    else if (key === 'username') out.username = next
    else if (key === 'password') out.password = next
    else if (key === 'prefix') out.prefix = next
    else if (key === 'devices') out.devices = positiveInt(key, next)
    else if (key === 'duration-sec') out.durationSec = positiveInt(key, next)
    else if (key === 'heartbeat-ms') out.heartbeatMs = positiveInt(key, next)
    else if (key === 'command-poll-ms') out.commandPollMs = positiveInt(key, next)
    else if (key === 'timeout-ms') out.timeoutMs = positiveInt(key, next)
    else if (key === 'config-every') out.configEvery = nonNegativeInt(key, next)
    else if (key === 'jitter-ms') out.jitterMs = nonNegativeInt(key, next)
    else throw new Error(`unknown option: --${key}`)
  }
  out.url = out.url.replace(/\/+$/, '')
  return out
}

function positiveInt (key, value) {
  const n = Number(value)
  if (!Number.isInteger(n) || n <= 0) throw new Error(`--${key} must be a positive integer`)
  return n
}

function nonNegativeInt (key, value) {
  const n = Number(value)
  if (!Number.isInteger(n) || n < 0) throw new Error(`--${key} must be a non-negative integer`)
  return n
}

function usage () {
  console.log(`Usage:
  node tools/load-test.js [options]

Options:
  --url <url>                 SignalK root URL (default: ${DEFAULTS.url})
  --devices <n>               Simulated devices (default: ${DEFAULTS.devices})
  --duration-sec <n>          Benchmark duration after registration (default: ${DEFAULTS.durationSec})
  --heartbeat-ms <n>          Status POST interval per device (default: ${DEFAULTS.heartbeatMs})
  --command-poll-ms <n>       Command poll GET interval per device (default: ${DEFAULTS.commandPollMs})
  --config-every <n>          Fetch config every n heartbeats; 0 disables periodic config GET
  --device-token <token>      ESP manager device/dev token (default: $ESPDISP_MANAGER_TOKEN or espdisp-dev)
  --token <token>             Alias for --device-token
  --signalk-token <token>     Existing SignalK bearer token (default: $SIGNALK_TOKEN)
  --username <name>           SignalK login username when no --signalk-token (default: $SIGNALK_USERNAME or admin)
  --password <password>       SignalK login password when no --signalk-token (default: $SIGNALK_PASSWORD or admin)
  --prefix <name>             Device id prefix (default: ${DEFAULTS.prefix})
  --timeout-ms <n>            Per-request timeout (default: ${DEFAULTS.timeoutMs})
  --jitter-ms <n>             Startup jitter spread per loop (default: ${DEFAULTS.jitterMs})

Examples:
  node tools/load-test.js --url http://192.168.2.11:3000 --devices 20 --duration-sec 180
  node tools/load-test.js --devices 75 --heartbeat-ms 30000 --command-poll-ms 15000 --config-every 10
`)
}

function sleep (ms) {
  return new Promise(resolve => setTimeout(resolve, ms))
}

function percentile (values, pct) {
  if (values.length === 0) return 0
  const i = Math.min(values.length - 1, Math.ceil((pct / 100) * values.length) - 1)
  return values[i]
}

class Metrics {
  constructor () {
    this.byName = new Map()
    this.startedAt = performance.now()
  }

  record (name, ms, ok, status) {
    if (!this.byName.has(name)) {
      this.byName.set(name, {
        count: 0,
        ok: 0,
        failed: 0,
        statuses: new Map(),
        latencies: []
      })
    }
    const m = this.byName.get(name)
    m.count++
    if (ok) m.ok++
    else m.failed++
    m.statuses.set(status, (m.statuses.get(status) || 0) + 1)
    m.latencies.push(ms)
  }

  print () {
    const elapsedSec = (performance.now() - this.startedAt) / 1000
    console.log(`\nLoad test summary (${elapsedSec.toFixed(1)} s)`)
    console.log('operation          count  ok  fail  rps    p50ms  p95ms  p99ms  maxms  statuses')
    for (const [name, m] of this.byName.entries()) {
      m.latencies.sort((a, b) => a - b)
      const statuses = [...m.statuses.entries()].map(([k, v]) => `${k}:${v}`).join(',')
      const row = [
        name.padEnd(18),
        String(m.count).padStart(5),
        String(m.ok).padStart(4),
        String(m.failed).padStart(5),
        (m.count / elapsedSec).toFixed(2).padStart(5),
        percentile(m.latencies, 50).toFixed(1).padStart(7),
        percentile(m.latencies, 95).toFixed(1).padStart(7),
        percentile(m.latencies, 99).toFixed(1).padStart(7),
        (m.latencies[m.latencies.length - 1] || 0).toFixed(1).padStart(6),
        statuses
      ]
      console.log(row.join('  '))
    }
  }
}

async function requestJson (opts, metrics, name, method, path, body) {
  const controller = new AbortController()
  const timeout = setTimeout(() => controller.abort(), opts.timeoutMs)
  const started = performance.now()
  let status = 'ERR'
  try {
    const res = await fetch(`${opts.url}${path}`, {
      method,
      signal: controller.signal,
      headers: {
        ...(opts.signalkToken ? { Authorization: `Bearer ${opts.signalkToken}` } : {}),
        'X-EspDisp-Authorization': `Bearer ${opts.deviceToken}`,
        ...(body ? { 'Content-Type': 'application/json' } : {})
      },
      body: body ? JSON.stringify(body) : undefined
    })
    status = String(res.status)
    const text = await res.text()
    let data = null
    if (text) {
      try {
        data = JSON.parse(text)
      } catch (err) {
        data = { raw: text }
      }
    }
    metrics.record(name, performance.now() - started, res.ok, status)
    if (!res.ok) {
      const msg = data && data.error && data.error.message ? data.error.message : text
      throw new Error(`${method} ${path} -> ${res.status}: ${msg}`)
    }
    return data
  } catch (err) {
    if (status === 'ERR') metrics.record(name, performance.now() - started, false, err.name || 'ERR')
    throw err
  } finally {
    clearTimeout(timeout)
  }
}

async function login (opts, metrics) {
  if (opts.signalkToken) return opts.signalkToken
  const data = await requestJson(opts, metrics, 'login', 'POST', '/signalk/v1/auth/login', {
    username: opts.username,
    password: opts.password
  })
  if (!data || !data.token) throw new Error('SignalK login response did not include token')
  opts.signalkToken = data.token
  return data.token
}

function deviceIdentity (opts, index) {
  const suffix = String(index).padStart(4, '0')
  const id = `${opts.prefix}-${suffix}`
  return {
    id,
    deviceId: id,
    name: `Load Test ${suffix}`,
    board: 'esp32-4848s040',
    role: 'display',
    firmware: {
      version: 'load-test',
      build_time: new Date(0).toISOString(),
      git_commit: 'loadtest'
    },
    display: {
      width: 480,
      height: 480,
      rotation: 0,
      shape: 'square',
      density: 'mdpi',
      layoutClass: 'square'
    },
    capabilities: {
      touch: true,
      psram: true
    }
  }
}

function statusBody (id, heartbeatNo) {
  const nowMs = Date.now()
  return {
    deviceId: id,
    device_id: id,
    network: {
      wifi_up: true,
      state: 'STA_CONNECTED',
      ip: `10.99.${Math.floor(heartbeatNo / 255) % 255}.${(heartbeatNo % 253) + 1}`,
      rssi: -50,
      hostname: id,
      domain: 'local',
      fqdn: `${id}.local`,
      ota_address: `${id}.local:3232`,
      mdns: { enabled: true, services: ['_espdisp._tcp', '_arduino._tcp'] }
    },
    sk: { state: 'live' },
    signalk: { connected: true, state: 'live' },
    ui: {
      uptime_ms: heartbeatNo * 30000,
      screen: 'dashboard',
      theme: heartbeatNo % 2 ? 'night' : 'day',
      brightness: 0.8,
      layoutVariant: 'load',
      widgetVariant: 'load',
      widgetConfigHash: ''
    },
    display: {
      width: 480,
      height: 480,
      rotation: 0,
      brightness: 0.8,
      shape: 'square',
      density: 'mdpi',
      layoutClass: 'square',
      usableArea: { x: 0, y: 0, width: 480, height: 480 }
    },
    memory: {
      heap_free_kb: 64,
      internal_free_kb: 40,
      internal_largest_block_kb: 20,
      internal_min_free_kb: 32,
      psram_free_kb: 7000
    },
    firmware: {
      version: 'load-test',
      build_time: new Date(nowMs).toISOString(),
      git_commit: 'loadtest'
    },
    touch: { mode: 'normal', controller: 'gt911', interrupt: true },
    nmea0183Wifi: {
      enabled: false,
      mode: 'tcp',
      host: '',
      port: 10110,
      connected: false,
      bytesIn: 0,
      sentencesOk: 0,
      sentencesBad: 0,
      lastRxMs: 0
    },
    nmea2000: {
      compiledIn: false,
      enabled: false,
      hardwareCan: false,
      framesRx: 0,
      pgnsDecoded: 0,
      lastRxMs: 0
    },
    ota: {
      enabled: true,
      mode: 'arduino-ota',
      address: `${id}.local`,
      port: 3232,
      passwordSet: false,
      pullInFlight: false,
      pendingConfirm: false,
      policy: { enabled: true, requireSha256: true, maxSizeBytes: 0 }
    },
    webAuth: { enabled: true, username: 'espdisp', passwordSet: true },
    config: { version: 'load-test', hash: '', applied: true },
    errors: []
  }
}

async function registerDevices (opts, metrics) {
  const devices = []
  for (let i = 0; i < opts.devices; i++) {
    const identity = deviceIdentity(opts, i)
    await requestJson(opts, metrics, 'register', 'POST', '/plugins/espdisp-manager/devices/register', {
      device: identity
    })
    devices.push(identity.id)
    if ((i + 1) % 10 === 0 || i + 1 === opts.devices) {
      process.stdout.write(`\rregistered ${i + 1}/${opts.devices}`)
    }
  }
  process.stdout.write('\n')
  return devices
}

async function periodicLoop (opts, metrics, endAt, intervalMs, fn, initialJitterMs) {
  await sleep(initialJitterMs)
  while (performance.now() < endAt) {
    const started = performance.now()
    try {
      await fn()
    } catch (err) {
      // Metrics already counted the failed request. Keep the loop alive.
    }
    const elapsed = performance.now() - started
    await sleep(Math.max(0, intervalMs - elapsed))
  }
}

async function runDevice (opts, metrics, id, deviceIndex, endAt) {
  let heartbeats = 0
  const jitterBase = opts.devices > 1 ? (opts.jitterMs * deviceIndex) / (opts.devices - 1) : 0
  const heartbeat = periodicLoop(opts, metrics, endAt, opts.heartbeatMs, async () => {
    heartbeats++
    const result = await requestJson(
      opts,
      metrics,
      'heartbeat',
      'POST',
      `/plugins/espdisp-manager/devices/${encodeURIComponent(id)}/status`,
      statusBody(id, heartbeats)
    )
    if (opts.configEvery > 0 && heartbeats % opts.configEvery === 0) {
      await requestJson(opts, metrics, 'config', 'GET', `/plugins/espdisp-manager/devices/${encodeURIComponent(id)}/config`)
    } else if (result && result.desiredConfig && result.desiredConfig.reload) {
      await requestJson(opts, metrics, 'config', 'GET', `/plugins/espdisp-manager/devices/${encodeURIComponent(id)}/config`)
    }
  }, jitterBase)

  const commands = periodicLoop(opts, metrics, endAt, opts.commandPollMs, async () => {
    await requestJson(opts, metrics, 'commands', 'GET', `/plugins/espdisp-manager/devices/${encodeURIComponent(id)}/commands`)
  }, Math.min(opts.jitterMs, jitterBase + opts.commandPollMs / 2))

  await Promise.all([heartbeat, commands])
}

async function main () {
  const opts = parseArgs(process.argv)
  const metrics = new Metrics()

  console.log(`SignalK ESP Display Manager load test
target:       ${opts.url}
devices:      ${opts.devices}
duration:     ${opts.durationSec}s
heartbeat:    ${opts.heartbeatMs}ms
command poll: ${opts.commandPollMs}ms
config every: ${opts.configEvery || 'on drift only'}
`)

  await login(opts, metrics)
  const devices = await registerDevices(opts, metrics)
  const endAt = performance.now() + opts.durationSec * 1000
  await Promise.all(devices.map((id, index) => runDevice(opts, metrics, id, index, endAt)))
  metrics.print()
}

main().catch((err) => {
  console.error(`load test failed: ${err.message}`)
  process.exit(1)
})
