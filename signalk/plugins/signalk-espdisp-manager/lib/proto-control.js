// proto-control.js — the espdisp control-protocol client for the SignalK
// manager plugin.
//
// This module is the plugin's migration onto the shared `@espdisp/proto`
// JS library (the same protocol the knob and the headless harness speak).
// It does NOT re-implement version negotiation, auth, or message validation:
// every outbound message is validated with `@espdisp/proto`'s ajv validators
// before it goes on the wire, every inbound ack is validated on the way back,
// version compatibility is decided by `versionCompatible`, and the optional
// shared key is checked with `authOk`.
//
// Two halves, mirroring the protocol's transport-agnostic design over HTTP
// (`GET/POST /api/p2p/*` against each target at `http://<addr>[:port]`):
//
//   Discovery — for each device with a known address, fetch its DeviceRecord
//               (`GET /api/p2p/device`), validate it, filter out any device
//               whose protocol major is incompatible, and surface the record
//               annotated with the device's pv/transports.
//
//   Control   — on-demand attach -> switch -> detach (matching the firmware
//               controller pattern: never hold an idle session). The plugin
//               attaches as controller `plugin:<id>` carrying a configurable
//               name + color (so the target's "controlled" frame shows the
//               plugin's color) and the optional shared key.
//
// `@espdisp/proto` is an ES module; this file is CommonJS (the rest of the
// plugin is). We load it once via dynamic import() — available on every Node
// the plugin supports (>=20.10) — and cache the promise.

let protoLibPromise = null
function loadProtoLib () {
  if (!protoLibPromise) {
    protoLibPromise = import('@espdisp/proto')
  }
  return protoLibPromise
}

const PROTO_VERSION = '1.0'
const DEFAULT_TTL_MS = 10000

// Normalise a base URL: accept "host", "host:port", "http://host[:port]".
function normalizeBase (addr, port) {
  let base = String(addr || '').trim()
  if (!base) return null
  if (!/^https?:\/\//i.test(base)) {
    base = 'http://' + base
  }
  // Append a port only when the caller passed one and the URL has none.
  if (port && !/:\d+($|\/)/.test(base.replace(/^https?:\/\//i, ''))) {
    base = base.replace(/\/+$/, '') + ':' + Number(port)
  }
  return base.replace(/\/+$/, '')
}

class ProtoControl {
  // opts:
  //   controllerId  — defaults to "plugin:espdisp-manager"
  //   name          — controller display name shown on the target frame
  //   color         — "#RRGGBB" frame color for this controller
  //   key           — optional shared key (omitted when falsy)
  //   fetchImpl     — injectable fetch (tests); defaults to global fetch
  //   timeoutMs     — per-request timeout
  //   debug         — optional logger
  constructor (opts) {
    opts = opts || {}
    this.controllerId = opts.controllerId || 'plugin:espdisp-manager'
    this.name = opts.name || 'SignalK Manager'
    this.color = opts.color || '#ff9800'
    this.key = opts.key || ''
    this.timeoutMs = Number(opts.timeoutMs || 4000)
    this.debug = typeof opts.debug === 'function' ? opts.debug : () => {}
    this._fetch = opts.fetchImpl ||
      ((...args) => globalThis.fetch(...args))
  }

  async _lib () {
    return loadProtoLib()
  }

  async _fetchJson (url, init) {
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), this.timeoutMs)
    try {
      const res = await this._fetch(url, { ...init, signal: controller.signal })
      const text = await res.text()
      let body = null
      if (text) {
        try { body = JSON.parse(text) } catch (e) { body = null }
      }
      return { ok: res.ok, status: res.status, body }
    } finally {
      clearTimeout(timer)
    }
  }

  // --- discovery ----------------------------------------------------------

  // Whether a device is reachable over the protocol's IP transport. A device
  // "speaks the protocol" when it advertises a protocol version (pv) and lists
  // the "ip" transport (the registry annotates these from the DeviceRecord).
  static speaksProtocol (device) {
    if (!device) return false
    const pv = device.pv || device.protocolVersion ||
      (device.proto && device.proto.pv)
    const transports = device.transports ||
      (device.proto && device.proto.transports) || []
    return Boolean(pv) && Array.isArray(transports) && transports.includes('ip')
  }

  // Fetch + validate a single device's DeviceRecord. Returns
  //   { ok, base, record, pv, transports, reason }
  // ok=false with a reason on transport error, invalid record, or an
  // incompatible protocol major (so callers can skip it).
  async describeDevice (device) {
    const base = normalizeBase(
      device.base || device.address || device.ip || device.host,
      device.port
    )
    if (!base) return { ok: false, reason: 'no_address' }
    const { validate, versionCompatible } = await this._lib()
    let res
    try {
      res = await this._fetchJson(`${base}/api/p2p/device`, { method: 'GET' })
    } catch (err) {
      return { ok: false, base, reason: `fetch_failed: ${err.message}` }
    }
    if (!res.ok || !res.body) {
      return { ok: false, base, reason: `http_${res.status}` }
    }
    if (!validate.DeviceRecord(res.body)) {
      return { ok: false, base, reason: 'invalid_device_record' }
    }
    const record = res.body
    if (!versionCompatible(record.v)) {
      return {
        ok: false,
        base,
        record,
        pv: record.v,
        reason: 'incompatible_version'
      }
    }
    return {
      ok: true,
      base,
      record,
      pv: record.v,
      transports: Array.isArray(record.transports) ? record.transports : []
    }
  }

  // Probe a list of {address/base[, port]} devices, keeping only protocol-
  // compatible ones. Returns an array of successful describeDevice results.
  async discover (devices) {
    const list = Array.isArray(devices) ? devices : []
    const out = []
    for (const device of list) {
      const desc = await this.describeDevice(device)
      if (desc.ok) {
        out.push(desc)
      } else {
        this.debug(`proto-control: skipping device ${device.deviceId || device.address || '?'}: ${desc.reason}`)
      }
    }
    return out
  }

  // --- control ------------------------------------------------------------

  _attachMessage () {
    const msg = {
      v: PROTO_VERSION,
      t: 'attach',
      controllerId: this.controllerId,
      name: this.name,
      color: this.color,
      ttlMs: DEFAULT_TTL_MS
    }
    if (this.key) msg.key = this.key
    return msg
  }

  // attach(device) -> { ok, sessionId, ttlMs, record, reason }
  async attach (device) {
    const base = normalizeBase(
      device.base || device.address || device.ip || device.host,
      device.port
    )
    if (!base) return { ok: false, reason: 'no_address' }
    const { validate } = await this._lib()

    const attach = this._attachMessage()
    if (!validate.Attach(attach)) {
      // Programming error (bad color/controllerId config) — never send.
      return { ok: false, base, reason: 'invalid_attach_message' }
    }
    let res
    try {
      res = await this._fetchJson(`${base}/api/p2p/attach`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(attach)
      })
    } catch (err) {
      return { ok: false, base, reason: `fetch_failed: ${err.message}` }
    }
    if (!res.ok || !res.body) return { ok: false, base, reason: `http_${res.status}` }
    if (!validate.AttachAck(res.body)) {
      return { ok: false, base, reason: 'invalid_attach_ack' }
    }
    if (!res.body.accepted) {
      return { ok: false, base, reason: res.body.reason || 'attach_rejected' }
    }
    return {
      ok: true,
      base,
      sessionId: res.body.sessionId,
      ttlMs: res.body.ttlMs || DEFAULT_TTL_MS,
      record: res.body.device || null
    }
  }

  // switchView(base, sessionId, viewId) -> { ok, currentView, reason }
  async switchView (base, sessionId, viewId) {
    const { validate } = await this._lib()
    const msg = { v: PROTO_VERSION, t: 'switch', sessionId, viewId }
    if (!validate.Switch(msg)) {
      return { ok: false, reason: 'invalid_switch_message' }
    }
    let res
    try {
      res = await this._fetchJson(`${base}/api/p2p/switch`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(msg)
      })
    } catch (err) {
      return { ok: false, reason: `fetch_failed: ${err.message}` }
    }
    if (!res.ok || !res.body) return { ok: false, reason: `http_${res.status}` }
    if (!validate.SwitchAck(res.body)) {
      return { ok: false, reason: 'invalid_switch_ack' }
    }
    if (!res.body.ok) return { ok: false, reason: res.body.reason || 'switch_rejected' }
    return { ok: true, currentView: res.body.currentView || viewId }
  }

  // detach(base, sessionId) -> { ok, reason }
  async detach (base, sessionId) {
    const { validate } = await this._lib()
    const msg = { v: PROTO_VERSION, t: 'detach', sessionId }
    if (!validate.Detach(msg)) {
      return { ok: false, reason: 'invalid_detach_message' }
    }
    try {
      const res = await this._fetchJson(`${base}/api/p2p/detach`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(msg)
      })
      return { ok: res.ok, reason: res.ok ? null : `http_${res.status}` }
    } catch (err) {
      return { ok: false, reason: `fetch_failed: ${err.message}` }
    }
  }

  // setScreen(device, viewId) — the on-demand convenience used to reframe the
  // manager's `screen.set` command: attach -> switch -> detach, carrying the
  // plugin's color so the target frame flashes the plugin's color while the
  // switch is applied. Best-effort detach (we don't fail the call if only the
  // teardown fails, since the view was already switched).
  async setScreen (device, viewId) {
    const att = await this.attach(device)
    if (!att.ok) {
      return { ok: false, stage: 'attach', reason: att.reason, base: att.base }
    }
    const sw = await this.switchView(att.base, att.sessionId, viewId)
    // Always try to release the session, even if the switch failed.
    const det = await this.detach(att.base, att.sessionId)
    if (!sw.ok) {
      return { ok: false, stage: 'switch', reason: sw.reason, base: att.base, sessionId: att.sessionId }
    }
    return {
      ok: true,
      base: att.base,
      sessionId: att.sessionId,
      currentView: sw.currentView,
      detached: det.ok
    }
  }
}

module.exports = { ProtoControl, normalizeBase, PROTO_VERSION }
