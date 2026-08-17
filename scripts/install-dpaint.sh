#!/usr/bin/env bash
#
# Install Deluxe Paint from an Amiga floppy image into the built distribution,
# so make-sdcard.sh puts it on the card.
#
# The ADF is commercial software and is not part of this repository: give the
# path to your own copy. The disk that carries DPaint IV is labelled DPaintIV
# even when the file is named something else -- `xdftool <adf> list` tells you.
#
# What goes where, and why it is not a free choice:
#
#   Tools/DPaint, Tools/DPaint.info    make-sdcard.sh copies a fixed list of
#                                      drawers, and a new top-level one is
#                                      silently left off the card. Tools is on
#                                      the list and is where a Workbench user
#                                      looks for an application.
#   Fonts/dpaint.font, Fonts/dpaint/   diskfont.library finds a font by name in
#                                      FONTS:, nowhere else. DPaint draws its
#                                      own menus with this one.
#
# Nothing from the floppy's C:, L:, libs: or devs: is taken. That is AmigaOS
# 1.3 and this system has its own, newer, and mixing them is how you get a
# machine that boots into a version of itself from 1988.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${BELLATRIX_DIST:-$ROOT/out/build/aros/bin/emu68-m68k/AROS}"
AMITOOLS="${AMITOOLS:-$HOME/bellatrix-legacy/external/amitools}"
ADF="${1:-}"

if [ -z "$ADF" ]; then
    echo "usage: $(basename "$0") <dpaint.adf>" >&2
    echo "  the DPaint IV floppy; xdftool <adf> list shows what a disk holds" >&2
    exit 2
fi
[ -f "$ADF" ] || { echo "ERROR: no such file: $ADF" >&2; exit 1; }
[ -d "$DIST" ] || { echo "ERROR: no distribution at $DIST — build it first" >&2; exit 1; }
[ -f "$AMITOOLS/bin/xdftool" ] || {
    echo "ERROR: xdftool not found under $AMITOOLS" >&2
    echo "  set AMITOOLS to an amitools checkout" >&2
    exit 1
}

xdf() { ( cd "$AMITOOLS" && PYTHONPATH=. python3 bin/xdftool "$ADF" "$@" ); }

echo "[dpaint] reading $(basename "$ADF")"
mkdir -p "$DIST/Tools" "$DIST/Fonts/dpaint"

xdf read dpaint       "$DIST/Tools/DPaint"      >/dev/null
xdf read DPaint.info  "$DIST/Tools/DPaint.info" >/dev/null
chmod +x "$DIST/Tools/DPaint"

xdf read fonts/dpaint.font "$DIST/Fonts/dpaint.font" >/dev/null
for size in 5 8; do
    xdf read "fonts/dpaint/$size" "$DIST/Fonts/dpaint/$size" >/dev/null
done

echo "[dpaint] installed:"
ls -l "$DIST/Tools/DPaint" "$DIST/Tools/DPaint.info" "$DIST/Fonts/dpaint.font"
echo "[dpaint] now run scripts/make-sdcard.sh (add --pack for the Pi)"
