'use strict'

// Curated catalogue of "sane defaults" screens for each supported
// display class. The shape mirrors what the firmware's layout
// fetcher consumes (`layout.screens[]` with `tiles[]`).
//
// Each preset answers: "give me a working screen for <board>". The
// operator picks one in the visual editor, the editor inserts it
// into the current profile, and the device renders it on next
// config-fetch.
//
// The set is intentionally small: too many presets makes the picker
// useless. Six per device class is enough to cover the common
// helmsman workflows (dashboard, wind, nav, depth, autopilot,
// system) and any board-specific layouts.
//
// Adding a preset:
//   1. Pick a stable id (kebab-case). Must match the firmware's
//      registered screen id if it's meant to replace a built-in.
//   2. Set the `displayClass` so it shows only on matching boards.
//      Use 'any' for screens that work on every supported display.
//   3. Pick `type: 'grid'` and provide 4 tiles for square panels;
//      use 'grid' with 6 tiles for wide panels (3x2).
//   4. Each tile binds a `widget` (catalog type) and a `metric`
//      (SignalK dotted path). Optional `secondary` for widgets
//      that show two values.

// ---- SignalK path catalogue used by tile bindings -----------------------
// Centralizing here lets the editor offer autocomplete from one source.

const SK = {
  // Navigation
  sog:           'navigation.speedOverGround',
  stw:           'navigation.speedThroughWater',
  cog:           'navigation.courseOverGroundTrue',
  heading:       'navigation.headingTrue',
  position:      'navigation.position',
  // Wind
  awa:           'environment.wind.angleApparent',
  aws:           'environment.wind.speedApparent',
  twa:           'environment.wind.angleTrueWater',
  tws:           'environment.wind.speedTrue',
  twd:           'environment.wind.directionTrue',
  beatAngle:     'performance.beatAngle',
  gybeAngle:     'performance.gybeAngle',
  // Environment
  depth:         'environment.depth.belowTransducer',
  depthKeel:     'environment.depth.belowKeel',
  waterTemp:     'environment.water.temperature',
  airTemp:       'environment.outside.temperature',
  // Electrical
  battV:         'electrical.batteries.house.voltage',
  battSoc:       'electrical.batteries.house.stateOfCharge',
  // Tanks
  fuel:          'tanks.fuel.0.currentLevel',
  freshwater:    'tanks.freshWater.0.currentLevel',
  // Routing / autopilot
  xte:           'navigation.courseRhumbline.crossTrackError',
  btw:           'navigation.courseRhumbline.nextPoint.bearingTrue',
  dtw:           'navigation.courseRhumbline.nextPoint.distance',
  cts:           'navigation.courseRhumbline.bearingTrackTrue',
  vmg:           'navigation.courseRhumbline.velocityMadeGood',
  apTarget:      'steering.autopilot.target.headingTrue',
  apState:       'steering.autopilot.state',
  // Current/tide
  currentSet:    'environment.current.setTrue',
  currentDrift:  'environment.current.drift'
}

const ALL_PATHS = Object.values(SK).sort()

// ---- Display class metadata --------------------------------------------

const DISPLAY_CLASSES = {
  'sunton-480': {
    label: 'Sunton 4848S040 — 480×480 square',
    width: 480, height: 480, shape: 'square',
    tilesPerScreen: 4, gridCols: 2, gridRows: 2
  },
  'waveshare-4_3-800x480': {
    label: 'Waveshare 4.3" — 800×480 wide',
    width: 800, height: 480, shape: 'wide',
    tilesPerScreen: 6, gridCols: 3, gridRows: 2
  },
  'waveshare-5-800x480': {
    label: 'Waveshare 5" — 800×480 wide',
    width: 800, height: 480, shape: 'wide',
    tilesPerScreen: 6, gridCols: 3, gridRows: 2
  },
  'waveshare-5-1024x600': {
    label: 'Waveshare 5" — 1024×600 wide',
    width: 1024, height: 600, shape: 'wide',
    tilesPerScreen: 8, gridCols: 4, gridRows: 2
  },
  'waveshare-7-800x480': {
    label: 'Waveshare 7" — 800×480 wide',
    width: 800, height: 480, shape: 'wide',
    tilesPerScreen: 6, gridCols: 3, gridRows: 2
  },
  'waveshare-7b-1024x600': {
    label: 'Waveshare 7"B — 1024×600 wide',
    width: 1024, height: 600, shape: 'wide',
    tilesPerScreen: 8, gridCols: 4, gridRows: 2
  }
}

const BOARD_TO_CLASS = {
  sunton_4848s040: 'sunton-480',
  waveshare_touch_lcd_4: 'sunton-480',          // also 480×480 square
  waveshare_touch_lcd_4_3: 'waveshare-4_3-800x480',
  waveshare_touch_lcd_4_3b: 'waveshare-4_3-800x480',
  waveshare_touch_lcd_5_800x480: 'waveshare-5-800x480',
  waveshare_touch_lcd_5_1024x600: 'waveshare-5-1024x600',
  waveshare_touch_lcd_7_800x480: 'waveshare-7-800x480',
  waveshare_touch_lcd_7b_1024x600: 'waveshare-7b-1024x600'
}

function classifyBoard (board) {
  if (!board) return 'sunton-480'
  return BOARD_TO_CLASS[board] || 'sunton-480'
}

// ---- Widget metadata used by the editor's field picker -----------------

const WIDGET_TYPES = {
  numeric: {
    label: 'Numeric value',
    description: 'Big digits for a single SignalK path',
    metrics: { primary: { required: true, label: 'Metric path' } }
  },
  text: {
    label: 'Text value',
    description: 'String field (autopilot state, position fix, …)',
    metrics: { primary: { required: true, label: 'Metric path' } }
  },
  gauge: {
    label: 'Analog gauge',
    description: 'Needle gauge with min/max range',
    metrics: { primary: { required: true, label: 'Metric path' } }
  },
  compass: {
    label: 'Compass card',
    description: 'Rotating bezel with heading marker',
    metrics: {
      primary: { required: true, label: 'Heading metric' },
      secondary: { required: false, label: 'Course-to-steer (optional)' }
    }
  },
  windRose: {
    label: 'Wind rose',
    description: 'Apparent + true wind on a rose',
    metrics: {
      primary: { required: true, label: 'Apparent angle' },
      secondary: { required: false, label: 'Apparent speed' }
    }
  },
  trend: {
    label: 'Trend chart',
    description: 'Sparkline over time for one metric',
    metrics: { primary: { required: true, label: 'Metric path' } }
  },
  bar: {
    label: 'Horizontal bar',
    description: 'For tanks, battery SOC, etc.',
    metrics: { primary: { required: true, label: 'Metric path (0..1)' } }
  },
  button: {
    label: 'Action button',
    description: 'Sends a manager command on tap',
    metrics: {}
  },
  autopilot: {
    label: 'Autopilot control',
    description: 'State + target heading + engage/standby buttons',
    metrics: {
      primary: { required: true, label: 'AP state' },
      secondary: { required: false, label: 'AP target heading' }
    }
  },
  autopilotHud: {
    label: 'Autopilot HUD (full-screen)',
    description: 'Semicircular heading compass + HDG + COG/SOG + XTE strip + '
      + "square data tiles. Matches the device's screen_autopilot.cpp render.",
    fullscreen: true,
    metrics: {
      heading: { required: true, label: 'Heading (rad)' },
      apTarget: { required: false, label: 'AP target heading (rad)' },
      cog: { required: false, label: 'Course over ground (rad)' },
      sog: { required: false, label: 'Speed over ground (m/s)' },
      xte: { required: false, label: 'Cross-track error (m)' }
    }
  },
  windDial: {
    label: 'Wind dial (full-screen)',
    description: 'Rotating bezel with cardinals, close-hauled arcs, '
      + 'apparent + true wind markers, tide vector. Matches the '
      + "device's screen_wind.cpp render.",
    fullscreen: true,
    metrics: {
      awa: { required: true,  label: 'Apparent wind angle (rad)' },
      aws: { required: true,  label: 'Apparent wind speed (m/s)' },
      twa: { required: false, label: 'True wind angle (rad)' },
      tws: { required: false, label: 'True wind speed (m/s)' },
      heading: { required: false, label: 'Heading (rad, rotates bezel)' },
      cog: { required: false, label: 'Course over ground (rad)' },
      currentSet: { required: false, label: 'Current set (rad, tide vector)' },
      currentDrift: { required: false, label: 'Current drift (m/s)' }
    }
  },
  windSteer: {
    label: 'Wind steering (full-screen)',
    description: 'Semicircular heading compass with a red no-go sector + green '
      + 'target laylines (from the SignalK polar beat/gybe angles) + amber wind '
      + "bug, HDG hero, TWA/TWD sub-line, XTE strip, AWS/AWA/TWS/TWA tiles. "
      + "Matches the device's screen_wind_steer.cpp render.",
    fullscreen: true,
    metrics: {
      heading: { required: true,  label: 'Heading (rad)' },
      twd: { required: false, label: 'True wind direction (rad)' },
      twa: { required: false, label: 'True wind angle (rad)' },
      tws: { required: false, label: 'True wind speed (m/s)' },
      awa: { required: false, label: 'Apparent wind angle (rad)' },
      aws: { required: false, label: 'Apparent wind speed (m/s)' },
      beatAngle: { required: false, label: 'Polar beat angle (rad)' },
      gybeAngle: { required: false, label: 'Polar gybe angle (rad)' },
      xte: { required: false, label: 'Cross-track error (m)' }
    }
  }
}

// ---- Preset builders ---------------------------------------------------

// Each builder returns one screen object: { id, type, title, tiles[] }.
// `widgetIdPrefix` lets us inject preset-scoped widget ids so two
// instances of the same preset on different screens don't collide.

// Three tile-count tiers cover the supported boards:
//   - quad:   480×480 sunton + waveshare-4 (2×2, 4 tiles)
//   - wide:   800×480 (3×2, 6 tiles)
//   - xwide:  1024×600 waveshare-5 + 7B (4×2, 8 tiles)
// `tileCount(displayClass)` returns the budget for the picked class.

function tileCount (displayClass) {
  const meta = DISPLAY_CLASSES[displayClass]
  return (meta && meta.tilesPerScreen) || 4
}

function dashboardQuad () {
  return {
    id: 'dashboard',
    title: 'Dashboard',
    type: 'grid',
    tiles: [
      { widget: 'windRose', title: 'WIND', primary: SK.awa, secondary: SK.aws },
      { widget: 'numeric',  title: 'SOG',  primary: SK.sog },
      { widget: 'numeric',  title: 'DEPTH',primary: SK.depth },
      { widget: 'numeric',  title: 'BATT', primary: SK.battV }
    ]
  }
}

function dashboardWide () {
  return {
    id: 'dashboard',
    title: 'Dashboard',
    type: 'grid',
    tiles: [
      { widget: 'windRose', title: 'WIND',   primary: SK.awa, secondary: SK.aws },
      { widget: 'compass',  title: 'COURSE', primary: SK.heading, secondary: SK.cog },
      { widget: 'numeric',  title: 'SOG',    primary: SK.sog },
      { widget: 'numeric',  title: 'DEPTH',  primary: SK.depth },
      { widget: 'numeric',  title: 'H2O',    primary: SK.waterTemp },
      { widget: 'bar',      title: 'BATT',   primary: SK.battSoc }
    ]
  }
}

function dashboardXwide () {
  // 4×2 grid for 1024×600 panels: dashboardWide() + 2 extras using the
  // additional column. Tank gauges + autopilot state fill the room
  // without overloading the helmsman's view.
  return {
    id: 'dashboard',
    title: 'Dashboard',
    type: 'grid',
    tiles: [
      { widget: 'windRose', title: 'WIND',    primary: SK.awa, secondary: SK.aws },
      { widget: 'compass',  title: 'COURSE',  primary: SK.heading, secondary: SK.cog },
      { widget: 'numeric',  title: 'SOG',     primary: SK.sog },
      { widget: 'numeric',  title: 'STW',     primary: SK.stw },
      { widget: 'numeric',  title: 'DEPTH',   primary: SK.depth },
      { widget: 'numeric',  title: 'H2O',     primary: SK.waterTemp },
      { widget: 'bar',      title: 'BATT',    primary: SK.battSoc },
      { widget: 'text',     title: 'AP',      primary: SK.apState }
    ]
  }
}

function fullscreenWind () {
  // Single fullscreen wind dial tile that matches the device's
  // src/ui/screen_wind.cpp rendering: rotating bezel with cardinals,
  // close-hauled red/green arcs, apparent + true wind markers, tide
  // vector. The dial is one logical tile because it fills the screen.
  return {
    id: 'wind',
    title: 'Wind',
    type: 'fullscreen',
    tiles: [{
      widget: 'windDial',
      title: '',
      awa: SK.awa,
      aws: SK.aws,
      twa: SK.twa,
      tws: SK.tws,
      heading: SK.heading,
      cog: SK.cog,
      currentSet: SK.currentSet,
      currentDrift: SK.currentDrift
    }]
  }
}

function windSteerScreen () {
  // Dedicated full-screen wind-steering HUD matching the device's
  // src/ui/screen_wind_steer.cpp: the autopilot-style semicircular compass with
  // a red no-go sector + green target laylines (from the polar beat/gybe
  // angles) + amber wind bug, plus AWS/AWA/TWS/TWA tiles. One fullscreen tile.
  return {
    id: 'wind-steer',
    title: 'Wind steer',
    type: 'fullscreen',
    tiles: [{
      widget: 'windSteer',
      title: '',
      heading: SK.heading,
      twd: SK.twd,
      twa: SK.twa,
      tws: SK.tws,
      awa: SK.awa,
      aws: SK.aws,
      beatAngle: SK.beatAngle,
      gybeAngle: SK.gybeAngle,
      xte: SK.xte
    }]
  }
}

function fullscreenNav (displayClass) {
  const tiles = [
    { widget: 'compass',  title: '',    primary: SK.heading, secondary: SK.cog },
    { widget: 'numeric',  title: 'SOG', primary: SK.sog },
    { widget: 'numeric',  title: 'COG', primary: SK.cog },
    { widget: 'text',     title: 'POS', primary: SK.position }
  ]
  const n = tileCount(displayClass)
  if (n >= 6) {
    tiles.push({ widget: 'numeric', title: 'STW', primary: SK.stw })
    tiles.push({ widget: 'numeric', title: 'XTE', primary: SK.xte })
  }
  if (n >= 8) {
    tiles.push({ widget: 'numeric', title: 'VMG', primary: SK.vmg })
    tiles.push({ widget: 'numeric', title: 'BTW', primary: SK.btw })
  }
  return { id: 'nav', title: 'Nav', type: 'grid', tiles }
}

function depthTempScreen (displayClass) {
  const tiles = [
    { widget: 'numeric', title: 'DEPTH',    primary: SK.depth },
    { widget: 'numeric', title: 'BELOW K',  primary: SK.depthKeel },
    { widget: 'numeric', title: 'H2O TEMP', primary: SK.waterTemp },
    { widget: 'trend',   title: 'DEPTH 5m', primary: SK.depth }
  ]
  const n = tileCount(displayClass)
  if (n >= 6) {
    tiles.push({ widget: 'numeric', title: 'AIR TEMP', primary: SK.airTemp })
    tiles.push({ widget: 'trend',   title: 'H2O 5m',   primary: SK.waterTemp })
  }
  if (n >= 8) {
    tiles.push({ widget: 'trend',   title: 'TEMP 5m',  primary: SK.airTemp })
    tiles.push({ widget: 'numeric', title: 'SOG',      primary: SK.sog })
  }
  return { id: 'depth', title: 'Depth', type: 'grid', tiles }
}

function steeringScreen (displayClass) {
  const tiles = [
    { widget: 'compass', title: 'HDG / CTS', primary: SK.heading, secondary: SK.cts },
    { widget: 'numeric', title: 'XTE',       primary: SK.xte },
    { widget: 'numeric', title: 'VMG',       primary: SK.vmg },
    { widget: 'numeric', title: 'BTW',       primary: SK.btw }
  ]
  const n = tileCount(displayClass)
  if (n >= 6) {
    tiles.push({ widget: 'numeric', title: 'COG', primary: SK.cog })
    tiles.push({ widget: 'numeric', title: 'DTW', primary: SK.dtw })
  }
  if (n >= 8) {
    tiles.push({ widget: 'numeric', title: 'SOG', primary: SK.sog })
    tiles.push({ widget: 'numeric', title: 'STW', primary: SK.stw })
  }
  return { id: 'steering', title: 'Steering', type: 'grid', tiles }
}

function routeScreen (displayClass) {
  const tiles = [
    { widget: 'numeric', title: 'DTW', primary: SK.dtw },
    { widget: 'numeric', title: 'BTW', primary: SK.btw },
    { widget: 'numeric', title: 'XTE', primary: SK.xte },
    { widget: 'numeric', title: 'VMG', primary: SK.vmg }
  ]
  const n = tileCount(displayClass)
  if (n >= 6) {
    tiles.push({ widget: 'numeric', title: 'CTS', primary: SK.cts })
    tiles.push({ widget: 'numeric', title: 'SOG', primary: SK.sog })
  }
  if (n >= 8) {
    tiles.push({ widget: 'numeric', title: 'COG', primary: SK.cog })
    tiles.push({ widget: 'text',    title: 'POS', primary: SK.position })
  }
  return { id: 'route', title: 'Route', type: 'grid', tiles }
}

function tripScreen (displayClass) {
  const tiles = [
    { widget: 'numeric', title: 'SOG',     primary: SK.sog },
    { widget: 'numeric', title: 'STW',     primary: SK.stw },
    { widget: 'numeric', title: 'CURRENT', primary: SK.currentDrift },
    { widget: 'numeric', title: 'SET',     primary: SK.currentSet }
  ]
  const n = tileCount(displayClass)
  if (n >= 6) {
    tiles.push({ widget: 'trend',   title: 'SOG 5m', primary: SK.sog })
    tiles.push({ widget: 'numeric', title: 'VMG',    primary: SK.vmg })
  }
  if (n >= 8) {
    tiles.push({ widget: 'numeric', title: 'COG',    primary: SK.cog })
    tiles.push({ widget: 'numeric', title: 'HDG',    primary: SK.heading })
  }
  return { id: 'trip', title: 'Trip', type: 'grid', tiles }
}

function autopilotScreen (displayClass) {
  // The device autopilot screen is a dedicated full-screen HUD (semicircular
  // compass + HDG + COG/SOG + XTE strip + square tiles), not a tile grid, so
  // the preset is a single fullscreen tile that previews that HUD.
  void displayClass
  return {
    id: 'autopilot',
    title: 'Autopilot',
    type: 'fullscreen',
    tiles: [{
      widget: 'autopilotHud',
      title: '',
      heading: SK.heading,
      apTarget: SK.apTarget,
      cog: SK.cog,
      sog: SK.sog,
      xte: SK.xte
    }]
  }
}

function systemScreen (displayClass) {
  const tiles = [
    { widget: 'bar',     title: 'BATT SOC',   primary: SK.battSoc },
    { widget: 'numeric', title: 'BATT V',     primary: SK.battV },
    { widget: 'bar',     title: 'FUEL',       primary: SK.fuel },
    { widget: 'bar',     title: 'FRESH H2O',  primary: SK.freshwater }
  ]
  const n = tileCount(displayClass)
  if (n >= 6) {
    tiles.push({ widget: 'text',    title: 'STATUS',   primary: SK.apState })
    tiles.push({ widget: 'numeric', title: 'AIR TEMP', primary: SK.airTemp })
  }
  if (n >= 8) {
    tiles.push({ widget: 'numeric', title: 'H2O TEMP', primary: SK.waterTemp })
    tiles.push({ widget: 'numeric', title: 'DEPTH',    primary: SK.depth })
  }
  return { id: 'system', title: 'System', type: 'grid', tiles }
}

// ---- Per-class preset list ---------------------------------------------

function getPresetsForClass (displayClass) {
  const n = tileCount(displayClass)
  let dash
  if (n >= 8) dash = dashboardXwide()
  else if (n >= 6) dash = dashboardWide()
  else dash = dashboardQuad()
  return [
    dash,
    fullscreenWind(),
    windSteerScreen(),
    fullscreenNav(displayClass),
    depthTempScreen(displayClass),
    steeringScreen(displayClass),
    routeScreen(displayClass),
    tripScreen(displayClass),
    autopilotScreen(displayClass),
    systemScreen(displayClass)
  ]
}

function listDisplayClasses () {
  return Object.keys(DISPLAY_CLASSES).map((id) => ({
    id,
    ...DISPLAY_CLASSES[id]
  }))
}

function listWidgetTypes () {
  return Object.keys(WIDGET_TYPES).map((id) => ({
    id,
    ...WIDGET_TYPES[id]
  }))
}

module.exports = {
  SK,
  ALL_PATHS,
  DISPLAY_CLASSES,
  WIDGET_TYPES,
  classifyBoard,
  getPresetsForClass,
  listDisplayClasses,
  listWidgetTypes
}
