const assert = require('assert')
const {
  buildManagerMdnsPacket,
  devicesFromMdnsPacket,
  parseMdnsPacket
} = require('../lib/manager')
const { makeManager } = require('./test-utils')

function dnsName (name) {
  const parts = name.split('.').filter(Boolean)
  return Buffer.concat([
    ...parts.map((part) => Buffer.concat([
      Buffer.from([Buffer.byteLength(part)]),
      Buffer.from(part)
    ])),
    Buffer.from([0])
  ])
}

function record (name, type, rdata) {
  const head = Buffer.alloc(10)
  head.writeUInt16BE(type, 0)
  head.writeUInt16BE(1, 2)
  head.writeUInt32BE(120, 4)
  head.writeUInt16BE(rdata.length, 8)
  return Buffer.concat([dnsName(name), head, rdata])
}

function txtData (items) {
  return Buffer.concat(items.map((item) => {
    const data = Buffer.from(item)
    return Buffer.concat([Buffer.from([data.length]), data])
  }))
}

function mdnsPacket () {
  const instance = 'espdisp-mdns-test._espdisp._tcp.local'
  const target = 'espdisp-mdns-test.local'
  const srv = Buffer.concat([
    Buffer.from([0, 0, 0, 0, 0, 80]),
    dnsName(target)
  ])
  const a = Buffer.from([192, 168, 50, 55])
  const answers = [
    record('_espdisp._tcp.local', 12, dnsName(instance)),
    record(instance, 33, srv),
    record(instance, 16, txtData([
      'proto=1',
      'device_id=espdisp-mdns-test',
      'path=/',
      'board=native_fake',
      'firmware=espdisp',
      'version=0.6.0-test',
      'display=480x480',
      'auth=basic',
      'seq=7'
    ])),
    record(target, 1, a)
  ]
  const header = Buffer.alloc(12)
  header.writeUInt16BE(0, 0)
  header.writeUInt16BE(0x8400, 2)
  header.writeUInt16BE(0, 4)
  header.writeUInt16BE(answers.length, 6)
  header.writeUInt16BE(0, 8)
  header.writeUInt16BE(0, 10)
  return Buffer.concat([header, ...answers])
}

const devices = devicesFromMdnsPacket(mdnsPacket(), { address: '192.168.50.1' })
assert.strictEqual(devices.length, 1)
assert.strictEqual(devices[0].deviceId, 'espdisp-mdns-test')
assert.strictEqual(devices[0].source, 'mdns')
assert.strictEqual(devices[0].address, '192.168.50.55')
assert.strictEqual(devices[0].port, 80)
assert.strictEqual(devices[0].authRequired, true)
assert.strictEqual(devices[0].board, 'native_fake')
assert.strictEqual(devices[0].firmware.version, '0.6.0-test')
assert.strictEqual(devices[0].display.width, 480)
assert.strictEqual(devices[0].mdns.txt.seq, '7')

const { manager } = makeManager()
manager.recordDiscoveredDevice(devices[0])
const discovered = manager.listDiscoveredDevices().devices
manager.close()

assert.strictEqual(discovered.length, 1)
assert.strictEqual(discovered[0].deviceId, 'espdisp-mdns-test')
assert.strictEqual(discovered[0].source, 'mdns')
assert.strictEqual(discovered[0].auth.mode, 'basic')

const managerMdns = parseMdnsPacket(buildManagerMdnsPacket({
  serverId: 'test-sk-manager',
  auth: { mode: 'dev-shared-token' },
  signalk: { port: 3000 }
}, {
  advertiseHost: '192.168.50.10'
}))

const ptr = managerMdns.records.find((rr) => rr.type === 12)
const srv = managerMdns.records.find((rr) => rr.type === 33)
const txt = managerMdns.records.find((rr) => rr.type === 16)
const a = managerMdns.records.find((rr) => rr.type === 1)

assert.strictEqual(ptr.name, '_espdisp-mgmt._tcp.local')
assert.strictEqual(ptr.ptr, 'test-sk-manager._espdisp-mgmt._tcp.local')
assert.strictEqual(srv.port, 3000)
assert.strictEqual(txt.txt.protocol, 'espdisp.management.v1')
assert.strictEqual(txt.txt.path, '/plugins/espdisp-manager')
assert.strictEqual(a.address, '192.168.50.10')
