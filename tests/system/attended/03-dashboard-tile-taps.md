# 03 Dashboard tile taps

**Goal**: every tile on the migrated `quad_grid` dashboard navigates to
its detail screen and emits a low-latency CommandRtt sample.

## Prereqs

- Calibration completed (test 02).
- `make demo-up` is running so tiles show realistic values.

## Steps

1. `screen dashboard` ⬜
2. Tap **WIND** tile (top-left). The wind detail screen opens within
   ~200 ms. Use swipe-right to return. ⬜
3. Tap **NAV** (top-right). Nav screen opens. Swipe right back. ⬜
4. Tap **DEPTH** (bottom-left). Depth screen + chart appears. Back. ⬜
5. Tap **SYSTEM** (bottom-right). Status screen appears. Back. ⬜
6. `bench` via BLE/serial — `CommandRtt`'s `count` increased by 4 and
   max value is < 300 ms. ⬜

## Pass criteria

All four navigations succeed and bench shows CommandRtt samples.

## If it fails

- Tap registers but no nav → check `[layout] tile CLICKED target=...`
  appears in serial log when you tap. If the target string is wrong,
  the screen id is incorrect.
- Top tiles unresponsive → re-run test 02 (calibration).
