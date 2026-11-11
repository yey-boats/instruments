# Changelog

All notable changes are documented here. Format roughly follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Security
- ArduinoOTA (port 3232) is compiled out of release builds and runs on dev
  builds only with a password (instruments#30).
- The web password is stored as a salted PBKDF2-HMAC-SHA256 record instead of
  plaintext (instruments#30). **After a manual downgrade below this version,
  the manager must re-push the device config (password)**; until then the
  older firmware's API is setup-open. Rollbacks by the bootloader are safe.

### Added
- Waveshare Tier 1 PlatformIO profiles for Touch LCD 4, 4.3, 4.3B, 5
  800x480, 5 1024x600, 7 800x480, and 7B 1024x600.
- Board geometry metadata for display shape, density class, usable area,
  display bus, touch interrupt support, and manager-facing string names.
- Host tests for geometry classification, HDPI layout spacing, round safe-area
  handling, and board metadata helper names.

### Changed
- Device identity, manager status, board CLI, and layout context now use the
  board geometry/capability contract instead of assuming one square panel.

## [0.2.0] - 2026-05-24

### Added
- **NAV quadrant** now shows SOG (large), COG, HDG, and live lat/lon together
- **STATUS quadrant** is now a device-health panel: battery, SignalK state, IP, RSSI
- **Triple-tap any quadrant** to expand it to a centered focus view; triple-tap again to return to grid
- **Demo mode** (`demo [seconds]` console command) cycles each quadrant focused for 3 s with a visible DEMO badge — useful for video capture
- **FPS overlay** (`fps` console command) — toggleable on-screen Hz + flush time stats
- **`bench` console command** dumps fps, flush stats, heap/PSRAM free, SignalK status
- **MOB (Man Overboard)** — always-visible red button at top-right; long-press to capture current GPS as the MOB mark and enter a full-screen rescue view (distance / true bearing / return bearing / elapsed time); long-press the bottom button to clear
- **Alarm banner** at bottom-center auto-shows for SHALLOW WATER (depth<3 m), SIGNALK STALLED (>10 s without data), or BATTERY LOW (V<11.5)
- `net::setExtraCommandHandler` allows `main` to register its own console commands; demo/fps/bench/mob route through this
- Forward declarations + multiple sanity edits across `main.cpp`

### Changed
- Default SignalK target is now empty — configure with `sk <host>` rather than a baked-in hotspot IP

[0.2.0]: https://github.com/yey-boats/instruments/releases/tag/v0.2.0

## [0.1.0] - 2026-05-24

### Added
- ESP32-4848S040 board support (ST7701 RGB panel + GT911 touch)
- SignalK WebSocket client subscribing to navigation / wind / depth / battery / tank levels
- 2×2 LVGL dashboard at 480×480, 5 Hz refresh
- WiFi STA + AP fallback, ArduinoOTA, mDNS
- BLE Nordic UART service for logs + console commands
- UDP log broadcast on port 9999
- Multi-target `logf()` (Serial / BLE / UDP)
- `tools/ble_console.py` — host-side BLE debug client
- `tools/fake_boat.py` — synthetic SignalK data pusher for development
- `tools/dump_chunked.sh` — chunked, resumable full-flash backup
- Host-portable `signalk_parser` with 11 unit tests on PlatformIO `native` env
- `Makefile` exposing the development pipeline
- GitHub Actions: CI (build + test + lint) and tag-triggered release with binaries

### License
- PolyForm Noncommercial 1.0.0 — free for personal, research, and noncommercial use; commercial use requires a separate license

[Unreleased]: https://github.com/yey-boats/instruments/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/yey-boats/instruments/releases/tag/v0.1.0
