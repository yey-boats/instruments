require('./plugin.test')
require('./firmware-contract.test')
require('./auth-token.test')
require('./mock-firmware.test')
require('./display-widgets.test')
require('./dashboard.test')
require('./discovery.test')
require('./discovery-claim-e2e.test')
require('./mdns-discovery.test')
require('./webapp-metadata.test')
require('./app-dock-config.test')
require('./ui-config-widget.test')
require('./dashboard-import-export.test')
require('./dashboard-editor-form.test')
require('./widget-parity.test')
require('./preset-coverage.test')
require('./device-projections.test')
// proto-control runs sequentially (not in the Promise.all batch below): it
// loads the ESM @espdisp/proto lib and spins up mock HTTP targets, which would
// otherwise starve the event loop during the timing-sensitive UDP tests.
require('./proto-control.test')
  .then(() => Promise.all([
    require('./knob-contract.test'),
    require('./github-firmware.test'),
    require('./udp-discovery.test'),
    require('./device-udp-discovery.test'),
    require('./discovery-scan.test'),
    require('./signalk-register-device.test'),
    require('./live-device.test'),
    require('./device-resolution.test')
  ]))
  .then(() => {
    console.log('espdisp-manager test suite passed')
  })
  .catch((err) => {
    console.error(err)
    process.exit(1)
  })
