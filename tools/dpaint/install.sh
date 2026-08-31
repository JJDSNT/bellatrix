#!/usr/bin/env bash
#
# Put Deluxe Paint IV on the boot volume, out of your own ADF.
#
#   tools/dpaint/install.sh <wb13.adf>
#
# The disk is a DPaintIV boot floppy (Workbench 1.3 plus the application).
# Only the application and what it carries with it go on the card: the
# Workbench half is already there, and a second copy of C: and LIBS: would be
# a second set of commands nobody asked for.
#
# Why this workload. It is an ordinary AmigaOS application, well-behaved and
# extremely well-exercised: it opens a custom screen through intuition and
# graphics.library and leans hard on the blitter. That is the OS path, which
# is what most Amiga software actually does -- and the half of the machine a
# demo that turns the OS off never touches.
#
# What it does and does not test today, stated plainly: without
# DEVS:Monitors/AmigaVideo there is no classic display driver, so
# graphics.library renders to the VideoCore through vcgfx and Rigel's Denise
# is handed nothing. So this run exercises AROS and the machine, not the
# chipset. The chipset half needs ISSUE-0081 closed first.
#
# The demo is somebody else's work and is not ours to redistribute; this reads
# the image you already have and stages under out/, which is git-ignored.
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/out/dpaint"

[ $# -eq 1 ] || { echo "usage: $0 <wb13.adf>" >&2; exit 2; }
ADF="$1"
[ -f "$ADF" ] || { echo "not a file: $ADF" >&2; exit 1; }

XDF=(env "PYTHONPATH=$ROOT/external/amitools" python3
     "$ROOT/external/amitools/bin/xdftool")

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"${XDF[@]}" "$ADF" unpack "$TMP" >/dev/null

vol="$(find "$TMP" -mindepth 1 -maxdepth 1 -type d | head -1)"
[ -n "$vol" ] || { echo "nothing unpacked from $ADF" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
# The application, its icon so Wanderer shows it, and the fonts it draws its
# own menus with. Everything else on that floppy is Workbench 1.3.
for f in dpaint DPaint.info Disk.info fonts; do
    [ -e "$vol/$f" ] && cp -a "$vol/$f" "$OUT/"
done

echo "[dpaint] $(find "$OUT" -type f | wc -l) files in $OUT"
find "$OUT" -maxdepth 1 -printf '  %P\n' | sed '/^  $/d'
