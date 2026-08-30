#!/usr/bin/env bash
#
# Put NewTek's Demo Reel 3 on the boot volume, out of your own ADFs.
#
#   tools/demoreel/install.sh "<disk 1.adf>" "<disk 2.adf>"
#
# Why this is a script and not files in the repository: Demo Reel 3 is
# commercial software from 1989 and is not ours to redistribute. This reads the
# images you already have and stages the extracted files under out/, which is
# git-ignored. Nothing it produces is ever committed.
#
# Why both disks go into one drawer, which looks wrong and is not: the demo's
# own ToRAM script does
#
#     if exists Monument
#     c:assign DemoReel3: ""
#     c:assign DemoReelData: ""
#     endif
#
# Monument is on disk 2, and "" is the current directory. So merged into one
# drawer, and run from it, the demo assigns both of its own volume names and
# needs no help from us. That is also why this does not invent assigns of its
# own: the disk knows where it lives, and a wrong guess here would be a bug
# that looks like the demo's.
#
# ISSUE-0068 phase 4. Nothing here needs Rigel to be fast; it needs the chipset
# display driver from phase 3 for the visuals to go through Denise at all.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AMITOOLS="$ROOT/external/amitools"
DEST="$ROOT/out/demoreel3"

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <Demo Reel 3 disk 1.adf> <Demo Reel 3 disk 2.adf>" >&2
    exit 2
fi

for adf in "$1" "$2"; do
    [ -f "$adf" ] || { echo "ERROR: no such ADF: $adf" >&2; exit 1; }
done

[ -d "$AMITOOLS/amitools" ] || {
    echo "ERROR: external/amitools is not initialised." >&2
    echo "       Run ./scripts/setup.sh first." >&2
    exit 1
}

rm -rf "$DEST"
mkdir -p "$DEST"

for adf in "$1" "$2"; do
    echo "[demoreel] unpacking $(basename "$adf")"
    # xdftool unpack writes a directory named after the volume; both volumes
    # are wanted in one place, so unpack aside and merge.
    tmp="$(mktemp -d)"
    ( cd "$AMITOOLS" && PYTHONPATH=. python3 bin/xdftool "$adf" unpack "$tmp" >/dev/null )
    # The volume directory is the only thing in tmp.
    vol="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -1)"
    [ -n "$vol" ] || { echo "ERROR: nothing unpacked from $adf" >&2; exit 1; }
    cp -a "$vol/." "$DEST/"
    rm -rf "$tmp"
done

# .info files carry the Amiga metadata xdftool writes beside each file; they are
# harmless on the card and are what makes the drawer visible in Wanderer.
count="$(find "$DEST" -type f | wc -l)"
echo "[demoreel] $count files in $DEST"

# Case matters here in the other direction: the disk spells it "toram" and
# "Monument", and AmigaDOS does not care while a Linux test does. Monument is
# the one ToRAM looks for before assigning, so its absence means the two disks
# did not both land.
missing=""
[ -f "$DEST/Slish" ] || missing="$missing Slish"
find "$DEST" -maxdepth 1 -iname "toram" | grep -q . || missing="$missing ToRAM"
find "$DEST" -maxdepth 1 -iname "monument" | grep -q . || missing="$missing Monument"
if [ -n "$missing" ]; then
    echo "WARNING: missing$missing -- are these the right two disks?" >&2
fi

cat <<'NOTE'
[demoreel] staged. To put it on the card:

    BELLATRIX_DEMOREEL=out/demoreel3 ./scripts/make-sdcard.sh

and on the machine:

    CD DemoReel3
    Execute ToRAM
    Slish

ToRAM assigns DemoReel3: and DemoReelData: itself, from the current
directory, and copies the tunes to RAM:. It also runs DoWeHaveMem, which
wants 1 MB.
NOTE
