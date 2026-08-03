#!/usr/bin/env bash
#
# Build a bootable SD card image for the AROS m68k-emu68 target.
#
#   ./scripts/make-sdcard.sh                      from the local AROS build
#   ./scripts/make-sdcard.sh --dist DIR           from another distribution tree
#   ./scripts/make-sdcard.sh --size 512M          bigger card
#   ./scripts/make-sdcard.sh --out path/to.img    elsewhere
#
# Layout: MBR, one bootable FAT32 (type 0c, LBA) partition starting at LBA 2048.
# partition.library's MBR handler reports type 0c as DOSType 0x46415402 ("FAT\2"),
# which is what fat-handler claims, so AROS ends up with SDCARD0P0:.
#
# QEMU's raspi3b wires -sd to the same Arasan controller that arch/m68k-emu68's
# soc/sdcard drives, so this image exercises the real driver rather than a
# stand-in.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/out/build/aros/bin/emu68-m68k/AROS"
OUT="$ROOT/out/aros/sd.img"
SIZE="256M"

while [ $# -gt 0 ]; do
    case "$1" in
        --dist) DIST="$2"; shift 2 ;;
        --out)  OUT="$2";  shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

for tool in sfdisk mformat mcopy truncate; do
    command -v "$tool" >/dev/null \
        || { echo "ERROR: $tool not found (sfdisk: util-linux, mformat/mcopy: mtools)" >&2; exit 1; }
done

[ -d "$DIST" ] || { echo "ERROR: distribution tree not found: $DIST" >&2; exit 1; }

# What goes on the card.
#
# Deliberately a list rather than the whole tree. Developer alone is ~291 MB of
# SDK that nothing in the boot path reads, and a card carrying it stalls the
# boot between AROSMonDrvs and "preparing console" — an open problem of its own,
# not worth walking into while bringing something else up.
DIRS=(C S Libs Devs L Classes Fonts System Prefs Storage Utilities Tools Locale)

# Locale is not optional: S:Startup-Sequence does Assign "LOCALE:" "SYS:Locale",
# and without it the boot console opens with "Can't find SYS:Locale" and every
# later LOCALE:-relative assign is built on sand.
missing=()
for d in "${DIRS[@]}"; do
    [ -d "$DIST/$d" ] || missing+=("$d")
done
[ -f "$DIST/AROS.boot" ] || missing+=("AROS.boot")

if [ "${#missing[@]}" -ne 0 ]; then
    echo "ERROR: $DIST is missing: ${missing[*]}" >&2
    echo "       The lean 'kernel-link-<target>' build only produces the ELF." >&2
    echo "       A bootable card needs the full distribution tree." >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"

echo "[sd] $SIZE image at $OUT"
truncate -s "$SIZE" "$OUT"

sfdisk -q "$OUT" >/dev/null <<'EOF'
label: dos
unit: sectors
start=2048, type=c, bootable
EOF

# @@1M is the mtools offset to the partition at LBA 2048.
SECTORS=$(( ($(stat -c%s "$OUT") / 512) - 2048 ))
mformat -i "$OUT@@1M" -F -v AROS -T "$SECTORS" ::

echo "[sd] copying distribution"
for d in "${DIRS[@]}"; do
    mcopy -i "$OUT@@1M" -s -Q "$DIST/$d" ::
done
mcopy -i "$OUT@@1M" -Q "$DIST/AROS.boot" ::

echo "[sd] $(basename "$OUT")  ($(stat -c%s "$OUT") bytes)"
mdir -i "$OUT@@1M" :: | tail -n +2
