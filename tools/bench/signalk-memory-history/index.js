// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Yey Boats Project. See LICENSE and COMMERCIAL.md.
//
// Bench-only SignalK History API provider: keeps every numeric value of
// vessels.self in memory (ring per path, `retentionMinutes`) and answers
// GET /signalk/v2/api/history/values with time buckets of `resolution`
// seconds (average | min | max | first | last; others fall back to average).
//
// It exists to test trend-history backfill (instruments #101, Display Manager
// #82) without InfluxDB. Not for boats: memory only, lost on restart.
// Install: tools/bench/history_backfill_test.sh install.

'use strict'

module.exports = function (app) {
  const plugin = {
    id: 'yey-memory-history',
    name: 'YEY memory history (bench test provider)',
    description: 'In-memory SignalK History API provider for bench tests of trend backfill'
  }
  let unsubscribes = []
  const series = new Map() // path -> [[ms, value], ...] oldest first
  let retentionMs = 60 * 60 * 1000

  function record (path, value, ms) {
    if (typeof value !== 'number' || !Number.isFinite(value)) return
    let a = series.get(path)
    if (!a) { a = []; series.set(path, a) }
    a.push([ms, value])
    const cut = ms - retentionMs
    let drop = 0
    while (drop < a.length && a[drop][0] < cut) drop++
    if (drop) a.splice(0, drop)
  }

  function toMs (x) {
    if (x == null) return undefined
    if (typeof x === 'number') return x
    if (typeof x.epochMilliseconds === 'number') return x.epochMilliseconds
    const t = Date.parse(String(x))
    return Number.isNaN(t) ? undefined : t
  }

  function durationMs (d) {
    if (d == null) return undefined
    if (typeof d === 'number') return d * 1000
    if (typeof d.total === 'function') return d.total({ unit: 'milliseconds' })
    return undefined
  }

  // Resolve the request's [from, to] in epoch ms (TimeRangeParams variants).
  function range (q) {
    const now = Date.now()
    let from = toMs(q.from)
    let to = toMs(q.to)
    const dur = durationMs(q.duration)
    if (from === undefined && to === undefined) { to = now; from = now - (dur || 0) }
    else if (from !== undefined && to === undefined) to = dur !== undefined ? from + dur : now
    else if (from === undefined && to !== undefined) from = to - (dur || 0)
    return { from, to }
  }

  function aggregate (vals, method) {
    if (!vals.length) return null
    switch (method) {
      case 'min': return Math.min(...vals)
      case 'max': return Math.max(...vals)
      case 'first': return vals[0]
      case 'last': return vals[vals.length - 1]
      default: return vals.reduce((s, v) => s + v, 0) / vals.length
    }
  }

  const provider = {
    async getValues (q) {
      const { from, to } = range(q)
      const specs = q.pathSpecs || []
      const stepMs = Math.max(1000, Math.round((q.resolution || 60) * 1000))
      const first = Math.floor(from / stepMs)
      const last = Math.floor((to - 1) / stepMs)
      const rows = []
      for (let b = first; b <= last; b++) {
        const lo = b * stepMs
        const hi = lo + stepMs
        const row = [new Date(lo).toISOString()]
        let any = false
        for (const s of specs) {
          const vals = (series.get(s.path) || []).filter(([t]) => t >= lo && t < hi && t >= from && t < to).map(([, v]) => v)
          const v = aggregate(vals, s.aggregate)
          if (v !== null) any = true
          row.push(v)
        }
        if (any) rows.push(row)
      }
      return {
        context: q.context || 'vessels.self',
        range: { from: new Date(from).toISOString(), to: new Date(to).toISOString() },
        values: specs.map((s) => ({ path: s.path, method: s.aggregate })),
        data: rows
      }
    },
    async getPaths () { return [...series.keys()].sort() },
    async getContexts () { return ['vessels.self'] }
  }

  plugin.start = function (options) {
    retentionMs = Math.max(10, Number(options && options.retentionMinutes) || 60) * 60 * 1000
    app.subscriptionmanager.subscribe(
      { context: 'vessels.self', subscribe: [{ path: '*', period: 1000 }] },
      unsubscribes,
      (err) => app.error(`subscribe: ${err}`),
      (delta) => {
        for (const u of delta.updates || []) {
          const ms = toMs(u.timestamp) || Date.now()
          for (const pv of u.values || []) record(pv.path, pv.value, ms)
        }
      }
    )
    app.registerHistoryApiProvider(provider)
    app.setPluginStatus(`recording vessels.self, ${retentionMs / 60000} min kept`)
  }

  plugin.stop = function () {
    unsubscribes.forEach((f) => f())
    unsubscribes = []
    series.clear()
  }

  plugin.schema = {
    type: 'object',
    properties: {
      retentionMinutes: { type: 'number', title: 'Minutes of history kept in memory', default: 60 }
    }
  }

  plugin._test = { provider, record, series }
  return plugin
}
