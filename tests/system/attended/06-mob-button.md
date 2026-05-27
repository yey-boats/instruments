# 06 MOB button

**Goal**: the Man-Overboard pill captures current position, renders
across all screens, and tracks bearing/distance back.

## Prereqs

- A SignalK source with live position (or `fake_boat.py` running).
- `screen dashboard`.

## Steps

1. Confirm `lat` / `lon` in `/api/sk` are non-null. ⬜
2. Long-press the **MOB** button (top-right pill area, ≥1.5 s). ⬜
3. The pill turns red and shows distance + bearing to the captured
   position. ⬜
4. Switch through every screen (`screen wind`, `screen depth`, etc.).
   The MOB pill stays visible on each one at the same screen position. ⬜
5. Send the device to a different lat/lon (modify `fake_boat.py` or
   push a delta). Distance/bearing in the pill updates. ⬜
6. Long-press MOB again to clear. Pill returns to neutral state. ⬜

## Pass criteria

Pill appears, tracks correctly across screens, clears cleanly.

## If it fails

- Pill never lights → check `[mob]` logs; long-press threshold may
  be off, or `sk::data.lat/lon` may be NaN.
- Layout breaks on a particular screen → that screen forgot to
  reserve the MOB-safe band (y=6..62).
