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
    tilesPerScreen: 6, gridCols: 3, gridRows: 2
  },
  'waveshare-7-800x480': {
    label: 'Waveshare 7" — 800×480 wide',
    width: 800, height: 480, shape: 'wide',
    tilesPerScreen: 6, gridCols: 3, gridRows: 2
  },
  'waveshare-7b-1024x600': {
    label: 'Waveshare 7"B — 1024×600 wide',
    width: 1024, height: 600, shape: 'wide',
    tilesPerScreen: 6, gridCols: 3, gridRows: 2
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
  }
}

// ---- Preset builders ---------------------------------------------------

// Each builder returns one screen object: { id, type, title, tiles[] }.
// `widgetIdPrefix` lets us inject preset-scoped widget ids so two
// instances of the same preset on different screens don't collide.

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

function fullscreenWind () {
  return {
    id: 'wind',
    title: 'Wind',
    type: 'grid',
    tiles: [
      { widget: 'windRose', title: '',     primary: SK.awa, secondary: SK.aws },
      { widget: 'numeric',  title: 'AWS',  primary: SK.aws },
      { widget: 'numeric',  title: 'TWS',  primary: SK.tws },
      { widget: 'numeric',  title: 'TWA',  primary: SK.twa }
    ]
  }
}

function fullscreenNav () {
  return {
    id: 'nav',
    title: 'Nav',
    type: 'grid',
    tiles: [
      { widget: 'compass',  title: '',    primary: SK.heading, secondary: SK.cog },
      { widget: 'numeric',  title: 'SOG', primary: SK.sog },
      { widget: 'numeric',  title: 'COG', primary: SK.cog },
      { widget: 'text',     title: 'POS', primary: SK.position }
    ]
  }
}

function depthTempScreen () {
  return {
    id: 'depth',
    title: 'Depth',
    type: 'grid',
    tiles: [
      { widget: 'numeric', title: 'DEPTH',    primary: SK.depth },
      { widget: 'numeric', title: 'BELOW K',  primary: SK.depthKeel },
      { widget: 'numeric', title: 'H2O TEMP', primary: SK.waterTemp },
      { widget: 'trend',   title: 'DEPTH 5m', primary: SK.depth }
    ]
  }
}

function steeringScreen () {
  return {
    id: 'steering',
    title: 'Steering',
    type: 'grid',
    tiles: [
      { widget: 'compass', title: 'HDG / CTS', primary: SK.heading, secondary: SK.cts },
      { widget: 'numeric', title: 'XTE',       primary: SK.xte },
      { widget: 'numeric', title: 'VMG',       primary: SK.vmg },
      { widget: 'numeric', title: 'BTW',       primary: SK.btw }
    ]
  }
}

function routeScreen () {
  return {
    id: 'route',
    title: 'Route',
    type: 'grid',
    tiles: [
      { widget: 'numeric', title: 'DTW', primary: SK.dtw },
      { widget: 'numeric', title: 'BTW', primary: SK.btw },
      { widget: 'numeric', title: 'XTE', primary: SK.xte },
      { widget: 'numeric', title: 'VMG', primary: SK.vmg }
    ]
  }
}

function tripScreen () {
  return {
    id: 'trip',
    title: 'Trip',
    type: 'grid',
    tiles: [
      { widget: 'numeric', title: 'SOG',     primary: SK.sog },
      { widget: 'numeric', title: 'STW',     primary: SK.stw },
      { widget: 'numeric', title: 'CURRENT', primary: SK.currentDrift },
      { widget: 'numeric', title: 'SET',     primary: SK.currentSet }
    ]
  }
}

function autopilotScreen () {
  return {
    id: 'autopilot',
    title: 'Autopilot',
    type: 'grid',
    tiles: [
      { widget: 'autopilot', title: 'AP',     primary: SK.apState, secondary: SK.apTarget },
      { widget: 'compass',   title: 'COURSE', primary: SK.heading, secondary: SK.apTarget },
      { widget: 'numeric',   title: 'XTE',    primary: SK.xte },
      { widget: 'button',    title: 'STBY',   payload: { command: 'autopilot.standby' } }
    ]
  }
}

function systemScreen (displayClass) {
  // Wide displays get extra tiles to use the room; square shows 4.
  const tiles = [
    { widget: 'bar',     title: 'BATT SOC',   primary: SK.battSoc },
    { widget: 'numeric', title: 'BATT V',     primary: SK.battV },
    { widget: 'bar',     title: 'FUEL',       primary: SK.fuel },
    { widget: 'bar',     title: 'FRESH H2O',  primary: SK.freshwater }
  ]
  if (DISPLAY_CLASSES[displayClass] && DISPLAY_CLASSES[displayClass].tilesPerScreen === 6) {
    tiles.push({ widget: 'text', title: 'STATUS', primary: SK.apState })
    tiles.push({ widget: 'numeric', title: 'AIR TEMP', primary: SK.airTemp })
  }
  return { id: 'system', title: 'System', type: 'grid', tiles }
}

// ---- Per-class preset list ---------------------------------------------

function getPresetsForClass (displayClass) {
  const isWide = DISPLAY_CLASSES[displayClass] &&
                 DISPLAY_CLASSES[displayClass].tilesPerScreen === 6
  return [
    isWide ? dashboardWide() : dashboardQuad(),
    fullscreenWind(),
    fullscreenNav(),
    depthTempScreen(),
    steeringScreen(),
    routeScreen(),
    tripScreen(),
    autopilotScreen(),
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
