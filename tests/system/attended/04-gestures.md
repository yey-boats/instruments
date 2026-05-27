# 04 Gestures

**Goal**: edge-zone swipes navigate between sibling screens; up-swipe
opens settings.

## Prereqs

- Calibration completed (test 02).
- Start on `screen dashboard`.

## Steps

1. **Swipe left** (start near right edge, drag to left). Next screen
   in the carousel appears. ⬜
2. **Swipe right** (start near left edge). Previous screen. ⬜
3. **Swipe up from the bottom edge**. Settings drawer opens. ⬜
4. **Swipe down from settings**. Returns to the prior screen. ⬜
5. Tap-and-hold a tile for **500 ms** → long-press action (currently
   a no-op stub, but a serial log line should appear). ⬜
6. Quick taps anywhere should NOT register as swipes (tap_drift_px
   guard). ⬜

## Pass criteria

Steps 1–4 succeed; step 6 does not produce false swipes.

## If it fails

- Swipes too sensitive → tweak `swipe_min_px` in gesture handler.
- Settings drawer doesn't open → ensure up-swipe started inside the
  bottom edge zone (`edge_zone_px`).
