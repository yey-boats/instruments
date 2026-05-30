const http = require('http')
const { test, expect } = require('@playwright/test')
const plugin = require('..')
const { makeManager } = require('./test-utils')
const { MockFirmware } = require('./mock-firmware')

function parseForm (body) {
  const out = {}
  for (const [key, value] of new URLSearchParams(body)) {
    if (Object.prototype.hasOwnProperty.call(out, key)) {
      out[key] = Array.isArray(out[key]) ? out[key].concat(value) : [out[key], value]
    } else {
      out[key] = value
    }
  }
  return out
}

async function readBody (req) {
  return await new Promise((resolve, reject) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', (chunk) => { body += chunk })
    req.on('end', () => resolve(body))
    req.on('error', reject)
  })
}

async function startHarness () {
  const { manager, auth } = makeManager({
    auth: { mode: 'dev-shared-token', devToken: 'test-token' },
    network: { domain: 'local', hostnamePrefix: 'espdisp', namingPolicy: 'device-id' }
  })
  const firmware = new MockFirmware(manager, {
    deviceId: 'espdisp-playwright-wide',
    auth,
    display: {
      width: 800,
      height: 480,
      rotation: 0,
      colorDepth: 16,
      density: 'mdpi',
      shape: 'wide'
    }
  })
  firmware.register()
  firmware.fetchConfig()
  firmware.heartbeat()

  const server = http.createServer(async (req, res) => {
    try {
      const url = new URL(req.url, 'http://127.0.0.1')
      if (req.method === 'GET' && url.pathname === '/plugins/espdisp-manager/ui/profiles') {
        res.setHeader('content-type', 'text/html; charset=utf-8')
        res.end(plugin._test.renderUi(manager, 'profiles', { params: {}, query: {} }))
        return
      }
      const presetMatch = url.pathname.match(/^\/plugins\/espdisp-manager\/ui\/profiles\/([^/]+)$/)
      if (req.method === 'GET' && presetMatch) {
        res.setHeader('content-type', 'text/html; charset=utf-8')
        res.end(plugin._test.renderUi(manager, 'preset', {
          params: { id: decodeURIComponent(presetMatch[1]) },
          query: {}
        }))
        return
      }
      const configMatch = url.pathname.match(/^\/plugins\/espdisp-manager\/ui\/devices\/([^/]+)\/config$/)
      if (req.method === 'GET' && configMatch) {
        res.setHeader('content-type', 'text/html; charset=utf-8')
        res.end(plugin._test.renderUi(manager, 'deviceConfig', {
          params: { id: decodeURIComponent(configMatch[1]) },
          query: {}
        }))
        return
      }
      if (req.method === 'POST' && url.pathname === '/plugins/espdisp-manager/profiles/import-dashboard') {
        const form = parseForm(await readBody(req))
        const imported = plugin._test.importDashboardPreset(manager, form, {
          'content-type': 'application/x-www-form-urlencoded'
        })
        res.statusCode = 303
        res.setHeader('location', `/plugins/espdisp-manager/ui/profiles/${encodeURIComponent(imported.id)}`)
        res.end()
        return
      }
      const applyMatch = url.pathname.match(/^\/plugins\/espdisp-manager\/ui\/profiles\/([^/]+)\/apply$/)
      if (req.method === 'POST' && applyMatch) {
        plugin._test.applyPresetForm(manager, decodeURIComponent(applyMatch[1]), parseForm(await readBody(req)))
        res.statusCode = 303
        res.setHeader('location', `/plugins/espdisp-manager/ui/profiles/${applyMatch[1]}`)
        res.end()
        return
      }
      res.statusCode = 404
      res.end('not found')
    } catch (err) {
      res.statusCode = 500
      res.end(err.stack || err.message)
    }
  })
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve))
  const port = server.address().port
  return {
    baseUrl: `http://127.0.0.1:${port}`,
    firmware,
    manager,
    close: () => new Promise((resolve) => server.close(resolve))
  }
}

test('operator creates a missing dashboard preset and applies populated real-data widgets', async ({ page }) => {
  const harness = await startHarness()
  try {
    await page.goto(`${harness.baseUrl}/plugins/espdisp-manager/ui/profiles`)

    const dashboard = {
      kind: 'espdisp.dashboard.v1',
      preset: { id: 'playwright-real-data', name: 'Playwright Real Data' },
      dashboard: {
        settings: { defaultScreen: 'dashboard', theme: 'day', brightness: 0.82 },
        widgets: {
          defaults: { fontSize: 18, labelFontSize: 12, valueFontSize: 48, unitFontSize: 16 },
          items: {
            sog: { type: 'numeric', title: 'SOG', path: 'navigation.speedOverGround', unit: 'kn', precision: 1 },
            awa: { type: 'numeric', title: 'AWA', path: 'environment.wind.angleApparent', unit: 'deg', precision: 0 },
            aws: { type: 'numeric', title: 'AWS', path: 'environment.wind.speedApparent', unit: 'kn', precision: 1 }
          }
        },
        layout: {
          screens: [
            {
              id: 'dashboard',
              type: 'grid',
              tiles: [
                { widget: 'sog', area: { col: 0, row: 0 } },
                { widget: 'awa', area: { col: 1, row: 0 } },
                { widget: 'aws', area: { col: 2, row: 0 } }
              ]
            }
          ]
        }
      }
    }

    await page.fill('input[name="presetId"]', 'playwright-real-data')
    await page.fill('textarea[name="raw"]', JSON.stringify(dashboard, null, 2))
    await page.getByRole('button', { name: 'Import preset' }).click()

    await expect(page).toHaveURL(/\/ui\/profiles\/playwright-real-data$/)
    await expect(page.getByRole('heading', { name: 'Playwright Real Data' })).toBeVisible()
    await page.locator('input[name="deviceIds"][value="espdisp-playwright-wide"]').check()
    await page.getByRole('button', { name: 'Apply to selected devices' }).click()

    harness.firmware.pollAndExecute()
    harness.firmware.heartbeat()

    await page.goto(`${harness.baseUrl}/plugins/espdisp-manager/ui/devices/espdisp-playwright-wide/config`)
    await expect(page.locator('select[name="profileId"]')).toHaveValue('playwright-real-data')
    await expect(page.getByText('navigation.speedOverGround')).toBeVisible()
    await expect(page.getByText('environment.wind.angleApparent')).toBeVisible()
    await expect(page.getByText('environment.wind.speedApparent')).toBeVisible()
    await expect(page.getByText('SOG').first()).toBeVisible()
    await expect(page.getByText('AWA').first()).toBeVisible()
    await expect(page.getByText('AWS').first()).toBeVisible()
    await expect(page.getByText('dashboard').first()).toBeVisible()
  } finally {
    await harness.close()
  }
})
