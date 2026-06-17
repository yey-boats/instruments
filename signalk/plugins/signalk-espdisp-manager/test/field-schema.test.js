'use strict'

// Slice 5 — pure field-schema tests (node:test). Covers manifest gating:
// valid/invalid view type, font gating, unit-family gating, range/zones only on
// gauge/bar, zoom gating, color defaulting, the markers/reference reservation,
// coerceField normalization, and the SignalK meta -> {unit,range,zones} mapping.

const { test } = require('node:test')
const assert = require('node:assert')

const fs = require('../lib/field-schema')
const { DEFAULT_MANIFEST, validateField, coerceField, metaToFieldDefaults } = fs

// A curated manifest variant (paths restricted) used by the path-gating tests.
const CURATED = Object.assign({}, DEFAULT_MANIFEST, {
  paths: ['navigation.speedOverGround', 'environment.depth.belowTransducer']
})

test('valid numeric field passes', () => {
  const f = {
    type: 'numeric',
    title: 'SOG',
    paths: { value: 'navigation.speedOverGround' },
    size: 48,
    unit: 'kn'
  }
  const r = validateField(f, DEFAULT_MANIFEST)
  assert.strictEqual(r.ok, true, JSON.stringify(r.errors))
})

test('unknown view type is rejected', () => {
  const r = validateField({ type: 'hologram', paths: { value: 'x' } }, DEFAULT_MANIFEST)
  assert.strictEqual(r.ok, false)
  assert.ok(r.errors.some((e) => e.code === 'type_not_supported'))
})

test('missing required named path is rejected', () => {
  const r = validateField({ type: 'windCircle', paths: { value: 'environment.wind.speedTrue' } }, DEFAULT_MANIFEST)
  assert.strictEqual(r.ok, false)
  assert.ok(r.errors.some((e) => e.field === 'paths.dir' && e.code === 'path_required'))
})

test('windCircle with both named paths passes', () => {
  const r = validateField({
    type: 'windCircle',
    paths: { value: 'environment.wind.speedTrue', dir: 'environment.wind.directionTrue' }
  }, DEFAULT_MANIFEST)
  assert.strictEqual(r.ok, true, JSON.stringify(r.errors))
})

test('font size not in manifest.fontSizes is rejected', () => {
  const r = validateField({ type: 'numeric', paths: { value: 'x' }, size: 33 }, DEFAULT_MANIFEST)
  assert.strictEqual(r.ok, false)
  assert.ok(r.errors.some((e) => e.code === 'size_not_supported'))
})

test('unit must be in the bound path quantity family', () => {
  // speed path -> speed family {kn, m/s}; "deg" is an angle unit -> reject.
  const bad = validateField({ type: 'numeric', paths: { value: 'navigation.speedOverGround' }, unit: 'deg' }, DEFAULT_MANIFEST)
  assert.strictEqual(bad.ok, false)
  assert.ok(bad.errors.some((e) => e.code === 'unit_not_in_family'))
  // kn is in the speed family -> ok.
  const good = validateField({ type: 'numeric', paths: { value: 'navigation.speedOverGround' }, unit: 'kn' }, DEFAULT_MANIFEST)
  assert.strictEqual(good.ok, true, JSON.stringify(good.errors))
})

test('range/zones only on gauge|bar', () => {
  const onNumeric = validateField({
    type: 'numeric', paths: { value: 'x' }, range: { min: 0, max: 1 }
  }, DEFAULT_MANIFEST)
  assert.strictEqual(onNumeric.ok, false)
  assert.ok(onNumeric.errors.some((e) => e.code === 'range_not_allowed'))

  const zonesOnText = validateField({
    type: 'text', paths: { value: 'x' }, zones: [{ lower: 0, upper: 1 }]
  }, DEFAULT_MANIFEST)
  assert.strictEqual(zonesOnText.ok, false)
  assert.ok(zonesOnText.errors.some((e) => e.code === 'zones_not_allowed'))

  const onGauge = validateField({
    type: 'gauge', paths: { value: 'environment.depth.belowTransducer' },
    range: { min: 0, max: 50 }, zones: [{ lower: 0, upper: 2, state: 'alarm' }]
  }, DEFAULT_MANIFEST)
  assert.strictEqual(onGauge.ok, true, JSON.stringify(onGauge.errors))
})

test('inverted range is rejected', () => {
  const r = validateField({ type: 'bar', paths: { value: 'x' }, range: { min: 10, max: 5 } }, DEFAULT_MANIFEST)
  assert.strictEqual(r.ok, false)
  assert.ok(r.errors.some((e) => e.code === 'range_inverted'))
})

test('zoom gating: text has no zoom modes, numeric allows auto, compass allows screenRef', () => {
  const textZoom = validateField({ type: 'text', paths: { value: 'x' }, zoom: 'auto' }, DEFAULT_MANIFEST)
  assert.strictEqual(textZoom.ok, false)
  assert.ok(textZoom.errors.some((e) => e.code === 'zoom_not_allowed'))

  const numAuto = validateField({ type: 'numeric', paths: { value: 'x' }, zoom: 'auto' }, DEFAULT_MANIFEST)
  assert.strictEqual(numAuto.ok, true, JSON.stringify(numAuto.errors))

  // numeric does NOT list screenRef -> a screen-ref zoom is rejected.
  const numRef = validateField({ type: 'numeric', paths: { value: 'x' }, zoom: 'wind' }, DEFAULT_MANIFEST)
  assert.strictEqual(numRef.ok, false)
  assert.ok(numRef.errors.some((e) => e.code === 'zoom_not_allowed'))

  // compass lists screenRef -> a screen-ref zoom is accepted.
  const compRef = validateField({ type: 'compass', paths: { value: 'navigation.headingTrue' }, zoom: 'wind' }, DEFAULT_MANIFEST)
  assert.strictEqual(compRef.ok, true, JSON.stringify(compRef.errors))
})

test('color defaulting: unset elements omitted; bad hex rejected on validate', () => {
  // A field with no color at all is valid (unset => theme default).
  const noColor = validateField({ type: 'numeric', paths: { value: 'x' } }, DEFAULT_MANIFEST)
  assert.strictEqual(noColor.ok, true)

  // A bad hex is rejected.
  const bad = validateField({ type: 'numeric', paths: { value: 'x' }, color: { value: 'red' } }, DEFAULT_MANIFEST)
  assert.strictEqual(bad.ok, false)
  assert.ok(bad.errors.some((e) => e.code === 'color_bad_hex'))

  // coerce drops invalid + unknown-element colors; keeps valid known ones.
  const c = coerceField({ type: 'numeric', paths: { value: 'x' }, color: { value: '#4fc3f7', bogus: '#000000', label: 'notahex' } }, DEFAULT_MANIFEST)
  assert.deepStrictEqual(c.color, { value: '#4fc3f7' })
})

test('markers/reference reservation: passed through on compass, rejected elsewhere', () => {
  // Reserved fields present on a numeric type -> rejected (reservation guard).
  const onNumeric = validateField({
    type: 'numeric', paths: { value: 'x' }, reference: 'navigation.headingTrue', markers: [{ id: 'a' }]
  }, DEFAULT_MANIFEST)
  assert.strictEqual(onNumeric.ok, false)
  assert.ok(onNumeric.errors.some((e) => e.code === 'reference_not_allowed'))
  assert.ok(onNumeric.errors.some((e) => e.code === 'markers_not_allowed'))

  // Same fields on a compass -> allowed (pass-through, untouched).
  const onCompass = validateField({
    type: 'compass', paths: { value: 'navigation.headingTrue' },
    reference: 'navigation.headingTrue', markers: [{ id: 'cog', glyph: 'triangle', path: 'navigation.courseOverGroundTrue' }]
  }, DEFAULT_MANIFEST)
  assert.strictEqual(onCompass.ok, true, JSON.stringify(onCompass.errors))

  // coerce preserves the reserved fields verbatim on compass-like types.
  const coerced = coerceField({
    type: 'windCircle',
    paths: { value: 'environment.wind.speedTrue', dir: 'environment.wind.directionTrue' },
    reference: 'navigation.headingTrue',
    markers: [{ id: 'twa', glyph: 'diamond', filled: true, color: '#ffb300', path: 'environment.wind.angleTrueWater' }]
  }, DEFAULT_MANIFEST)
  assert.strictEqual(coerced.reference, 'navigation.headingTrue')
  assert.strictEqual(coerced.markers.length, 1)
  assert.strictEqual(coerced.markers[0].glyph, 'diamond')

  // coerce strips reserved fields on non-compass types.
  const stripped = coerceField({ type: 'numeric', paths: { value: 'x' }, reference: 'y', markers: [{ id: 'z' }] }, DEFAULT_MANIFEST)
  assert.strictEqual(stripped.reference, undefined)
  assert.strictEqual(stripped.markers, undefined)
})

test('curated path list gates bindings; open allows any', () => {
  const inList = validateField({ type: 'numeric', paths: { value: 'navigation.speedOverGround' } }, CURATED)
  assert.strictEqual(inList.ok, true, JSON.stringify(inList.errors))

  const notInList = validateField({ type: 'numeric', paths: { value: 'electrical.batteries.house.voltage' } }, CURATED)
  assert.strictEqual(notInList.ok, false)
  assert.ok(notInList.errors.some((e) => e.code === 'path_not_allowed'))

  // open manifest allows arbitrary paths.
  const open = validateField({ type: 'numeric', paths: { value: 'some.custom.path' } }, DEFAULT_MANIFEST)
  assert.strictEqual(open.ok, true, JSON.stringify(open.errors))
})

test('coerceField snaps size, drops out-of-family unit, defaults numeric zoomable', () => {
  const c = coerceField({
    type: 'numeric',
    paths: { value: 'navigation.speedOverGround' },
    size: 30, // not legal -> snaps to nearest (28 or 32)
    unit: 'deg' // wrong family for a speed path -> dropped
  }, DEFAULT_MANIFEST)
  assert.ok([28, 32].includes(c.size), 'size snapped to nearest legal: ' + c.size)
  assert.strictEqual(c.unit, undefined)
  assert.strictEqual(c.zoomable, true)
})

test('coerceField maps legacy single path -> paths.value and strips range on numeric', () => {
  const c = coerceField({ type: 'numeric', path: 'environment.depth.belowTransducer', range: { min: 0, max: 50 } }, DEFAULT_MANIFEST)
  assert.strictEqual(c.paths.value, 'environment.depth.belowTransducer')
  assert.strictEqual(c.range, undefined)
})

test('coerceField falls back to first manifest type for unknown type', () => {
  const c = coerceField({ type: 'nope', paths: { value: 'x' } }, DEFAULT_MANIFEST)
  assert.ok(DEFAULT_MANIFEST.viewTypes[c.type], 'coerced to a real type: ' + c.type)
})

test('metaToFieldDefaults maps units, displayScale, and zones', () => {
  const meta = {
    units: 'm/s',
    displayScale: { lower: 0, upper: 25 },
    zones: [
      { upper: 5, state: 'alarm' },
      { lower: 5, upper: 20, state: 'nominal' },
      { lower: 20, state: 'warn' }
    ]
  }
  const d = metaToFieldDefaults(meta)
  assert.strictEqual(d.unit, 'm/s')
  assert.deepStrictEqual(d.range, { min: 0, max: 25 })
  assert.strictEqual(d.zones.length, 3)
  assert.strictEqual(d.zones[0].upper, 5)
  assert.strictEqual(d.zones[0].state, 'alarm')
  assert.strictEqual(d.zones[0].color, '#ff5252')
})

test('metaToFieldDefaults maps rad -> deg and ignores bad displayScale', () => {
  const d = metaToFieldDefaults({ units: 'rad', displayScale: { lower: 10, upper: 5 } })
  assert.strictEqual(d.unit, 'deg')
  assert.strictEqual(d.range, undefined) // inverted scale ignored
})

test('metaToFieldDefaults returns empty on empty/garbage meta', () => {
  assert.deepStrictEqual(metaToFieldDefaults(null), {})
  assert.deepStrictEqual(metaToFieldDefaults({}), {})
  assert.deepStrictEqual(metaToFieldDefaults({ units: 'furlong' }), {})
})
