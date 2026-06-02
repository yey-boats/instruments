const assert = require('assert')
const { makeManager } = require('./test-utils')

const { manager } = makeManager({
  auth: { mode: 'provision-token', provisionToken: 'provision-secret' }
})

const reg = manager.registerDevice({
  device: { id: 'espdisp-token-test', name: 'Token Test' }
}, { provision: 'provision-secret' })

assert.ok(reg.deviceToken)
let device = manager.getDevice('espdisp-token-test')
assert.strictEqual(device.deviceToken, undefined)
assert.ok(device.deviceTokenHash.startsWith('sha256:'))
assert.strictEqual(manager.authStatus('espdisp-token-test').provisioned, true)

const heartbeat = manager.updateStatus('espdisp-token-test', {
  config: { hash: manager.generateConfig('espdisp-token-test').hash, applied: true }
}, { bearer: reg.deviceToken })
assert.strictEqual(heartbeat.status, 'ok')

const rotated = manager.rotateDeviceToken('espdisp-token-test')
assert.ok(rotated.deviceToken)
device = manager.getDevice('espdisp-token-test')
assert.strictEqual(device.deviceToken, undefined)
assert.ok(device.deviceTokenHash.startsWith('sha256:'))

assert.throws(() => {
  manager.updateStatus('espdisp-token-test', {}, { bearer: reg.deviceToken })
}, /invalid device token/)
assert.strictEqual(manager.updateStatus('espdisp-token-test', {}, {
  bearer: rotated.deviceToken
}).status, 'ok')

manager.revokeDeviceToken('espdisp-token-test')
assert.strictEqual(manager.authStatus('espdisp-token-test').provisioned, false)
assert.throws(() => {
  manager.updateStatus('espdisp-token-test', {}, { bearer: rotated.deviceToken })
}, /invalid device token/)

const legacy = makeManager({ auth: { mode: 'provision-token', provisionToken: 'legacy-provision' } })
const provision = legacy.manager.createProvisioningToken({ token: 'one-time-provision', uses: 1 })
assert.strictEqual(provision.token, 'one-time-provision')
assert.strictEqual(legacy.manager.store.provisioning.tokens[0].token, undefined)
assert.ok(legacy.manager.store.provisioning.tokens[0].tokenHash.startsWith('sha256:'))

const provisioned = legacy.manager.registerDevice({
  device: { id: 'espdisp-provision-hash' }
}, { provision: 'one-time-provision' })
assert.strictEqual(provisioned.status, 'registered')
assert.strictEqual(legacy.manager.store.provisioning.tokens[0].usesRemaining, 0)

legacy.manager.store.registry.devices['espdisp-legacy-token'] = {
  id: 'espdisp-legacy-token',
  name: 'Legacy Token',
  deviceToken: 'old-cleartext-token',
  deviceTokenId: 'old',
  assignedProfile: 'default'
}
legacy.manager.store.saveRegistry()

assert.strictEqual(legacy.manager.updateStatus('espdisp-legacy-token', {}, {
  bearer: 'old-cleartext-token'
}).status, 'ok')
const migrated = legacy.manager.getDevice('espdisp-legacy-token')
assert.strictEqual(migrated.deviceToken, undefined)
assert.ok(migrated.deviceTokenHash.startsWith('sha256:'))

legacy.manager.store.provisioning.tokens.push({
  id: 'legacy-prov-token',
  token: 'legacy-clear-provision',
  createdAt: new Date().toISOString(),
  expiresAt: new Date(Date.now() + 60000).toISOString(),
  usesRemaining: 1,
  note: ''
})
legacy.manager.store.saveProvisioning()
const legacyProvReg = legacy.manager.registerDevice({
  device: { id: 'espdisp-legacy-provision' }
}, { provision: 'legacy-clear-provision' })
assert.strictEqual(legacyProvReg.status, 'registered')
const legacyProv = legacy.manager.store.provisioning.tokens.find((item) => {
  return item.id === 'legacy-prov-token'
})
assert.strictEqual(legacyProv.token, undefined)
assert.ok(legacyProv.tokenHash.startsWith('sha256:'))
