#!/usr/bin/env bash
# Regenerate the LVGL C font sources in src/fonts/ from the bundled TTFs.
#
# The embedded firmware compiles the generated .c directly; the TTFs here are
# build-time assets only (never linked into the image). Run this whenever the
# glyph set or typeface changes, then commit the regenerated .c.
#
# Typeface: B612 (Airbus / ENAC / intactile DESIGN), designed and tested for
# aircraft cockpit screens at low and medium resolution. SIL OFL 1.1, see
# B612-OFL.txt. It replaces LVGL's built-in Montserrat everywhere (lv_conf.h
# disables those) so every screen uses one family:
#   font_b612_14 / _20       B612 Regular   captions, units, small text
#   font_b612_28 / _38 / _48 B612 Bold      values and secondary numbers
#   font_xl_64               B612 Bold      hero digits (subset glyph set)
# The two LVGL symbols the UI uses (LV_SYMBOL_UP / LV_SYMBOL_DOWN) come from
# LVGL's bundled FontAwesome subset (FontAwesome Free, SIL OFL 1.1), merged
# into every size so the symbol macros keep working.
#
# Requires Node.js. Uses lv_font_conv (https://github.com/lvgl/lv_font_conv),
# fetched on demand into a temp prefix so nothing is installed globally.
# Without Node locally:
#   docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/w -w /w \
#     node:20-slim bash tools/fonts/gen_fonts.sh
#
# Usage:  tools/fonts/gen_fonts.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
LVFC_VER="1.5.3"
LVFC_PREFIX="${LVFC_PREFIX:-/tmp/lvfc}"
LVFC_BIN="$LVFC_PREFIX/node_modules/.bin/lv_font_conv"

if [ ! -x "$LVFC_BIN" ]; then
    echo "[gen_fonts] installing lv_font_conv@$LVFC_VER into $LVFC_PREFIX ..."
    npm install "lv_font_conv@$LVFC_VER" --no-save --prefix "$LVFC_PREFIX" >/dev/null
fi

# Printable ASCII, Latin-1 (°, ±, accented names), dashes, bullet and
# ellipsis. (B612 has no arrow or triangle glyphs; neither did Montserrat.)
TEXT_RANGE='0x20-0x7E,0xA0-0xFF,0x2013-0x2014,0x2022,0x2026'
SYMBOLS_FA='0xF077,0xF078'  # LV_SYMBOL_UP, LV_SYMBOL_DOWN
FA="tools/fonts/FontAwesome5-Solid+Brands+Regular.woff"

# Run from the repo root with relative paths so the generated files' "Opts:"
# header is identical on every machine (no absolute home path leaks in).
cd "$REPO"

gen_ui() {  # size ttf
    echo "[gen_fonts] font_b612_$1.c <- $(basename "$2") (${1}px, bpp4)"
    "$LVFC_BIN" \
        --font "$2" --range "$TEXT_RANGE" \
        --font "$FA" --range "$SYMBOLS_FA" \
        --size "$1" --bpp 4 --format lvgl --lv-include lvgl.h \
        --lv-font-name "font_b612_$1" \
        --no-compress -o "src/fonts/font_b612_$1.c"
}
gen_ui 14 tools/fonts/B612-Regular.ttf
gen_ui 20 tools/fonts/B612-Regular.ttf
gen_ui 28 tools/fonts/B612-Bold.ttf
gen_ui 38 tools/fonts/B612-Bold.ttf
gen_ui 48 tools/fonts/B612-Bold.ttf

# font_xl_64: the 64 px hero readout (zoom numbers, dial heroes; also the base
# glyphs that fit_hero_scale() scales up). Minimal glyph set: digits,
# sign/decimal, the degree sign, and the unit/cardinal letters the hero
# formatters print, plus space. B612 Bold is proportional, so the decimal
# point sits tight against its digits (the B612 Mono cell left a gap).
HERO_SYMBOLS='0123456789.-°STPNEWkmndegVA% '
echo "[gen_fonts] font_xl_64.c <- B612-Bold.ttf (64px, bpp4)"
"$LVFC_BIN" \
    --font tools/fonts/B612-Bold.ttf \
    --size 64 --bpp 4 --format lvgl --lv-include lvgl.h \
    --symbols "$HERO_SYMBOLS" \
    --lv-font-name font_xl_64 \
    --no-compress -o src/fonts/font_xl_64.c

echo "[gen_fonts] done. Review the diff and commit src/fonts/*.c."
