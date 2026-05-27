# Attended system tests

These tests require a human touching the device. Each is a numbered
markdown file with steps and pass criteria. Run top to bottom.

| # | File                              | Coverage                              |
|---|-----------------------------------|---------------------------------------|
| 1 | `01-first-boot-provisioning.md`   | WiFi captive portal, BLE provisioning |
| 2 | `02-touch-calibration.md`         | 10x10 cal flow, persistence           |
| 3 | `03-dashboard-tile-taps.md`       | Each tile navigates correctly         |
| 4 | `04-gestures.md`                  | Swipe left/right/up edge zones        |
| 5 | `05-settings-ui.md`               | Brightness, theme, demo toggle        |
| 6 | `06-mob-button.md`                | MOB activation + pill display         |
| 7 | `07-multi-source-failover.md`     | Live demo of source priority + UI     |

Mark each step as ✅ or ❌ as you go. If a step fails, capture a photo
of the screen and file a bug with the markdown file # + step #.
