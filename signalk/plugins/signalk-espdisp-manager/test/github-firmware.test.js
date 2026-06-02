const assert = require('assert')
const { makeManager } = require('./test-utils')

module.exports = (async () => {
  const { manager, auth } = makeManager({
    auth: { mode: 'dev-shared-token', devToken: 'test-token' },
    firmware: { github: { enabled: false } }
  })
  const deviceId = 'espdisp-github-fw'
  manager.registerDevice({
    device: {
      id: deviceId,
      board: 'sunton_4848s040',
      chip: 'ESP32-S3',
      firmware: { name: 'espdisp', version: '0.5.0' }
    }
  }, auth)

  const githubFetch = async (url) => {
    if (url === 'https://api.github.com/repos/navado/esp32-boat-mfd/releases/latest') {
      return {
        ok: true,
        json: async () => ({
          tag_name: 'v0.6.0',
          prerelease: false,
          html_url: 'https://github.com/navado/esp32-boat-mfd/releases/tag/v0.6.0',
          assets: [
            {
              name: 'esp32-4848s040-merged_firmware.bin',
              size: 2097152,
              content_type: 'application/octet-stream',
              browser_download_url: 'https://github.com/navado/esp32-boat-mfd/releases/download/v0.6.0/esp32-4848s040-merged_firmware.bin'
            },
            {
              name: 'waveshare-touch-lcd-7b_1024x600-merged_firmware.bin',
              size: 2098000,
              content_type: 'application/octet-stream',
              browser_download_url: 'https://github.com/navado/esp32-boat-mfd/releases/download/v0.6.0/waveshare-touch-lcd-7b_1024x600-merged_firmware.bin'
            },
            {
              name: 'SHA256SUMS',
              browser_download_url: 'https://github.com/navado/esp32-boat-mfd/releases/download/v0.6.0/SHA256SUMS'
            }
          ]
        })
      }
    }
    if (url === 'https://github.com/navado/esp32-boat-mfd/releases/download/v0.6.0/SHA256SUMS') {
      return {
        ok: true,
        text: async () => [
          'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  esp32-4848s040-merged_firmware.bin',
          'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  waveshare-touch-lcd-7b_1024x600-merged_firmware.bin'
        ].join('\n')
      }
    }
    throw new Error(`unexpected fetch URL ${url}`)
  }

  manager.options.firmware.github.enabled = true
  const refreshed = await manager.refreshFirmwareFromGithub(githubFetch)
  assert.strictEqual(refreshed.refreshed.release, 'v0.6.0')
  assert.strictEqual(refreshed.refreshed.imported, 2)

  const artifact = manager.getFirmwareArtifact('github-v0.6.0-esp32-4848s040')
  assert.strictEqual(artifact.firmware.version, '0.6.0')
  assert.deepStrictEqual(artifact.compatibility.boards, ['sunton_4848s040'])
  assert.strictEqual(artifact.compatibility.releaseTarget, 'esp32-4848s040')
  assert.strictEqual(artifact.file.sha256, 'sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
  assert.strictEqual(artifact.file.url.endsWith('/esp32-4848s040-merged_firmware.bin'), true)

  const job = manager.createFirmwareJob(deviceId, { artifactId: artifact.artifactId })
  const command = manager.pendingCommands(deviceId).find((cmd) => {
    return cmd.type === 'firmware.update' && cmd.payload.jobId === job.jobId
  })
  assert.ok(command)
  assert.strictEqual(command.payload.url, artifact.file.url)
  assert.strictEqual(command.payload.version, '0.6.0')
  assert.strictEqual(command.payload.sha256, 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')
})()
