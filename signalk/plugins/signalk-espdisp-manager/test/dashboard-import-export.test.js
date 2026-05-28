const assert = require('assert')
const plugin = require('..')

const sample = {
  kind: 'espdisp.dashboard.v1',
  preset: {
    id: 'wide-day',
    name: 'Wide Day'
  },
  dashboard: {
    settings: {
      defaultScreen: 'dashboard',
      theme: 'day',
      brightness: 0.85
    },
    widgets: {
      defaults: {
        valueFontSize: 48
      },
      items: {
        sog: {
          type: 'numeric',
          title: 'SOG',
          path: 'navigation.speedOverGround'
        }
      }
    }
  }
}

const yaml = plugin._test.toYaml(sample)
assert.ok(yaml.includes('kind: espdisp.dashboard.v1'))
assert.ok(yaml.includes('theme: day'))
assert.ok(yaml.includes('valueFontSize: 48'))

const roundTrip = plugin._test.fromYaml(yaml)
assert.strictEqual(roundTrip.kind, sample.kind)
assert.strictEqual(roundTrip.preset.id, 'wide-day')
assert.strictEqual(roundTrip.dashboard.settings.theme, 'day')
assert.strictEqual(roundTrip.dashboard.widgets.defaults.valueFontSize, 48)
