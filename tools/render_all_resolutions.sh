#!/usr/bin/env bash
# Render every headless-LVGL sim env (see platformio.ini [env:sim*]) across
# the full resolution + theme matrix, and organize the output as:
#
#   docs/bench/screens/<env>/<WxH>/<name>.png           default (night) theme,
#                                                       every geometry
#   docs/bench/screens/<env>/<WxH>/<theme>/<name>.png   ALL 5 themes at the
#                                                       env's native geometry
#
# This is a superset driver of tools/sim_render.sh (which writes a flatter
# docs/sim-shots/ tree consumed by README.md/docs/*.md) — use this one when
# you want a full env x resolution x theme matrix in one place, e.g. to audit
# a new board geometry or palette before wiring it into the docs galleries.
#
# Sweeps:
#   1. Resolution sweep (default night palette, flat <env>/<WxH>/ files):
#        sim / sim-midl / sim-wind / sim-ap / sim-screens at 480x480,
#        800x480, 1024x600; sim-knob at its fixed 360x360; sim-control rect
#        at the three wide geometries + round at 360x360.
#   2. Theme sweep (<env>/<WxH>/<theme>/ files, all 5 palettes — night, day,
#      high-contrast, red-night, classic): every env at its NATIVE geometry
#      (480x480 for the touch envs, 360x360 for the knob; sim-control gets
#      rect@480x480 + round@360x360). Themes are selected at runtime via the
#      SIM_THEME env var (sim/sim_theme.h -> ui::use_theme), so the sweep
#      reuses the native-resolution build — no per-theme rebuilds.
#
# sim-midl renders all 6 gallery screens of the baked demo doc
# (include/midl_demo_doc.h), one PNG per screen, named by screen title
# (wind/course/engine/power/race/anchor — argv[1] of the sim-midl binary).
#
# Every produced PNG's pixel size is verified against the target geometry
# with `sips`; a mismatch is reported as DIM_FAIL in the summary matrix.
# BMP outputs are converted to PNG with macOS `sips`; if `sips` is
# unavailable the BMP is left in place and the dimension check is skipped.
#
# The <env>/ subtrees under docs/bench/screens are wiped and regenerated on
# every run (stale names from earlier layouts are deleted); the unrelated
# review/report files that share docs/bench/screens are left alone.
#
# Usage: tools/render_all_resolutions.sh
#   Set PLATFORMIO_WORKSPACE_DIR to isolate .pio state from other pio
#   invocations running concurrently against this checkout.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT_ROOT="docs/bench/screens"
WIDE_RES=("480x480" "800x480" "1024x600")
NATIVE_RES="480x480"
KNOB_RES="360x360"
THEMES=("night" "day" "high-contrast" "red-night" "classic")
ENVS=("sim" "sim-midl" "sim-wind" "sim-ap" "sim-screens" "sim-knob" "sim-control")

MIDL_SCREENS=("wind" "course" "engine" "power" "race" "anchor")
SCREENS=("nav" "depth" "steering" "route" "trip" "wind_classic" "wind_steer" "zoom-pos" "zoom-num")
KNOB_VIEWS=("ap_hud" "compass" "wind" "big" "menu-modepicker" "menu-display" "menu-view")

# pio puts build products under $PLATFORMIO_WORKSPACE_DIR/build when the
# workspace is redirected (parallel-agent isolation), else <project>/.pio/build.
# Resolve it here so we always run the binary we just built, never a stale
# .pio/build one from another invocation.
BUILD_DIR="${PLATFORMIO_WORKSPACE_DIR:-$PWD/.pio}/build"

declare -a RESULTS=()  # "env WxH theme name PASS|FAIL|DIM_FAIL|BUILD_FAIL"

to_png() {
    local bmp="$1"
    if command -v sips >/dev/null 2>&1; then
        sips -s format png "$bmp" --out "${bmp%.bmp}.png" >/dev/null 2>&1 && rm -f "$bmp"
    fi
}

# verify_png <png> <WxH> — true if the PNG's pixel size matches. Skipped
# (true) when sips is unavailable (the BMP was left in place).
verify_png() {
    local png="$1" res="$2" got
    command -v sips >/dev/null 2>&1 || return 0
    [ -f "$png" ] || return 1
    got="$(sips -g pixelWidth -g pixelHeight "$png" 2>/dev/null |
        awk '/pixelWidth/{w=$2} /pixelHeight/{h=$2} END{print w "x" h}')"
    [ "$got" = "$res" ]
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

# run_shot <bin> <env> <WxH> <theme|-> <name> <args...>
# Runs the sim binary with SIM_THEME set (theme "-" = default night, flat
# dir), writes <outdir>/<name>.bmp -> .png, verifies the PNG geometry, and
# records the result. A literal "{}" in <args...> is replaced by the BMP
# path; without one the path is appended (matches most sim argv shapes).
run_shot() {
    local bin="$1" env="$2" res="$3" theme="$4" name="$5"
    shift 5
    local outdir="${OUT_ROOT}/${env}/${res}"
    [ "$theme" != "-" ] && outdir="${outdir}/${theme}"
    mkdir -p "$outdir"
    local bmp="${outdir}/${name}.bmp"
    local themearg=""
    [ "$theme" != "-" ] && themearg="$theme"
    local -a args=()
    local subst=0 a
    for a in "$@"; do
        if [ "$a" = "{}" ]; then
            args+=("$bmp")
            subst=1
        else
            args+=("$a")
        fi
    done
    [ $subst -eq 0 ] && args+=("$bmp")
    local log="/tmp/render_all_resolutions_run.$$.log"
    if SIM_THEME="$themearg" "$bin" "${args[@]}" >/dev/null 2>"$log"; then
        to_png "$bmp"
        if verify_png "${outdir}/${name}.png" "$res"; then
            RESULTS+=("$env $res $theme $name PASS")
        else
            echo "  DIM MISMATCH: env=$env res=$res theme=$theme name=$name" >&2
            RESULTS+=("$env $res $theme $name DIM_FAIL")
        fi
    else
        echo "  RUN FAILED: env=$env res=$res theme=$theme name=$name" >&2
        cat "$log" >&2
        RESULTS+=("$env $res $theme $name FAIL")
    fi
    rm -f "$log"
}

# Per-env shot lists: shots_<env> <WxH> <theme|->
shots_sim() {
    run_shot "${BUILD_DIR}/sim/program" sim "$1" "$2" dashboard
}
shots_sim_midl() {
    local s
    for s in "${MIDL_SCREENS[@]}"; do
        run_shot "${BUILD_DIR}/sim-midl/program" sim-midl "$1" "$2" "$s" "$s"
    done
}
shots_sim_wind() {
    run_shot "${BUILD_DIR}/sim-wind/program" sim-wind "$1" "$2" wind
}
shots_sim_ap() {
    run_shot "${BUILD_DIR}/sim-ap/program" sim-ap "$1" "$2" ap
}
shots_sim_screens() {
    local s
    for s in "${SCREENS[@]}"; do
        run_shot "${BUILD_DIR}/sim-screens/program" sim-screens "$1" "$2" "$s" "$s"
    done
}
shots_sim_knob() {
    local v
    for v in "${KNOB_VIEWS[@]}"; do
        run_shot "${BUILD_DIR}/sim-knob/program" sim-knob "$1" "$2" "$v" "$v"
    done
}

# sweep_wide <env> <shots_fn>: resolution sweep at the default theme, plus
# the full theme sweep at the native 480x480 build (reused, no rebuild).
sweep_wide() {
    local env="$1" fn="$2" r w h t
    echo "=== env:${env} ==="
    for r in "${WIDE_RES[@]}"; do
        w="${r%x*}"
        h="${r#*x}"
        if build_env "$env" "$w" "$h"; then
            "$fn" "$r" "-"
            if [ "$r" = "$NATIVE_RES" ]; then
                for t in "${THEMES[@]}"; do
                    "$fn" "$r" "$t"
                done
            fi
        else
            RESULTS+=("$env $r - build BUILD_FAIL")
        fi
    done
}

# Regenerate the env subtrees from scratch so stale files from older layouts
# (e.g. sim-midl/<WxH>/midl.png before the per-screen names) never linger.
for e in "${ENVS[@]}"; do
    rm -rf "${OUT_ROOT:?}/${e}"
done

sweep_wide sim shots_sim
sweep_wide sim-midl shots_sim_midl
sweep_wide sim-wind shots_sim_wind
sweep_wide sim-ap shots_sim_ap
sweep_wide sim-screens shots_sim_screens

echo "=== env:sim-knob (round knob views + menus, fixed ${KNOB_RES}) ==="
if build_env sim-knob; then
    shots_sim_knob "$KNOB_RES" "-"
    for t in "${THEMES[@]}"; do
        shots_sim_knob "$KNOB_RES" "$t"
    done
else
    RESULTS+=("sim-knob $KNOB_RES - build BUILD_FAIL")
fi

echo "=== env:sim-control (controlled frame overlay) ==="
if build_env sim-control; then
    ctl_bin="${BUILD_DIR}/sim-control/program"
    # Resolution sweep: rect shape shares the overlay across every square/
    # landscape touch panel (geometry is runtime argv); round is the fixed
    # 360x360 knob panel.
    for r in "${WIDE_RES[@]}"; do
        w="${r%x*}"
        h="${r#*x}"
        for count in 1 3; do
            run_shot "$ctl_bin" sim-control "$r" "-" "control-frame-rect-${count}" \
                rect "$count" "{}" "$w" "$h"
        done
    done
    for count in 1 3; do
        run_shot "$ctl_bin" sim-control "$KNOB_RES" "-" "control-frame-round-${count}" \
            round "$count"
    done
    # Theme sweep: one representative shot per shape at native geometry.
    for t in "${THEMES[@]}"; do
        run_shot "$ctl_bin" sim-control "$NATIVE_RES" "$t" "control-frame-rect-3" rect 3
        run_shot "$ctl_bin" sim-control "$KNOB_RES" "$t" "control-frame-round-3" round 3
    done
else
    RESULTS+=("sim-control $NATIVE_RES - build BUILD_FAIL")
fi

echo
echo "=== summary matrix (pass/total per env x theme; '-' = default-night resolution sweep) ==="
printf '%s\n' "${RESULTS[@]}" | awk '
{
    env = $1; theme = $3; st = $5
    key = env SUBSEP theme
    tot[key]++
    if (st == "PASS") ok[key]++
    envs[env] = 1
    themes[theme] = 1
}
END {
    ntheme = split("- night day high-contrast red-night classic", order, " ")
    printf "%-14s", "env"
    for (i = 1; i <= ntheme; i++)
        if (order[i] in themes) printf " %14s", order[i]
    printf "\n"
    nenv = split("sim sim-midl sim-wind sim-ap sim-screens sim-knob sim-control", eord, " ")
    for (e = 1; e <= nenv; e++) {
        env = eord[e]
        if (!(env in envs)) continue
        printf "%-14s", env
        for (i = 1; i <= ntheme; i++) {
            t = order[i]
            if (!(t in themes)) continue
            key = env SUBSEP t
            if (key in tot)
                printf " %14s", (ok[key] + 0) "/" tot[key]
            else
                printf " %14s", "-"
        }
        printf "\n"
    }
}'
echo
pass=0
fail=0
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
echo "output -> ${OUT_ROOT}/<env>/<WxH>/*.png (night default) and ${OUT_ROOT}/<env>/<WxH>/<theme>/*.png"
[ "$fail" -eq 0 ]
