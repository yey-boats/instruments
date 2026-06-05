// Playwright snapshot harness for every widget kind.
//
// Renders the editor's widgetPreview() into a sized tile container
// (mirrors the device's 234x198 sunton-480 QuadGrid tile), screenshots
// it to docs/widget-previews/, and asserts:
//   - PNG bytes were written (non-trivial render)
//   - no two text-bearing elements in the preview have >2 px overlap
//     in BOTH axes (enforces "labels don't overlap numbers")
//
// We extract only the CSS + the JS functions widgetPreview() actually
// needs (el, PATH_SAMPLES, sampleFor, chromeCap, widgetPreview) so the
// page doesn't try to bootstrap API calls or DOM event handlers from
// the full editor.

const { test, expect } = require('@playwright/test')
const fs = require('fs')
const path = require('path')

const OUT_DIR = path.resolve(__dirname, '..', '..', '..', '..', 'docs', 'widget-previews')
fs.mkdirSync(OUT_DIR, { recursive: true })

function extractBlock (src, startNeedle) {
  const i = src.indexOf(startNeedle)
  if (i < 0) throw new Error('layout-editor.html missing: ' + startNeedle)
  // Walk braces from the first `{` after the function/const head.
  let j = src.indexOf('{', i)
  let depth = 1
  ++j
  while (j < src.length && depth > 0) {
    const c = src[j]
    if (c === '{') depth++
    else if (c === '}') depth--
    ++j
  }
  return src.slice(i, j)
}

function loadHarness () {
  const editorPath = path.resolve(__dirname, '..', 'public', 'layout-editor.html')
  const src = fs.readFileSync(editorPath, 'utf8')
  const style = src.match(/<style>([\s\S]*?)<\/style>/)[1]
  const elFn = extractBlock(src, 'function el ')
  const samples = extractBlock(src, 'const PATH_SAMPLES =')
  const sampleFor = extractBlock(src, 'function sampleFor ')
  const chromeCapText = extractBlock(src, 'function chromeCapText ')
  const chromeCap = extractBlock(src, 'function chromeCap ')
  const widgetPreview = extractBlock(src, 'function widgetPreview ')
  return `<!DOCTYPE html><html><head>
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link href="https://fonts.googleapis.com/css2?family=Montserrat:wght@400;500;700&display=swap" rel="stylesheet">
    <style>${style}
      body { padding: 0; margin: 0; }
      /* Sunton-480 QuadGrid tile: 234x198 with the standard chrome. */
      #stage { width: 234px; height: 198px; background: var(--panel);
        border: 1px solid var(--panel-edge); border-radius: 8px;
        position: relative; overflow: hidden; box-sizing: border-box; }
      /* The editor's .wpreview rule sets its own border/bg/min-height;
         when embedded in the tile stage we want it edge-to-edge. */
      #stage .wpreview { border: 0; margin: 0; min-height: 0; height: 100%;
        background: transparent; border-radius: 0; padding: 8px; }
    </style></head>
    <body>
      <div id="stage"></div>
      <script>'use strict';
        ${elFn}
        ${samples}
        ${sampleFor}
        ${chromeCapText}
        ${chromeCap}
        ${widgetPreview}
        window.__renderPreview = function (widget, tile) {
          const stage = document.getElementById('stage')
          stage.replaceChildren()
          const preview = widgetPreview(widget, tile || {})
          stage.appendChild(preview)
        }
      </script>
    </body></html>`
}

const html = loadHarness()

const WIDGETS = [
  { name: 'numeric-sog',     widget: 'numeric',   tile: { title: 'SOG',    primary: 'navigation.speedOverGround' } },
  { name: 'numeric-depth',   widget: 'numeric',   tile: { title: 'DEPTH',  primary: 'environment.depth.belowTransducer' } },
  { name: 'numeric-battv',   widget: 'numeric',   tile: { title: 'BATT V', primary: 'electrical.batteries.house.voltage' } },
  { name: 'compass-hdg',     widget: 'compass',   tile: { title: 'HDG',    primary: 'navigation.headingTrue', secondary: 'navigation.courseOverGroundTrue' } },
  { name: 'gauge-soc',       widget: 'gauge',     tile: { title: 'SOC',    primary: 'electrical.batteries.house.stateOfCharge' } },
  { name: 'bar-soc',         widget: 'bar',       tile: { title: 'BATT',   primary: 'electrical.batteries.house.stateOfCharge' } },
  { name: 'bar-fuel',        widget: 'bar',       tile: { title: 'FUEL',   primary: 'tanks.fuel.0.currentLevel' } },
  { name: 'windRose-aws',    widget: 'windRose',  tile: { title: 'WIND',   primary: 'environment.wind.speedApparent' } },
  { name: 'text-position',   widget: 'text',      tile: { title: 'POS',    primary: 'navigation.position' } },
  { name: 'autopilot-auto',  widget: 'autopilot', tile: { title: 'AP',     primary: 'steering.autopilot.state' } },
  { name: 'button-stby',     widget: 'button',    tile: { title: 'STBY' } },
  { name: 'trend-depth',     widget: 'trend',     tile: { title: 'DEPTH 5m', primary: 'environment.depth.belowTransducer' } }
]

for (const w of WIDGETS) {
  test(`widget ${w.name} renders + no overlap`, async ({ page }) => {
    await page.setContent(html, { waitUntil: 'domcontentloaded' })
    await page.evaluate(() => document.fonts && document.fonts.ready).catch(() => {})
    await page.evaluate(([widget, tile]) => window.__renderPreview(widget, tile), [w.widget, w.tile])
    const stage = page.locator('#stage')
    await expect(stage).toBeVisible()
    const out = path.join(OUT_DIR, `${w.name}.png`)
    await stage.screenshot({ path: out })

    // Bounding-box overlap check on text leaves.
    const boxes = await page.evaluate(() => {
      const r = []
      const walk = (n) => {
        if (n.nodeType !== 1) return
        const hasText = Array.from(n.childNodes).some((c) => c.nodeType === 3 && (c.textContent || '').trim())
        if (hasText) {
          const b = n.getBoundingClientRect()
          const txt = (n.textContent || '').trim()
          if (b.width > 0 && b.height > 0 && txt) r.push({ txt, x: b.x, y: b.y, w: b.width, h: b.height })
          return
        }
        for (const c of n.children) walk(c)
      }
      walk(document.getElementById('stage'))
      return r
    })
    const overlapping = []
    for (let i = 0; i < boxes.length; ++i) {
      for (let j = i + 1; j < boxes.length; ++j) {
        const a = boxes[i], b = boxes[j]
        const ox = Math.max(0, Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x))
        const oy = Math.max(0, Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y))
        if (ox > 2 && oy > 2) overlapping.push(`"${a.txt}" overlaps "${b.txt}"`)
      }
    }
    expect(overlapping, `widget ${w.name}: text elements overlap`).toEqual([])

    const size = fs.statSync(out).size
    expect(size).toBeGreaterThan(200)
  })
}
