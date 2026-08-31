#!/usr/bin/env bash
#
# Put "The evil Hannibals from Mars" on the boot volume, out of your own ADF.
#
#   tools/hannibals/install.sh <3ddemo2.adf>
#
# Why a script and not files in the repository: the demo is somebody else's
# work and is not ours to redistribute. This reads the image you already have
# and stages the extracted files under out/, which is git-ignored.
#
# Why this demo. It opens dos.library and nothing else -- no graphics, no
# intuition, no audio.device -- and its only absolute chipset references are
# INTENA and DMACON, which is the preamble of a program that turns the
# operating system off and drives the hardware itself. That makes it the one
# workload that needs no display driver on our side: it programs Denise, so
# there is no producer to supply and no ownership to arbitrate. Everything it
# does lands in Rigel.
#
# Its startup-sequence is three lines and starts it, so nothing has to be
# clicked:
#
#     type s:text
#     add21k
#     hannidemo2.EXE
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/out/hannibals"

[ $# -eq 1 ] || { echo "usage: $0 <3ddemo2.adf>" >&2; exit 2; }
ADF="$1"
[ -f "$ADF" ] || { echo "not a file: $ADF" >&2; exit 1; }

XDF=(env "PYTHONPATH=$ROOT/external/amitools" python3
     "$ROOT/external/amitools/bin/xdftool")

rm -rf "$OUT"
mkdir -p "$OUT"
"${XDF[@]}" "$ADF" unpack "$OUT" >/dev/null

# xdftool unpacks into a directory named after the volume, beside three
# metadata files describing the image itself. Only the volume's contents
# belong on the card.
inner="$(find "$OUT" -mindepth 1 -maxdepth 1 -type d | head -1)"
[ -n "$inner" ] || { echo "nothing unpacked from $ADF" >&2; exit 1; }
find "$OUT" -maxdepth 1 -type f -delete
mv "$inner"/* "$OUT"/ 2>/dev/null || true
mv "$inner"/.[!.]* "$OUT"/ 2>/dev/null || true
rmdir "$inner"

echo "[hannibals] $(find "$OUT" -type f | wc -l) files in $OUT"
