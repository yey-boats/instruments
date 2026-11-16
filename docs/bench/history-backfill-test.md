# Bench test: trend history backfill

What it proves: a trend tile fills its chart from the SignalK History API when
its screen is shown (firmware: `src/trend_backfill.cpp`; Display Manager
previews: `public/signalk-history.js`), instead of starting empty at the right
edge. Spec: midl `docs/specs/2026-11-15-trend-history.md` §4.

The bench SignalK has no history data by default (its `kip` provider answers
with `data: []`). `tools/bench/signalk-memory-history` is a bench-only
provider that keeps every numeric `vessels.self` value in memory for 60
minutes and answers `GET /signalk/v2/api/history/values` (average, min, max,
first, last). Not for boats.

## Setup

```sh
tools/bench/history_backfill_test.sh install   # copy, npm install, enable, make it the default provider, restart SignalK
# wait at least the trend's window (10 min for the default 10m window)
tools/bench/history_backfill_test.sh check     # exit 0 once >= 80 % of the 120 buckets have data
```

`check` asks exactly what the display asks:
`/signalk/v2/api/history/values?paths=<path>:average&duration=PT600S&resolution=5.000`
(`HIST_PATH`, `WINDOW_S`, `POINTS` override the defaults).

## Cases

The display must run firmware with trend history (instruments #101 or later)
and show a screen with a trend tile (e.g. the "Wind history" preset: TWS 10m).

| # | Do | Expect |
|---|----|--------|
| 1 | Show another screen, then the trend screen again (Display Manager: Show on device, or `POST /api/screen/<id>`). Screenshot within 10 s (`/api/screenshot.png`). | The TWS chart already spans the whole window, with min/max labels: backfilled. Without the provider the line starts at the right edge and grows ~1.5 px per 5 s. |
| 2 | Same, but the tile's history window is `1h` (edit it in the display's `/editor` or the builder). Wait until `WINDOW_S=3600 check` passes. | The chart spans the hour right away. |
| 3 | Reload the Display Manager device page. | The live-view preview's trend is full within a second or two (its own backfill), not growing from the right. |
| 4 | `remove`, wait for SignalK to restart, repeat case 1. | The display falls back to an empty chart that fills live; no errors or reboots in the display's logs (`[trend]` / `[bench]` lines). |
| 5 | While case 1 is backfilling, switch screens quickly 3 times. | No stale series on the new screen (the generation id drops late answers); heap steady in the heartbeat. |

Record results (screenshots, `[bench] trend series` log lines, heap before and
after) in the PR or issue that asked for the test.

## Cleanup

```sh
tools/bench/history_backfill_test.sh remove    # disable the provider, restore the previous default
```
