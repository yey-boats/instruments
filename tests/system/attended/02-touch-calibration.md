# 02 Touch calibration

**Goal**: a fresh calibration corrects the GT911's compressed Y axis
so taps land where the user's finger is, including the top of the
screen.

## Background

The GT911 on this panel reports raw `y ∈ [12, 297]` for visual
`[0, 479]` (~1.68× compression). Without calibration, taps near the
top register inside the MOB pill zone instead of on the top tiles.

## Steps

1. From any screen, type `screen touch_grid` via serial / BLE
   (`make ble-cmd CMD="screen touch_grid"`). ⬜
2. The screen shows a 10×10 grid of targets and TEST mode is active. ⬜
3. Tap each target once with a single finger; the spot is marked. ⬜
4. After tapping all 100 cells (or `screen touch_cal` / on-screen
   "CAL" button), the device runs the affine solver and logs:

   ```
   [cal] saved a=... b=... c=... d=... e=... f=...
   ```

   `a` should be close to **1.0** and `e` close to **1.68** on this
   board. ⬜
5. Return to `screen dashboard`. ⬜
6. Tap the **WIND** tile (top-left). The wind detail screen should
   open. ⬜
7. Power-cycle the device and verify calibration persists by
   repeating step 6. ⬜

## Pass criteria

- Step 6 succeeds (taps on the top half register). 
- Step 7 succeeds (calibration survives reboot).

## If it fails

- `[cal] rejected implausible matrix` → see plausibility bounds in
  `src/touch_cal.cpp`; widen if your panel is in a tighter or wider
  range than expected.
- Some top cells unreachable → re-run `screen touch_grid`; ensure
  every cell got at least one tap.
