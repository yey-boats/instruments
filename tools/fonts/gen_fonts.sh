#!/usr/bin/env bash
# Regenerate the LVGL C font sources in src/fonts/ from the bundled TTFs.
#
# The embedded firmware compiles the generated .c directly; the TTFs here are
# build-time assets only (never linked into the image). Run this whenever the
# glyph set or typeface changes, then commit the regenerated .c.
#
# Requires Node.js. Uses lv_font_conv (https://github.com/lvgl/lv_font_conv),
# fetched on demand into a temp prefix so nothing is installed globally.
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

# font_xl_64 — 64 px monospace hero readout (zoom fullscreen numbers + wind-rose
# readouts). Monospace (JetBrains Mono) keeps digit width constant so the big
# numbers don't jitter as values change. Glyph set is intentionally minimal:
# digits, sign/decimal, the degree sign, and the few unit/cardinal letters used
# by the hero formatters (S T P N E W k m n d e g V A %) plus space.
SYMBOLS='0123456789.-°STPNEWkmndegVA% '

echo "[gen_fonts] font_xl_64.c <- JetBrainsMono-Regular.ttf (64px, bpp4)"
# Run from the repo root with relative paths so the generated file's "Opts:"
# header is identical on every machine (no absolute home path leaks in).
cd "$REPO"
"$LVFC_BIN" \
    --font tools/fonts/JetBrainsMono-Regular.ttf \
    --size 64 --bpp 4 --format lvgl --lv-include lvgl.h \
    --symbols "$SYMBOLS" \
    --no-compress -o src/fonts/font_xl_64.c

echo "[gen_fonts] done. Review the diff and commit src/fonts/font_xl_64.c."
