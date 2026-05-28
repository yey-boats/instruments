const assert = require('assert')
const fs = require('fs')
const path = require('path')

const root = path.join(__dirname, '..')
const pkg = require('../package.json')

assert.ok(pkg.keywords.includes('signalk-webapp'))
assert.ok(pkg.keywords.includes('signalk-node-server-plugin'))
assert.strictEqual(pkg.signalk.displayName, 'ESP Display Manager')
assert.strictEqual(pkg.signalk.appIcon, './icon.svg')
assert.ok(fs.existsSync(path.join(root, 'public', 'index.html')))
assert.ok(fs.existsSync(path.join(root, 'public', 'icon.svg')))
