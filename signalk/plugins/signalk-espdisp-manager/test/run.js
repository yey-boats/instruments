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
Promise.all([
  require('./github-firmware.test'),
  require('./udp-discovery.test'),
  require('./device-udp-discovery.test'),
  require('./discovery-scan.test'),
  require('./signalk-register-device.test'),
  require('./live-device.test')
])
  .then(() => {
    console.log('espdisp-manager test suite passed')
  })
  .catch((err) => {
    console.error(err)
    process.exit(1)
  })
