// Preset coverage contract.
//
// Asserts every supported display class produces a complete preset library
// (dashboard, wind, wind-steer, nav, depth, steering, route, trip, autopilot,
// system) and that each grid-type preset's tile count matches the display's
// grid budget. Wind and wind-steer are fullscreen HUDs (1 tile) on every class.

const assert = require('assert')
const presets = require('../lib/screen-presets')

const EXPECTED_IDS = [
  'dashboard', 'wind', 'wind-steer', 'nav', 'depth', 'steering',
  'route', 'trip', 'autopilot', 'system'
]

for (const [classId, meta] of Object.entries(presets.DISPLAY_CLASSES)) {
  const screens = presets.getPresetsForClass(classId)
  assert.strictEqual(screens.length, EXPECTED_IDS.length,
    `${classId}: expected ${EXPECTED_IDS.length} screens, got ${screens.length}`)

  const ids = screens.map((s) => s.id)
  assert.deepStrictEqual(ids, EXPECTED_IDS,
    `${classId}: screen ids must be in canonical order. Got ${ids.join(',')}`)

  for (const screen of screens) {
    assert.ok(Array.isArray(screen.tiles) && screen.tiles.length > 0,
      `${classId}/${screen.id}: tiles must be a non-empty array`)

    if (screen.type === 'fullscreen') {
      assert.strictEqual(screen.tiles.length, 1,
        `${classId}/${screen.id}: fullscreen presets are 1 logical tile`)
      continue
    }

    // Grid presets must fill the display's tile budget. (Allows fewer
    // tiles only if explicitly intended; today every grid preset fills
    // exactly tilesPerScreen.)
    assert.strictEqual(screen.tiles.length, meta.tilesPerScreen,
      `${classId}/${screen.id}: expected ${meta.tilesPerScreen} tiles ` +
        `(grid ${meta.gridCols}x${meta.gridRows}), got ${screen.tiles.length}`)

    // Every tile must reference a known widget type.
    for (const tile of screen.tiles) {
      assert.ok(tile.widget, `${classId}/${screen.id}: tile missing widget`)
      assert.ok(presets.WIDGET_TYPES[tile.widget],
        `${classId}/${screen.id}: unknown widget '${tile.widget}'`)
    }
  }
}

console.log('preset-coverage: %d classes × %d screens validated',
  Object.keys(presets.DISPLAY_CLASSES).length, EXPECTED_IDS.length)
