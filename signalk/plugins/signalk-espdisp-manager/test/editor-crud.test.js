'use strict'

// Slice 5 — manifest-gated layout-editor CRUD (manager methods backing the
// /editor/* routes). Exercises: manifest fallback vs device-reported,
// add/rename/reorder/delete screens (and the maxViews cap), add/update/remove
// fields (and the maxTilesPerScreen cap), manifest gating on field config,
// "save limits", orphan-item cleanup, and prototype-pollution guards.

const { test } = require('node:test')
const assert = require('node:assert')
const { makeManager } = require('./test-utils')

function freshManager () {
  const { manager, auth } = makeManager({ auth: { mode: 'dev-shared-token', devToken: 'test-token' } })
  const id = 'espdisp-cccccccccccc'
  manager.registerDevice({
    device: { id, name: 'Bench', role: 'display', board: 'sunton_4848s040', display: { width: 480, height: 480, shape: 'square' } }
  }, auth)
  // Give the device its own empty profile so we author from a clean slate.
  manager.upsertProfile({ id: 'bench', name: 'Bench', config: { layout: { version: 1, screens: [] }, widgets: { version: 1, items: {} } } })
  manager.assignProfile(id, { profileId: 'bench' })
  return { manager, auth, id }
}

// A small manifest a device might report; tighter caps than the default.
const REPORTED_MANIFEST = {
  viewTypes: {
    numeric: { paths: ['value'], attrs: ['title', 'format', 'size', 'unit', 'color'], zoom: ['auto'] },
    gauge: { paths: ['value'], attrs: ['title', 'size', 'unit', 'color', 'range', 'zones'], zoom: ['auto'] }
  },
  fontSizes: [16, 28, 48],
  units: { speed: ['kn', 'm/s'], depth: ['m', 'ft'] },
  maxViews: 2,
  maxTilesPerScreen: 2,
  paths: 'open',
  themes: ['day', 'night']
}

test('effectiveManifest falls back to default when device reports none', () => {
  const { manager, id } = freshManager()
  const m = manager.effectiveManifest(id)
  assert.ok(m.viewTypes.numeric, 'default manifest has numeric')
  assert.strictEqual(m.maxViews, 8)
})

test('effectiveManifest uses device-reported ui.capabilities', () => {
  const { manager, id, auth } = freshManager()
  manager.updateStatus(id, { time: new Date().toISOString(), ui: { capabilities: REPORTED_MANIFEST } }, auth)
  const m = manager.effectiveManifest(id)
  assert.strictEqual(m.maxViews, 2)
  assert.ok(!m.viewTypes.compass, 'reported manifest omits compass')
})

test('add / rename / reorder / delete screens with maxViews cap', () => {
  const { manager, id, auth } = freshManager()
  manager.updateStatus(id, { time: new Date().toISOString(), ui: { capabilities: REPORTED_MANIFEST } }, auth)

  let layout = manager.addScreen(id, { title: 'Dash' })
  assert.strictEqual(layout.screens.length, 1)
  const firstId = layout.screens[0].id
  assert.strictEqual(layout.screens[0].title, 'Dash')

  layout = manager.addScreen(id, { title: 'Nav' })
  assert.strictEqual(layout.screens.length, 2)

  // maxViews = 2 -> a third add is rejected.
  assert.throws(() => manager.addScreen(id, { title: 'Too many' }), /max.*view/i)

  // rename
  layout = manager.renameScreen(id, firstId, 'Dashboard')
  assert.strictEqual(layout.screens.find((s) => s.id === firstId).title, 'Dashboard')

  // reorder: put the second screen first
  const order = [layout.screens[1].id, layout.screens[0].id]
  layout = manager.reorderScreens(id, order)
  assert.deepStrictEqual(layout.screens.map((s) => s.id), order)

  // delete
  layout = manager.deleteScreen(id, order[0])
  assert.strictEqual(layout.screens.length, 1)
  assert.throws(() => manager.deleteScreen(id, 'nope'), /not_found|not found/i)
})

test('add / update / remove fields with maxTilesPerScreen cap + gating', () => {
  const { manager, id, auth } = freshManager()
  manager.updateStatus(id, { time: new Date().toISOString(), ui: { capabilities: REPORTED_MANIFEST } }, auth)
  let layout = manager.addScreen(id, { title: 'Dash' })
  const sid = layout.screens[0].id

  layout = manager.addField(id, sid, { type: 'numeric', paths: { value: 'navigation.speedOverGround' }, size: 48, unit: 'kn' })
  let screen = layout.screens.find((s) => s.id === sid)
  assert.strictEqual(screen.tiles.length, 1)
  const widgetId = screen.tiles[0].widget
  assert.ok(layout.items[widgetId], 'widget item created')
  assert.strictEqual(layout.items[widgetId].paths.value, 'navigation.speedOverGround')

  layout = manager.addField(id, sid, { type: 'numeric', paths: { value: 'environment.depth.belowTransducer' }, unit: 'm' })
  assert.strictEqual(layout.screens.find((s) => s.id === sid).tiles.length, 2)

  // maxTilesPerScreen = 2 -> third add rejected.
  assert.throws(() => manager.addField(id, sid, { type: 'numeric', paths: { value: 'x' } }), /max.*tile/i)

  // update gating: an out-of-family unit (deg on a speed path) is COERCED away
  // — the editor normalizes rather than erroring, so the persisted field drops
  // the illegal unit.
  layout = manager.updateField(id, sid, widgetId, { type: 'numeric', paths: { value: 'navigation.speedOverGround' }, unit: 'deg' })
  assert.strictEqual(layout.items[widgetId].unit, undefined, 'deg unit stripped on a speed path')

  // update gating: a font size not in the manifest is SNAPPED to the nearest
  // legal size (33 -> 28, the nearest of [16,28,48]).
  layout = manager.updateField(id, sid, widgetId, { type: 'numeric', paths: { value: 'navigation.speedOverGround' }, size: 33 })
  assert.strictEqual(layout.items[widgetId].size, 28, 'size 33 snapped to 28')

  // a non-coercible problem (missing required path) IS rejected.
  assert.throws(() => manager.updateField(id, sid, widgetId, { type: 'numeric', paths: {} }), /manifest|field_invalid|invalid|required/i)

  // valid update applies.
  layout = manager.updateField(id, sid, widgetId, { type: 'numeric', paths: { value: 'navigation.speedOverGround' }, size: 28, unit: 'm/s' })
  assert.strictEqual(layout.items[widgetId].size, 28)
  assert.strictEqual(layout.items[widgetId].unit, 'm/s')

  // remove the first field; its widget item is garbage-collected.
  layout = manager.removeField(id, sid, widgetId)
  assert.strictEqual(layout.screens.find((s) => s.id === sid).tiles.length, 1)
  assert.ok(!layout.items[widgetId], 'orphan widget item removed')
})

test('range/zones only persist on gauge/bar; save limits writes them', () => {
  const { manager, id, auth } = freshManager()
  manager.updateStatus(id, { time: new Date().toISOString(), ui: { capabilities: REPORTED_MANIFEST } }, auth)
  let layout = manager.addScreen(id, { title: 'Tanks' })
  const sid = layout.screens[0].id

  // numeric field: range is COERCED AWAY (not an error) — coerceField strips
  // range/zones on non-ranged types, so the persisted field has no range.
  layout = manager.addField(id, sid, { type: 'numeric', paths: { value: 'x' }, range: { min: 0, max: 1 } })
  const numWid = layout.screens.find((s) => s.id === sid).tiles[0].widget
  assert.strictEqual(layout.items[numWid].range, undefined, 'range stripped on numeric')
  // clear it so the next gauge add fits the maxTilesPerScreen=2 budget headroom.
  layout = manager.removeField(id, sid, numWid)

  // gauge field accepts range + zones.
  layout = manager.addField(id, sid, {
    type: 'gauge', paths: { value: 'environment.depth.belowTransducer' },
    range: { min: 0, max: 50 }, zones: [{ lower: 0, upper: 2, state: 'alarm', color: '#ff5252' }]
  })
  const wid = layout.screens.find((s) => s.id === sid).tiles[0].widget
  assert.deepStrictEqual(layout.items[wid].range, { min: 0, max: 50 })

  // save limits updates them.
  layout = manager.saveFieldLimits(id, sid, wid, { range: { min: 0, max: 80 }, zones: [{ lower: 0, upper: 5, state: 'alarm' }] })
  assert.deepStrictEqual(layout.items[wid].range, { min: 0, max: 80 })
  assert.strictEqual(layout.items[wid].zones.length, 1)
})

test('prototype-pollution guard on field id', () => {
  const { manager, id, auth } = freshManager()
  manager.updateStatus(id, { time: new Date().toISOString(), ui: { capabilities: REPORTED_MANIFEST } }, auth)
  const layout = manager.addScreen(id, { title: 'Dash' })
  const sid = layout.screens[0].id
  assert.throws(() => manager.updateField(id, sid, '__proto__', { type: 'numeric', paths: { value: 'x' } }), /invalid|not_found|not found/i)
  // Object.prototype was not polluted.
  assert.strictEqual({}.type, undefined)
})

test('CRUD queues a config.reload so the device fetches the new layout', () => {
  const { manager, id } = freshManager()
  const before = manager.store.commands.commands.length
  manager.addScreen(id, { title: 'Dash' })
  const after = manager.store.commands.commands.filter((c) => c.deviceId === id && c.type === 'config.reload')
  assert.ok(after.length >= 1, 'a config.reload was queued')
  assert.ok(manager.store.commands.commands.length > before)
})
