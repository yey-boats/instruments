#!/usr/bin/env bash
# Render every headless-LVGL sim env (see platformio.ini [env:sim*]) at every
# display geometry that applies to it, and organize the output as:
#
#   docs/bench/screens/<envname>/<WxH>/<name>.png
#
# This is a superset driver of tools/sim_render.sh (which writes a flatter
# docs/sim-shots/ tree consumed by README.md/docs/*.md) — use this one when
# you want a full env x resolution matrix in one place, e.g. to audit a new
# board geometry before wiring it into the docs galleries.
#
# Envs / geometries covered:
#   sim          dashboard tiles              480x480 / 800x480 / 1024x600
#   sim-midl     MIDL demo-doc render          480x480 / 800x480 / 1024x600
#   sim-wind     fullscreen wind dial          480x480 / 800x480 / 1024x600
#   sim-ap       autopilot HUD                 480x480 / 800x480 / 1024x600
#   sim-screens  QuadGrid screens + zoom views  480x480 / 800x480 / 1024x600
#   sim-knob     round knob views + menus       360x360 only (physical panel)
#   sim-control  "controlled" frame overlay     rect: 480x480/800x480/1024x600
#                                                round: 360x360 only
#
# LCD_W/LCD_H are compile-time for sim/sim-midl/sim-wind/sim-ap/sim-screens
# (PLATFORMIO_BUILD_FLAGS injects -DSIM_LCD_W/-DSIM_LCD_H per resolution, same
# trick as tools/sim_render.sh), so each resolution is a separate build for
# those envs. sim-knob's panel is a fixed 360x360 round board, no sweep
# needed. sim-control reads its geometry from argv at runtime (it owns
# board::geometry() itself), so one build covers every resolution.
#
# BMP outputs are converted to PNG with macOS `sips`; if `sips` is
# unavailable the BMP is left in place instead.
#
# Usage: tools/render_all_resolutions.sh
#   Set PLATFORMIO_WORKSPACE_DIR to isolate .pio state from other pio
#   invocations running concurrently against this checkout.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT_ROOT="docs/bench/screens"
WIDE_RES=("480x480" "800x480" "1024x600")

# pio puts build products under $PLATFORMIO_WORKSPACE_DIR/build when the
# workspace is redirected (parallel-agent isolation), else <project>/.pio/build.
# Resolve it here so we always run the binary we just built, never a stale
# .pio/build one from another invocation.
BUILD_DIR="${PLATFORMIO_WORKSPACE_DIR:-$PWD/.pio}/build"

declare -a RESULTS=()  # "env WxH name PASS|FAIL"

to_png() {
    local bmp="$1"
    if command -v sips >/dev/null 2>&1; then
        sips -s format png "$bmp" --out "${bmp%.bmp}.png" >/dev/null 2>&1 && rm -f "$bmp"
    fi
}

# build_env <env> [SIM_LCD_W SIM_LCD_H]
build_env() {
    local env="$1" w="${2:-}" h="${3:-}" log
    log="$(mktemp)"
    if [ -n "$w" ]; then
        PLATFORMIO_BUILD_FLAGS="-DSIM_LCD_W=${w} -DSIM_LCD_H=${h}" pio run -e "$env" >"$log" 2>&1
    else
        pio run -e "$env" >"$log" 2>&1
    fi
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "  BUILD FAILED: env=$env $( [ -n "$w" ] && echo "${w}x${h}" ) (see $log)" >&2
        tail -n 20 "$log" >&2
    else
        rm -f "$log"
    fi
    return $rc
}

# run_shot <bin> <outdir> <name> <args...>
# Runs the sim binary, writes <outdir>/<name>.bmp -> .png, records the result.
run_shot() {
    local bin="$1" env="$2" res="$3" name="$4"; shift 4
    local outdir="${OUT_ROOT}/${env}/${res}"
    mkdir -p "$outdir"
    local bmp="${outdir}/${name}.bmp"
    if "$bin" "$@" "$bmp" >/dev/null 2>/tmp/render_all_resolutions_run.$$.log; then
        to_png "$bmp"
        RESULTS+=("$env $res $name PASS")
    else
        echo "  RUN FAILED: env=$env res=$res name=$name" >&2
        cat "/tmp/render_all_resolutions_run.$$.log" >&2
        RESULTS+=("$env $res $name FAIL")
    fi
    rm -f "/tmp/render_all_resolutions_run.$$.log"
}

echo "=== env:sim (dashboard tiles) ==="
for r in "${WIDE_RES[@]}"; do
    w="${r%x*}"; h="${r#*x}"
    if build_env sim "$w" "$h"; then
        run_shot "${BUILD_DIR}/sim/program" sim "$r" dashboard
    else
        RESULTS+=("sim $r dashboard BUILD_FAIL")
    fi
done

echo "=== env:sim-midl (MIDL demo doc) ==="
for r in "${WIDE_RES[@]}"; do
    w="${r%x*}"; h="${r#*x}"
    if build_env sim-midl "$w" "$h"; then
        run_shot "${BUILD_DIR}/sim-midl/program" sim-midl "$r" midl
    else
        RESULTS+=("sim-midl $r midl BUILD_FAIL")
    fi
done

echo "=== env:sim-wind (fullscreen wind dial) ==="
for r in "${WIDE_RES[@]}"; do
    w="${r%x*}"; h="${r#*x}"
    if build_env sim-wind "$w" "$h"; then
        run_shot "${BUILD_DIR}/sim-wind/program" sim-wind "$r" wind
    else
        RESULTS+=("sim-wind $r wind BUILD_FAIL")
    fi
done

echo "=== env:sim-ap (autopilot HUD) ==="
for r in "${WIDE_RES[@]}"; do
    w="${r%x*}"; h="${r#*x}"
    if build_env sim-ap "$w" "$h"; then
        run_shot "${BUILD_DIR}/sim-ap/program" sim-ap "$r" ap
    else
        RESULTS+=("sim-ap $r ap BUILD_FAIL")
    fi
done

echo "=== env:sim-screens (QuadGrid screens + zoom views) ==="
SCREENS=("nav" "depth" "steering" "route" "trip" "wind_classic" "wind_steer" "zoom-pos" "zoom-num")
for r in "${WIDE_RES[@]}"; do
    w="${r%x*}"; h="${r#*x}"
    if build_env sim-screens "$w" "$h"; then
        for s in "${SCREENS[@]}"; do
            run_shot "${BUILD_DIR}/sim-screens/program" sim-screens "$r" "$s" "$s"
        done
    else
        for s in "${SCREENS[@]}"; do
            RESULTS+=("sim-screens $r $s BUILD_FAIL")
        done
    fi
done

echo "=== env:sim-knob (round knob views + menus, fixed 360x360) ==="
if build_env sim-knob; then
    KNOB_VIEWS=("ap_hud" "compass" "wind" "big" "menu-modepicker" "menu-display" "menu-view")
    for v in "${KNOB_VIEWS[@]}"; do
        run_shot "${BUILD_DIR}/sim-knob/program" sim-knob "360x360" "$v" "$v"
    done
else
    for v in "ap_hud" "compass" "wind" "big" "menu-modepicker" "menu-display" "menu-view"; do
        RESULTS+=("sim-knob 360x360 $v BUILD_FAIL")
    done
fi

echo "=== env:sim-control (controlled frame overlay) ==="
if build_env sim-control; then
    ctl_bin="${BUILD_DIR}/sim-control/program"
    # rect shape shares the overlay across every square/landscape touch panel.
    for r in "${WIDE_RES[@]}"; do
        w="${r%x*}"; h="${r#*x}"
        for count in 1 3; do
            name="control-frame-rect-${count}"
            outdir="${OUT_ROOT}/sim-control/${r}"
            mkdir -p "$outdir"
            bmp="${outdir}/${name}.bmp"
            if "$ctl_bin" rect "$count" "$bmp" "$w" "$h" >/dev/null 2>&1; then
                to_png "$bmp"
                RESULTS+=("sim-control $r $name PASS")
            else
                echo "  RUN FAILED: env=sim-control res=$r name=$name" >&2
                RESULTS+=("sim-control $r $name FAIL")
            fi
        done
    done
    # round shape is the fixed 360x360 knob panel only.
    for count in 1 3; do
        run_shot "$ctl_bin" sim-control "360x360" "control-frame-round-${count}" round "$count"
    done
else
    for r in "${WIDE_RES[@]}" "360x360"; do
        RESULTS+=("sim-control $r control-frame BUILD_FAIL")
    done
fi

echo
echo "=== summary ==="
pass=0; fail=0
for row in "${RESULTS[@]}"; do
    status="${row##* }"
    if [ "$status" = "PASS" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $row"
    fi
done
echo "$pass passed, $fail failed (of ${#RESULTS[@]})"
echo "output -> ${OUT_ROOT}/<env>/<WxH>/*.png"
[ "$fail" -eq 0 ]
