const assert = require('assert')
const plugin = require('..')

const overrides = plugin._test.configOverridesFromForm({
  defaultScreen: 'dashboard',
  theme: 'day',
  brightness: '0.75',
  fontSize: '18',
  labelFontSize: '12',
  valueFontSize: '36',
  unitFontSize: '14',
  widgetId: ['sog', 'awa', 'draft widget'],
  widgetTitle: ['SOG', 'AWA', 'Draft'],
  widgetType: ['numeric', 'numeric', 'bar'],
  widgetPath: [
    'navigation.speedOverGround',
    'environment.wind.angleApparent',
    'environment.depth.belowTransducer'
  ],
  widgetUnit: ['kn', 'deg', 'm'],
  widgetPrecision: ['1', '0', '1'],
  widgetValueFontSize: ['48', '', '28'],
  removeWidget: ['awa'],
  screenId: ['dashboard'],
  screenType: ['grid'],
  tileScreen: ['dashboard', 'dashboard', 'dashboard'],
  tileWidget: ['sog', 'awa', 'draft-widget'],
  tileCol: ['0', '1', '0'],
  tileRow: ['0', '0', '1']
})

assert.strictEqual(overrides.settings.defaultScreen, 'dashboard')
assert.strictEqual(overrides.settings.brightness, 0.75)
assert.strictEqual(overrides.widgets.defaults.valueFontSize, 36)
assert.strictEqual(overrides.widgets.items.sog.title, 'SOG')
assert.strictEqual(overrides.widgets.items.sog.valueFontSize, 48)
assert.strictEqual(overrides.widgets.items.sog.precision, 1)
assert.strictEqual(overrides.widgets.items.awa, undefined)
assert.strictEqual(overrides.widgets.items['draft-widget'].type, 'bar')
assert.strictEqual(overrides.layout.screens[0].id, 'dashboard')
assert.deepStrictEqual(overrides.layout.screens[0].tiles[0], {
  widget: 'sog',
  area: { col: 0, row: 0 }
})
assert.deepStrictEqual(overrides.layout.screens[0].tiles[2], {
  widget: 'draft-widget',
  area: { col: 0, row: 1 }
})
