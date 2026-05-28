const fs = require('fs')
const path = require('path')

function ensureDir (dir) {
  fs.mkdirSync(dir, { recursive: true })
}

function readJson (file, fallback) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'))
  } catch (err) {
    if (err.code === 'ENOENT') return fallback
    throw err
  }
}

function writeJson (file, value) {
  ensureDir(path.dirname(file))
  const tmp = `${file}.tmp`
  fs.writeFileSync(tmp, `${JSON.stringify(value, null, 2)}\n`)
  fs.renameSync(tmp, file)
}

function appendJsonl (file, value) {
  ensureDir(path.dirname(file))
  fs.appendFileSync(file, `${JSON.stringify(value)}\n`)
}

class JsonStore {
  constructor (dataDir) {
    this.dataDir = dataDir
    this.registryFile = path.join(dataDir, 'registry.json')
    this.profilesFile = path.join(dataDir, 'profiles.json')
    this.commandsFile = path.join(dataDir, 'commands.json')
    this.discoveryFile = path.join(dataDir, 'discovery.json')
    this.firmwareFile = path.join(dataDir, 'firmware-catalog.json')
    this.jobsFile = path.join(dataDir, 'firmware-jobs.json')
    this.provisioningFile = path.join(dataDir, 'provisioning.json')
    this.auditFile = path.join(dataDir, 'audit.jsonl')
  }

  init () {
    ensureDir(this.dataDir)
    this.registry = readJson(this.registryFile, { devices: {} })
    this.profiles = readJson(this.profilesFile, defaultProfiles())
    this.commands = readJson(this.commandsFile, { commands: [] })
    this.discovery = readJson(this.discoveryFile, { devices: {} })
    this.firmware = readJson(this.firmwareFile, { artifacts: [] })
    this.jobs = readJson(this.jobsFile, { jobs: [] })
    this.provisioning = readJson(this.provisioningFile, { tokens: [] })
  }

  saveRegistry () { writeJson(this.registryFile, this.registry) }
  saveProfiles () { writeJson(this.profilesFile, this.profiles) }
  saveCommands () { writeJson(this.commandsFile, this.commands) }
  saveDiscovery () { writeJson(this.discoveryFile, this.discovery) }
  saveFirmware () { writeJson(this.firmwareFile, this.firmware) }
  saveJobs () { writeJson(this.jobsFile, this.jobs) }
  saveProvisioning () { writeJson(this.provisioningFile, this.provisioning) }

  audit (type, subject, data) {
    appendJsonl(this.auditFile, {
      time: new Date().toISOString(),
      type,
      subject,
      data: data || {}
    })
  }
}

function defaultProfiles () {
  return {
    profiles: {
      default: {
        id: 'default',
        name: 'Default ESP Display',
        version: 1,
        updatedAt: new Date(0).toISOString(),
        config: {
          settings: {
            defaultScreen: 'dashboard',
            theme: 'day',
            brightness: 0.8,
            demoMode: false
          },
          sources: {
            priority: ['signalk', 'nmea0183Wifi', 'nmea2000'],
            timeoutsMs: {
              signalk: 10000,
              nmea0183Wifi: 3000,
              nmea2000: 2000
            }
          },
          nmea0183Wifi: {
            enabled: true,
            mode: 'tcp',
            host: 'signalk.local',
            port: 10110
          },
          autopilot: {
            enabled: true,
            allowEngage: false,
            allowStandby: true,
            allowHeadingAdjust: true,
            backend: 'signalk'
          },
          layout: {
            version: 1,
            screens: [],
            variants: [
              {
                id: 'square-480',
                match: { display: { width: 480, height: 480 } },
                screens: []
              },
              {
                id: 'wide-800x480',
                match: { display: { width: 800, height: 480 } },
                screens: []
              }
            ]
          },
          widgets: {
            defaults: {
              fontSize: 18,
              labelFontSize: 12,
              valueFontSize: 32,
              unitFontSize: 14
            },
            items: {
              sog: {
                type: 'numeric',
                title: 'SOG',
                path: 'navigation.speedOverGround',
                unit: 'kn',
                fontSize: 42,
                precision: 1
              },
              heading: {
                type: 'numeric',
                title: 'HDG',
                path: 'navigation.headingTrue',
                unit: 'deg',
                fontSize: 36,
                precision: 0
              },
              autopilotState: {
                type: 'text',
                title: 'AP',
                path: 'steering.autopilot.state',
                fontSize: 24,
                requires: { capability: 'autopilotControls' }
              }
            },
            variants: [
              {
                id: 'square-480',
                match: { display: { width: 480, height: 480 } },
                defaults: {
                  fontSize: 18,
                  labelFontSize: 12,
                  valueFontSize: 34,
                  unitFontSize: 14
                }
              },
              {
                id: 'wide-800x480',
                match: { display: { width: 800, height: 480 } },
                defaults: {
                  fontSize: 20,
                  labelFontSize: 14,
                  valueFontSize: 46,
                  unitFontSize: 16
                }
              },
              {
                id: 'small-320x240',
                match: { display: { width: 320, height: 240 } },
                defaults: {
                  fontSize: 14,
                  labelFontSize: 10,
                  valueFontSize: 24,
                  unitFontSize: 12
                }
              }
            ]
          },
          debug: {
            logLevel: 'info',
            touchMode: 'irq'
          }
        }
      }
    }
  }
}

module.exports = {
  JsonStore,
  readJson,
  writeJson,
  ensureDir
}
