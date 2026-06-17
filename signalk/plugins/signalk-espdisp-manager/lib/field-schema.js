'use strict'

// Slice 5 — manifest-gated field schema for the layout editor.
//
// A "field" (a.k.a. tile) is the typed tuple authored by the editor and pushed
// to the device. This module is the PURE, node-tested core: it defines the
// field shape, a sane built-in default manifest (used when a device hasn't
// reported `ui.capabilities` yet), and two pure functions:
//
//   validateField(field, manifest)  -> { ok, errors:[...], field }
//   coerceField(field, manifest)    -> a normalized, manifest-legal copy
//
// Gating rules (all enforced against the capability manifest):
//   * view `type`  ∈ manifest.viewTypes
//   * required named `paths` per type are present (numeric:{value},
//     windCircle:{value,dir}, …)
//   * `size` ∈ manifest.fontSizes
//   * `unit` ∈ the unit family for the bound value path's physical quantity
//   * `range`/`zones` only on gauge/bar
//   * `zoom` ∈ the type's allowed zoom modes
//   * colors default to the active theme when unset (an unconfigured field
//     looks exactly like today)
//
// Reserved (future compass marker-rings slice, NOT built here): a `reference`
// field and a `markers[]` array are passed through untouched on compass/
// windCircle, and validated to be absent/empty on every other type. We do not
// build marker UI in this slice.

// ---------------------------------------------------------------------------
// Built-in default manifest (matches the spec's example). Used as a fallback
// when the connected device reports no `ui.capabilities` (offline / pre-Slice-2
// firmware), so the editor still works.
const DEFAULT_MANIFEST = {
  viewTypes: {
    numeric: { paths: ['value'], attrs: ['title', 'format', 'size', 'unit', 'color'], zoom: ['auto'] },
    compass: { paths: ['value'], markers: true, attrs: ['title', 'size', 'color', 'reference'], zoom: ['auto', 'screenRef'] },
    windCircle: { paths: ['value', 'dir'], markers: true, attrs: ['title', 'format', 'size', 'unit', 'color', 'reference'], zoom: ['auto', 'screenRef'] },
    gauge: { paths: ['value'], attrs: ['title', 'size', 'unit', 'color', 'range', 'zones'], zoom: ['auto'] },
    bar: { paths: ['value'], attrs: ['title', 'size', 'unit', 'color', 'range', 'zones'], zoom: ['auto'] },
    trend: { paths: ['value'], attrs: ['title', 'size', 'unit', 'color'], zoom: ['auto'] },
    text: { paths: ['value'], attrs: ['title', 'size', 'color'], zoom: [] },
    control: { paths: ['value'], attrs: ['title', 'size', 'color'], controls: ['autopilot'], zoom: ['screenRef'] }
  },
  fontSizes: [12, 14, 16, 20, 28, 32, 38, 48, 64],
  units: {
    speed: ['kn', 'm/s'],
    angle: ['deg'],
    depth: ['m', 'ft'],
    temp: ['C', 'F'],
    ratio: ['%'],
    voltage: ['V']
  },
  maxViews: 8,
  maxTilesPerScreen: 4,
  maxMarkersPerDial: 12,
  glyphs: ['triangle', 'diamond', 'circle', 'bar', 'cross', 'chevron_in', 'chevron_out', 'chevron_left', 'chevron_right', 'chevron_double'],
  paths: 'open',
  controls: ['autopilot'],
  themes: ['day', 'night', 'high-contrast']
}

// The view types that may carry compass-marker rings + a rotation reference.
// (Marker UI is the follow-on slice; here we only reserve the fields.)
const COMPASS_LIKE = ['compass', 'windCircle']

// Types that may carry range/zones (gauge-style scalar widgets).
const RANGED_TYPES = ['gauge', 'bar']

// Physical-quantity inference from a SignalK path. Mirrors the heuristic the
// live-preview uses, but maps to manifest unit-FAMILY keys (not unit symbols)
// so we can gate `unit` to the right family. Returns a family key present in
// `manifest.units`, or null when we can't classify the path.
function quantityForPath (path) {
  const p = String(path || '')
  if (!p) return null
  if (/speed|drift|velocity|sog|stw|tws|aws/i.test(p)) return 'speed'
  if (/angle|heading|course|bearing|direction|setTrue|cog|track/i.test(p)) return 'angle'
  if (/depth/i.test(p)) return 'depth'
  if (/temperature|temp/i.test(p)) return 'temp'
  if (/stateOfCharge|currentLevel|relativeHumidity|ratio/i.test(p)) return 'ratio'
  if (/voltage/i.test(p)) return 'voltage'
  return null
}

function resolveManifest (manifest) {
  if (!manifest || typeof manifest !== 'object' || !manifest.viewTypes) {
    return DEFAULT_MANIFEST
  }
  return manifest
}

function typeSpec (manifest, type) {
  const m = resolveManifest(manifest)
  return (m.viewTypes && m.viewTypes[type]) || null
}

// Allowed unit symbols for a bound value path under this manifest.
// Empty array = no inferable family (operator may set anything / nothing).
function allowedUnitsForPath (manifest, path) {
  const m = resolveManifest(manifest)
  const fam = quantityForPath(path)
  if (!fam) return []
  const units = (m.units && m.units[fam]) || []
  return Array.isArray(units) ? units.slice() : []
}

// Default (unset) color for a field element. Unset ⇒ theme default, which we
// represent as `null` so the firmware/preview falls back to the active theme.
// We never bake a hex in here — that would freeze the field to one theme.
function isHexColor (v) {
  return typeof v === 'string' && /^#[0-9a-fA-F]{6}$/.test(v)
}

// The element-color keys each view type understands. Unset elements fall back
// to the theme; we keep the map small + per-type so the editor only offers the
// relevant swatches.
const COLOR_ELEMENTS = {
  numeric: ['value', 'label'],
  text: ['value', 'label'],
  trend: ['value', 'label', 'line'],
  gauge: ['value', 'label', 'needle', 'arc'],
  bar: ['value', 'label', 'fill'],
  compass: ['value', 'label', 'needle'],
  windCircle: ['value', 'label', 'needle', 'dir'],
  control: ['value', 'label']
}

function colorElementsFor (type) {
  return COLOR_ELEMENTS[type] || ['value', 'label']
}

// ---------------------------------------------------------------------------
// validateField — pure. Returns { ok, errors, field }. Never throws on bad
// input shape; collects every problem so the editor can surface them at once.
function validateField (field, manifest) {
  const errors = []
  const m = resolveManifest(manifest)
  const f = field && typeof field === 'object' ? field : {}

  // --- type gating -------------------------------------------------------
  const spec = typeSpec(m, f.type)
  if (!f.type || !spec) {
    errors.push({ field: 'type', code: 'type_not_supported', message: `view type "${f.type}" is not in the device manifest` })
    // Without a known type we can't gate the rest meaningfully.
    return { ok: false, errors, field: f }
  }

  // --- required named paths ---------------------------------------------
  const required = Array.isArray(spec.paths) ? spec.paths : ['value']
  const paths = f.paths && typeof f.paths === 'object' ? f.paths : {}
  for (const key of required) {
    const bound = paths[key]
    if (typeof bound !== 'string' || !bound.trim()) {
      errors.push({ field: `paths.${key}`, code: 'path_required', message: `${f.type} requires a "${key}" path binding` })
    }
  }
  // Path-catalogue gating: when the manifest curates paths (array), every
  // bound path must be in it. "open" (the default) allows any path.
  if (Array.isArray(m.paths)) {
    const allowed = new Set(m.paths)
    for (const key of Object.keys(paths)) {
      const bound = paths[key]
      if (typeof bound === 'string' && bound.trim() && !allowed.has(bound)) {
        errors.push({ field: `paths.${key}`, code: 'path_not_allowed', message: `path "${bound}" is not in the device's curated path list` })
      }
    }
  }

  // --- font size gating --------------------------------------------------
  if (f.size != null) {
    const sizes = Array.isArray(m.fontSizes) ? m.fontSizes : []
    if (!sizes.includes(f.size)) {
      errors.push({ field: 'size', code: 'size_not_supported', message: `font size ${f.size} is not in manifest.fontSizes` })
    }
  }

  // --- unit gating (against the value path's quantity family) -----------
  if (f.unit != null && f.unit !== '') {
    const allowed = allowedUnitsForPath(m, paths.value)
    // If we can't infer a family (allowed empty), we don't block — the path
    // may be a custom/string path with no canonical unit family.
    if (allowed.length && !allowed.includes(f.unit)) {
      errors.push({ field: 'unit', code: 'unit_not_in_family', message: `unit "${f.unit}" is not valid for this path's quantity (allowed: ${allowed.join(', ')})` })
    }
  }

  // --- range / zones only on gauge|bar ----------------------------------
  const ranged = RANGED_TYPES.includes(f.type)
  if (f.range != null && !ranged) {
    errors.push({ field: 'range', code: 'range_not_allowed', message: `range is only valid for ${RANGED_TYPES.join('/')} fields` })
  }
  if (f.range != null && ranged) {
    const r = f.range
    if (typeof r !== 'object' || typeof r.min !== 'number' || typeof r.max !== 'number') {
      errors.push({ field: 'range', code: 'range_invalid', message: 'range must be { min:Number, max:Number }' })
    } else if (r.min >= r.max) {
      errors.push({ field: 'range', code: 'range_inverted', message: 'range.min must be < range.max' })
    }
  }
  if (f.zones != null) {
    if (!ranged) {
      errors.push({ field: 'zones', code: 'zones_not_allowed', message: `zones are only valid for ${RANGED_TYPES.join('/')} fields` })
    } else if (!Array.isArray(f.zones)) {
      errors.push({ field: 'zones', code: 'zones_invalid', message: 'zones must be an array' })
    } else {
      f.zones.forEach((z, i) => {
        if (!z || typeof z !== 'object' || typeof z.lower !== 'number' || typeof z.upper !== 'number') {
          errors.push({ field: `zones[${i}]`, code: 'zone_invalid', message: 'each zone needs numeric lower/upper' })
        }
      })
    }
  }

  // --- color elements ----------------------------------------------------
  if (f.color != null) {
    if (typeof f.color !== 'object' || Array.isArray(f.color)) {
      errors.push({ field: 'color', code: 'color_invalid', message: 'color must be a map of element -> hex' })
    } else {
      for (const key of Object.keys(f.color)) {
        const v = f.color[key]
        if (v != null && !isHexColor(v)) {
          errors.push({ field: `color.${key}`, code: 'color_bad_hex', message: `color.${key} must be a #rrggbb hex or unset` })
        }
      }
    }
  }

  // --- zoom gating -------------------------------------------------------
  if (f.zoom != null && f.zoom !== '') {
    const modes = Array.isArray(spec.zoom) ? spec.zoom : []
    // "auto" is a literal mode; anything else is a screenRef and is only legal
    // if the type lists "screenRef".
    const isAuto = f.zoom === 'auto'
    if (isAuto && !modes.includes('auto')) {
      errors.push({ field: 'zoom', code: 'zoom_not_allowed', message: `${f.type} does not support zoom "auto"` })
    } else if (!isAuto && !modes.includes('screenRef')) {
      errors.push({ field: 'zoom', code: 'zoom_not_allowed', message: `${f.type} does not support a screen-reference zoom` })
    }
  }

  // --- reserved marker/reference fields (future slice) -------------------
  // Compass-like types may carry them (pass-through); every other type must
  // not. We don't validate their CONTENTS here (that's the marker slice) —
  // only the reservation: absent/empty on non-compass types.
  const compassLike = COMPASS_LIKE.includes(f.type)
  if (!compassLike) {
    if (f.reference != null && f.reference !== '') {
      errors.push({ field: 'reference', code: 'reference_not_allowed', message: `reference is only valid for ${COMPASS_LIKE.join('/')} fields` })
    }
    if (f.markers != null && !(Array.isArray(f.markers) && f.markers.length === 0)) {
      errors.push({ field: 'markers', code: 'markers_not_allowed', message: `markers are only valid for ${COMPASS_LIKE.join('/')} fields` })
    }
  }

  return { ok: errors.length === 0, errors, field: f }
}

// ---------------------------------------------------------------------------
// coerceField — pure. Returns a normalized, manifest-legal COPY of the field:
//   * unknown type -> first manifest viewType (so the editor has something live)
//   * missing paths object -> {}
//   * size snapped to nearest legal fontSize
//   * unit dropped if not in the path's family
//   * range/zones stripped on non-ranged types
//   * zoom dropped if not an allowed mode
//   * colors: invalid/blank elements removed (unset ⇒ theme default)
//   * reference/markers stripped on non-compass types; markers defaulted to []
//     on compass-like types (reserved, untouched otherwise)
// Never mutates the input.
function coerceField (field, manifest) {
  const m = resolveManifest(manifest)
  const src = field && typeof field === 'object' ? field : {}
  const out = {}

  // type
  let type = src.type
  if (!typeSpec(m, type)) {
    type = Object.keys(m.viewTypes || {})[0] || 'numeric'
  }
  out.type = type
  const spec = typeSpec(m, type) || {}

  // title
  if (typeof src.title === 'string') out.title = src.title

  // paths (keep only string bindings; keep all keys, the editor manages which)
  out.paths = {}
  const srcPaths = src.paths && typeof src.paths === 'object' ? src.paths : {}
  for (const key of Object.keys(srcPaths)) {
    const v = srcPaths[key]
    if (typeof v === 'string') out.paths[key] = v.trim()
  }
  // Back-compat: a legacy single `path` maps to paths.value.
  if (!out.paths.value && typeof src.path === 'string' && src.path.trim()) {
    out.paths.value = src.path.trim()
  }

  // format (free string mask)
  if (typeof src.format === 'string' && src.format) out.format = src.format

  // size: snap to nearest legal fontSize
  const sizes = Array.isArray(m.fontSizes) ? m.fontSizes.slice().sort((a, b) => a - b) : []
  if (src.size != null && sizes.length) {
    if (sizes.includes(src.size)) {
      out.size = src.size
    } else {
      out.size = sizes.reduce((best, s) => (Math.abs(s - src.size) < Math.abs(best - src.size) ? s : best), sizes[0])
    }
  }

  // unit: keep only if in the value path's family
  if (src.unit != null && src.unit !== '') {
    const allowed = allowedUnitsForPath(m, out.paths.value)
    if (!allowed.length || allowed.includes(src.unit)) out.unit = src.unit
  }

  // range / zones only on ranged types
  if (RANGED_TYPES.includes(type)) {
    if (src.range && typeof src.range === 'object' &&
        typeof src.range.min === 'number' && typeof src.range.max === 'number' &&
        src.range.min < src.range.max) {
      out.range = { min: src.range.min, max: src.range.max }
    }
    if (Array.isArray(src.zones)) {
      out.zones = src.zones
        .filter((z) => z && typeof z === 'object' && typeof z.lower === 'number' && typeof z.upper === 'number')
        .map((z) => {
          const zone = { lower: z.lower, upper: z.upper }
          if (typeof z.state === 'string') zone.state = z.state
          if (isHexColor(z.color)) zone.color = z.color
          return zone
        })
    }
  }

  // colors: drop invalid/blank elements; unset ⇒ theme default (omit key)
  if (src.color && typeof src.color === 'object' && !Array.isArray(src.color)) {
    const elements = new Set(colorElementsFor(type))
    const color = {}
    for (const key of Object.keys(src.color)) {
      if (!elements.has(key)) continue
      if (isHexColor(src.color[key])) color[key] = src.color[key]
    }
    if (Object.keys(color).length) out.color = color
  }

  // zoomable: numeric defaults true; keep explicit booleans
  if (typeof src.zoomable === 'boolean') {
    out.zoomable = src.zoomable
  } else if (type === 'numeric') {
    out.zoomable = true
  }

  // zoom: keep only if the type supports the mode
  if (src.zoom != null && src.zoom !== '') {
    const modes = Array.isArray(spec.zoom) ? spec.zoom : []
    const isAuto = src.zoom === 'auto'
    if ((isAuto && modes.includes('auto')) || (!isAuto && modes.includes('screenRef'))) {
      out.zoom = src.zoom
    }
  }

  // reserved compass marker fields — pass through untouched on compass-like
  // types; strip on everything else.
  if (COMPASS_LIKE.includes(type)) {
    if (typeof src.reference === 'string' && src.reference) out.reference = src.reference
    out.markers = Array.isArray(src.markers) ? src.markers : []
  }

  return out
}

// ---------------------------------------------------------------------------
// SignalK meta → field defaults. Pure mapping used by the SK-metadata prefill
// flow. Given a SignalK path `meta` object, derive { unit, range, zones }:
//   meta.units                  -> unit (mapped to a manifest unit symbol)
//   meta.displayScale.lower/upper -> range {min,max}
//   meta.zones[]                -> zones [{lower,upper,state,color}]
// Returns only the keys it could derive. Pure, node-tested.
//
// SignalK base units are SI (m/s, rad, K, ratio, V…). The device's display
// unit family is coarser; map the common SI units to the manifest symbols.
const SK_UNIT_TO_DISPLAY = {
  'm/s': 'm/s',
  rad: 'deg',
  K: 'C',
  m: 'm',
  V: 'V',
  ratio: '%',
  '%': '%'
}

const ZONE_STATE_COLORS = {
  nominal: '#4caf50',
  normal: '#4caf50',
  alert: '#ffb300',
  warn: '#ff9800',
  alarm: '#ff5252',
  emergency: '#d50000'
}

function metaToFieldDefaults (meta) {
  const out = {}
  if (!meta || typeof meta !== 'object') return out

  if (typeof meta.units === 'string' && meta.units) {
    const mapped = SK_UNIT_TO_DISPLAY[meta.units]
    if (mapped) out.unit = mapped
  }

  const ds = meta.displayScale
  if (ds && typeof ds === 'object' &&
      typeof ds.lower === 'number' && typeof ds.upper === 'number' &&
      ds.lower < ds.upper) {
    out.range = { min: ds.lower, max: ds.upper }
  }

  if (Array.isArray(meta.zones) && meta.zones.length) {
    const zones = meta.zones
      .filter((z) => z && typeof z === 'object' &&
        (typeof z.lower === 'number' || typeof z.upper === 'number'))
      .map((z) => {
        const zone = {}
        if (typeof z.lower === 'number') zone.lower = z.lower
        if (typeof z.upper === 'number') zone.upper = z.upper
        if (typeof z.state === 'string') {
          zone.state = z.state
          if (ZONE_STATE_COLORS[z.state]) zone.color = ZONE_STATE_COLORS[z.state]
        }
        return zone
      })
    if (zones.length) out.zones = zones
  }

  return out
}

module.exports = {
  DEFAULT_MANIFEST,
  COMPASS_LIKE,
  RANGED_TYPES,
  quantityForPath,
  resolveManifest,
  allowedUnitsForPath,
  colorElementsFor,
  isHexColor,
  validateField,
  coerceField,
  metaToFieldDefaults
}
