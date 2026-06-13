#!/usr/bin/env bash
# Render the dashboard AND the fullscreen wind dial at every supported display
# class via the headless host LVGL harness (env:sim / env:sim-wind), writing
# BMP + PNG to docs/sim-shots/. LCD_W/LCD_H are compile-time, so each resolution
# is a separate build (PLATFORMIO_BUILD_FLAGS injects -DSIM_LCD_W/-DSIM_LCD_H).
#
# Usage: tools/sim_render.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p docs/sim-shots

# "WxH" per supported display class (800x480 covers the 4.3/5/7" Waveshare).
RES=("480x480" "800x480" "1024x600")

for r in "${RES[@]}"; do
  w="${r%x*}"; h="${r#*x}"
  echo "=== rendering ${w}x${h} ==="
  PLATFORMIO_BUILD_FLAGS="-DSIM_LCD_W=${w} -DSIM_LCD_H=${h}" pio run -e sim >/dev/null
  bin=".pio/build/sim/program"
  "$bin" "docs/sim-shots/dash-${w}x${h}.bmp"
  if command -v sips >/dev/null 2>&1; then
    sips -s format png "docs/sim-shots/dash-${w}x${h}.bmp" \
      --out "docs/sim-shots/dash-${w}x${h}.png" >/dev/null
    rm -f "docs/sim-shots/dash-${w}x${h}.bmp"
  fi
done

# Fullscreen wind dial (screen_wind.cpp) via env:sim-wind — gives the A / T wind
# indices and the current arrow an eyeball-able render alongside the dashboard.
for r in "${RES[@]}"; do
  w="${r%x*}"; h="${r#*x}"
  echo "=== rendering wind ${w}x${h} ==="
  PLATFORMIO_BUILD_FLAGS="-DSIM_LCD_W=${w} -DSIM_LCD_H=${h}" pio run -e sim-wind >/dev/null
  ".pio/build/sim-wind/program" "docs/sim-shots/wind-${w}x${h}.bmp"
  if command -v sips >/dev/null 2>&1; then
    sips -s format png "docs/sim-shots/wind-${w}x${h}.bmp" \
      --out "docs/sim-shots/wind-${w}x${h}.png" >/dev/null
    rm -f "docs/sim-shots/wind-${w}x${h}.bmp"
  fi
done

# Waveshare-knob round views (env:sim-knob) — fixed 360x360 round panel. One
# PNG per dedicated view (ap_hud / compass / wind / big) for the README gallery.
# The board id + 360x360 are pinned in the env, so no per-resolution rebuild.
echo "=== rendering knob 360x360 round views ==="
pio run -e sim-knob >/dev/null
knob_bin=".pio/build/sim-knob/program"
KNOB_VIEWS=("ap_hud" "compass" "wind" "big")
for v in "${KNOB_VIEWS[@]}"; do
  "$knob_bin" "$v" "docs/sim-shots/knob-${v}.bmp"
  if command -v sips >/dev/null 2>&1; then
    sips -s format png "docs/sim-shots/knob-${v}.bmp" \
      --out "docs/sim-shots/knob-${v}.png" >/dev/null
    rm -f "docs/sim-shots/knob-${v}.bmp"
  fi
done

# Optional 2x2 composite of the four round views (requires ImageMagick montage).
if command -v montage >/dev/null 2>&1; then
  montage \
    "docs/sim-shots/knob-ap_hud.png" "docs/sim-shots/knob-compass.png" \
    "docs/sim-shots/knob-wind.png" "docs/sim-shots/knob-big.png" \
    -tile 2x2 -geometry +8+8 -background black \
    "docs/sim-shots/knob-gallery.png" >/dev/null 2>&1 \
    && echo "wrote docs/sim-shots/knob-gallery.png"
fi

echo "done -> docs/sim-shots/"
