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

# The data file's name cannot exist on the card, so both sides of it move.
#
# The demo's second half is a 406 KB file called `Har vi røget hash?` -- a
# Danish joke, and 'LOAD' plus m68k code at its head, so it is program and not
# decoration. Hannidemo2.EXE opens it by that name: the string sits at offset
# 0x292 as `Har vi r\xf8get hash?`.
#
# Our boot volume is FAT32, because that is what the Pi's firmware reads, and
# FAT forbids `?` in a name. mtools drops the file without a word, the demo
# finds nothing and sits there -- which is exactly what happened, with the
# drawer looking correct in every listing except the card's own.
#
# So rename the file and patch the name the binary asks for, to the same
# thing. This edits somebody else's program, which is worth being explicit
# about: it is your copy, it stays under out/ which is git-ignored, nothing is
# redistributed, and it is the only way to put this demo on a volume whose
# filesystem cannot spell its data file. The replacement is shorter and
# NUL-padded, so nothing after it moves.
python3 - "$OUT" <<'PATCH'
import io, os, sys

out = sys.argv[1]
OLD = b"Har vi r\xf8get hash?\x00"
NEW = b"hannidata\x00" + b"\x00" * (len(OLD) - len(b"hannidata\x00"))

exe = os.path.join(out, "Hannidemo2.EXE")
d = io.open(exe, "rb").read()
n = d.count(OLD)
if n != 1:
    sys.exit("Hannidemo2.EXE: expected one copy of the data file's name, found %d" % n)
io.open(exe, "wb").write(d.replace(OLD, NEW))

for name in os.listdir(out):
    if name.startswith("Har vi r") and name.endswith("hash?"):
        os.rename(os.path.join(out, name), os.path.join(out, "hannidata"))
        break
else:
    sys.exit("the data file is not where it was expected")
print("[hannibals] data file renamed to 'hannidata', and the binary asks for it")
PATCH

echo "[hannibals] $(find "$OUT" -type f | wc -l) files in $OUT"
