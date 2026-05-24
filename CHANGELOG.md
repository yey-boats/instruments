# Changelog

All notable changes are documented here. Format roughly follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- License switched from MIT to **PolyForm Noncommercial 1.0.0** — free for personal, research, and noncommercial use; commercial use requires a separate license
- README rewritten in a more generic, product-focused style
- Repository copyright attributed to `navado and contributors`
- Security disclosures routed through GitHub private security advisories

### Added
- `Makefile` exposing the development pipeline (`make build/test/flash/ota/monitor/ble/demo-up/...`)
- Initial public release scaffolding (LICENSE, README, CI, release workflow)
- ESP32-4848S040 board support (ST7701 RGB panel + GT911 touch)
- SignalK WebSocket client subscribing to navigation / wind / depth / battery
- 2×2 LVGL dashboard at 480×480, 5 Hz refresh
- WiFi STA + AP fallback, ArduinoOTA, mDNS
- BLE Nordic UART service for logs + console commands
- UDP log broadcast on port 9999
- `tools/ble_console.py` — host-side BLE debug client
- `tools/fake_boat.py` — synthetic SignalK data pusher for development
- Host-side unit tests for the SignalK delta parser
