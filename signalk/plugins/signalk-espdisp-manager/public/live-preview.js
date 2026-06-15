/* Device-page live preview: renders an authored screen layout (widgets.items +
 * layout.screens) bound to LIVE SignalK data, so the operator sees what the
 * screen resembles with real values before switching/saving. Self-contained;
 * reads window.__espdispPreview = { screens:[{id,title,tiles:[...]}], current }.
 * Tiles are flattened server-side to {widget,title,path,unit,precision}. */
(function () {
  'use strict'
  const cfg = window.__espdispPreview || {}
  const root = document.getElementById('lp-root')
  const sel = document.getElementById('lp-screen')
  if (!root) return

  const values = Object.create(null) // signalk path -> latest value
  let currentScreenId = cfg.current || (cfg.screens && cfg.screens[0] && cfg.screens[0].id)

  // --- value formatting (mirror the device's unit conventions) -------------
  function format (tile) {
    const v = values[tile.path]
    if (v === undefined || v === null) return '--'
    if (typeof v !== 'number') return String(v)
    let x = v
    if (tile.unit === 'kn') x = v * 1.94384            // m/s -> knots
    else if (tile.unit === '°' || tile.unit === 'deg') x = v * 180 / Math.PI // rad -> deg
    else if (tile.unit === '%') x = v <= 1.0001 ? v * 100 : v               // ratio -> %
    const p = tile.precision != null ? tile.precision : 1
    return x.toFixed(p)
  }
  function percent (tile) {
    const v = values[tile.path]
    if (typeof v !== 'number') return 0
    const x = v <= 1.0001 ? v * 100 : v
    return Math.max(0, Math.min(100, x))
  }

  // --- render one screen's tiles into the grid -----------------------------
  function screenById (id) {
    const scr = (cfg.screens || []).find((s) => s.id === id)
    return scr || (cfg.screens || [])[0] || null
  }
  function renderScreen () {
    root.replaceChildren()
    const scr = screenById(currentScreenId)
    if (!scr || !Array.isArray(scr.tiles) || !scr.tiles.length) {
      const m = document.createElement('div')
      m.className = 'lp-empty'
      m.textContent = 'No managed layout for this view (firmware built-in screen).'
      root.appendChild(m)
      return
    }
    const grid = document.createElement('div')
    grid.className = 'lp-grid'
    scr.tiles.forEach((tile) => {
      const cell = document.createElement('div')
      cell.className = 'lp-tile lp-w-' + (tile.widget || 'numeric')
      const cap = document.createElement('div')
      cap.className = 'lp-cap'
      cap.textContent = (tile.title || (tile.path || '').split('.').pop() || '').toUpperCase()
      cell.appendChild(cap)
      if (tile.widget === 'bar') {
        const bar = document.createElement('div'); bar.className = 'lp-bar'
        const fill = document.createElement('div'); fill.className = 'lp-bar-fill'
        fill.style.height = percent(tile) + '%'
        bar.appendChild(fill); cell.appendChild(bar)
        const val = document.createElement('div'); val.className = 'lp-val lp-val-sm'
        val.textContent = format(tile) + (tile.unit ? ' ' + tile.unit : '')
        cell.appendChild(val)
      } else {
        const val = document.createElement('div'); val.className = 'lp-val'
        val.textContent = format(tile)
        const unit = document.createElement('span'); unit.className = 'lp-unit'
        unit.textContent = tile.unit || ''
        val.appendChild(unit); cell.appendChild(val)
      }
      grid.appendChild(cell)
    })
    root.appendChild(grid)
  }

  // throttle re-render to ~5 Hz (matches the device refresh cadence)
  let dirty = false
  setInterval(() => { if (dirty) { dirty = false; renderScreen() } }, 200)

  // --- live SignalK stream (same-origin; uses the logged-in session) -------
  function connect () {
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
    let ws
    try { ws = new WebSocket(proto + '//' + location.host + '/signalk/v1/stream?subscribe=all') } catch (e) { return }
    ws.onmessage = (e) => {
      let msg
      try { msg = JSON.parse(e.data) } catch (_) { return }
      if (!msg.updates) return
      msg.updates.forEach((u) => (u.values || []).forEach((val) => {
        if (val && typeof val.path === 'string') values[val.path] = val.value
      }))
      dirty = true
    }
    ws.onclose = () => setTimeout(connect, 3000)
    ws.onerror = () => { try { ws.close() } catch (_) {} }
  }

  if (sel) {
    sel.addEventListener('change', () => { currentScreenId = sel.value; renderScreen() })
  }
  renderScreen()
  connect()
}())
