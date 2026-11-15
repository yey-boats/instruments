#!/usr/bin/env bash
# Render the headless host LVGL harness shots to docs/sim-shots/ (BMP, PNG when
# macOS `sips` is present):
#   - env:sim-midl     the baked YB-MIDL demo doc (include/midl_demo_doc.h)
#                      through the REAL device MIDL render path
#                      (midl::solve_screen -> map_element -> create_freeform);
#   - env:sim-screens  the system touch screens (touch_grid / touch_cal);
#   - env:sim-control  the "controlled" frame overlay, rect + round.
# Content screens are MIDL records only (docs/midl/midl-only.md).
#
# Usage: tools/sim_render.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p docs/sim-shots

to_png() {
  if command -v sips >/dev/null 2>&1; then
    sips -s format png "docs/sim-shots/$1.bmp" --out "docs/sim-shots/$1.png" >/dev/null
    rm -f "docs/sim-shots/$1.bmp"
  fi
}

echo "=== rendering midl 480x480 ==="
PLATFORMIO_BUILD_FLAGS="-DSIM_LCD_W=480 -DSIM_LCD_H=480" pio run -e sim-midl >/dev/null
".pio/build/sim-midl/program" "docs/sim-shots/midl-square-480.bmp"
to_png midl-square-480

echo "=== rendering system touch screens ==="
pio run -e sim-screens >/dev/null
for s in touch_grid touch_cal; do
  ".pio/build/sim-screens/program" "$s" "docs/sim-shots/${s}-480x480.bmp"
  to_png "${s}-480x480"
done

echo "=== rendering control-frame overlay ==="
pio run -e sim-control >/dev/null
ctl_bin=".pio/build/sim-control/program"
# "<shape> <count> <name>"
CTL_CASES=("rect 1 control-frame-rect-1" "rect 3 control-frame-rect-3" \
  "round 1 control-frame-round-1" "round 3 control-frame-round-3")
for c in "${CTL_CASES[@]}"; do
  set -- $c
  "$ctl_bin" "$1" "$2" "docs/sim-shots/$3.bmp"
  to_png "$3"
done

echo "done -> docs/sim-shots/"
