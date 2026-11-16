// node tools/bench/signalk-memory-history/test.js
'use strict'
const assert = require('assert')
const make = require('./index')

const app = { subscriptionmanager: { subscribe () {} }, registerHistoryApiProvider () {}, setPluginStatus () {}, error () {} }
const p = make(app)
p.start({ retentionMinutes: 60 })
const { provider, record } = p._test

const now = Date.now()
const t0 = Math.floor(now / 5000) * 5000 - 60000 // 12 buckets of 5 s back
for (let i = 0; i < 60; i++) record('environment.wind.speedTrue', 5 + (i % 5), t0 + i * 1000) // 1 Hz, 0..59 s
record('environment.wind.speedTrue', 'not a number', t0)

;(async () => {
  const q = (aggregate) => ({
    from: { epochMilliseconds: t0 }, to: { epochMilliseconds: t0 + 60000 }, resolution: 5,
    pathSpecs: [{ path: 'environment.wind.speedTrue', aggregate, parameter: [] }]
  })
  let r = await provider.getValues(q('average'))
  assert.strictEqual(r.data.length, 12, 'twelve 5 s buckets')
  assert.strictEqual(r.data[0][0], new Date(t0).toISOString())
  assert.strictEqual(r.data[0][1], (5 + 6 + 7 + 8 + 9) / 5)
  r = await provider.getValues(q('max'))
  assert.strictEqual(r.data[0][1], 9)
  r = await provider.getValues(q('min'))
  assert.strictEqual(r.data[0][1], 5)
  // duration only (Temporal.Duration-like or seconds) ends at now
  r = await provider.getValues({ duration: { total: () => 600000 }, resolution: 5, pathSpecs: [{ path: 'environment.wind.speedTrue', aggregate: 'average' }] })
  assert.ok(r.data.length >= 12 && r.data.length <= 14, `duration query found ${r.data.length} buckets`)
  // unknown path: no rows
  r = await provider.getValues({ duration: 600, resolution: 5, pathSpecs: [{ path: 'nope.x', aggregate: 'average' }] })
  assert.strictEqual(r.data.length, 0)
  console.log('signalk-memory-history: ok')
})().catch((e) => { console.error(e); process.exit(1) })
