#!/usr/bin/env bash
# Regression check: a screen rebuilt in place (what a live theme switch or a
# config push does) must render exactly like a freshly built one.
#
# Each hand-built screen keeps per-label "dirty-value" caches (ui_dirty.h) so
# refresh() skips unchanged text. If a cache survives a rebuild, the new
# widgets keep their placeholder text for every value that has not changed
# since: after a day/night switch the SYSTEM page showed "-" for battery,
# Wi-Fi and IP, DEPTH lost its water temperature, TRIP read 0.00 nm.
#
# Renders each sim-screens id twice (normal, SIM_REBUILD=1) and compares.
# The touch screens (touch_grid, touch_cal) also run under AddressSanitizer
# (env:sim-screens-asan): their raw-touch poll timers must die with the root,
# or a tap after a rebuild writes freed objects (instruments#73).
# Usage: tools/check_rebuild_render.sh   (builds env:sim-screens + -asan first)
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
pio run -e sim-screens >"$out/build.log" 2>&1 || { cat "$out/build.log"; exit 1; }
pio run -e sim-screens-asan >"$out/build-asan.log" 2>&1 || { cat "$out/build-asan.log"; exit 1; }
bin=.pio/build/sim-screens/program
asan=.pio/build/sim-screens-asan/program
fail=0
for id in touch_grid touch_cal; do
    if ! SIM_REBUILD=1 "$asan" "$id" "$out/$id-asan.bmp" >"$out/$id-asan.log" 2>&1; then
        grep -E "ERROR: AddressSanitizer|^    #[0-4] " "$out/$id-asan.log" || tail -20 "$out/$id-asan.log"
        echo "FAIL  $id: rebuild under ASan (poll timer outlived its root?)"
        fail=1
    fi
done
for id in nav depth steering route trip wind_steer wind_classic zoom-pos zoom-num touch_grid touch_cal; do
    "$bin" "$id" "$out/$id-once.bmp" >/dev/null
    SIM_REBUILD=1 "$bin" "$id" "$out/$id-rebuilt.bmp" >/dev/null
    if cmp -s "$out/$id-once.bmp" "$out/$id-rebuilt.bmp"; then
        echo "ok    $id"
    else
        echo "FAIL  $id: rebuilt screen renders differently (stale dirty-value cache?)"
        fail=1
    fi
done
exit $fail
