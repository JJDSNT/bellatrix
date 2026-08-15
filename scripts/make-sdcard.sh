#!/usr/bin/env bash
#
# Build a bootable SD card image for the AROS m68k-emu68 target.
#
#   ./scripts/make-sdcard.sh                      from the local AROS build
#   ./scripts/make-sdcard.sh --pi                 add the boot payload for real hardware
#   ./scripts/make-sdcard.sh --pack               ... and pack it as .tar.xz to hand out
#   ./scripts/make-sdcard.sh --dist DIR           from another distribution tree
#   ./scripts/make-sdcard.sh --size 512M          bigger card
#   ./scripts/make-sdcard.sh --out path/to.img    elsewhere
#
# Without --pi the image carries AROS alone, which is all QEMU needs: run.sh
# passes the kernel, the device tree and the m68k ELF on the command line. A
# Raspberry Pi has no command line, so --pi additionally writes the Broadcom
# firmware, Emu68, the AROS ELF and the two boot text files into the same
# partition, producing an image that boots a Pi 3 as written:
#
#   ./scripts/make-sdcard.sh --pi
#   sudo dd if=out/aros/bellatrix-pi3.img of=/dev/sdX bs=4M conv=fsync
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

# The default is this repository's own build, which requires
# `./scripts/build-aros.sh full` -- the lean build produces the ELF and the
# modules linked into it, and nothing else. Pointing --dist at a foreign
# distribution is supported and was the norm until 2026-08-07, but it means the
# card carries that tree's modules: every library, Zune class and C: command.
# A patch touching module code then changes nothing that boots, silently.
OUT=""
SIZE="256M"
PI=0
PACK=0

while [ $# -gt 0 ]; do
    case "$1" in
        --pi)   PI=1; shift ;;
        --pack) PI=1; PACK=1; shift ;;
        --dist) DIST="$2"; shift 2 ;;
        --out)  OUT="$2";  shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        -h|--help) sed -n '2,28p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

# Two different artefacts, so two different names: nothing distinguishes them
# by content once written, and flashing the QEMU card to a Pi produces a board
# that does nothing at all.
if [ -z "$OUT" ]; then
    if [ "$PI" = 1 ]; then
        OUT="$ROOT/out/aros/bellatrix-pi3.img"
    else
        OUT="$ROOT/out/aros/sd.img"
    fi
fi

FIRMWARE="$ROOT/out/firmware"

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

# The boot payload, for --pi only.
#
# bootcode.bin is the stage the Pi 3 loads out of the card by itself; start.elf
# and fixup.dat are the GPU firmware it then runs; the DTB is picked by the
# firmware from the board model, so every Pi 3 variant's tree is written and the
# board chooses. Emu68 goes in gzipped exactly as the build installs it -- the
# firmware decompresses a kernel image by its content, and config.txt below
# names it by that same name.
#
# The AROS ELF is taken from the distribution tree rather than out/aros so the
# kernel and the modules on the card always come from one build.
if [ "$PI" = 1 ]; then
    BOOT_FILES=("$FIRMWARE/bootcode.bin" "$FIRMWARE/start.elf" "$FIRMWARE/fixup.dat"
                "$FIRMWARE/Emu68.img.gz" "$DIST/aros-emu68-m68k.elf")
    for f in "$FIRMWARE"/bcm2710-*.dtb; do
        [ -e "$f" ] && BOOT_FILES+=("$f")
    done

    missing=()
    for f in "${BOOT_FILES[@]}"; do
        [ -f "$f" ] || missing+=("$(basename "$f")")
    done
    if [ "${#missing[@]}" -ne 0 ]; then
        echo "ERROR: boot payload incomplete: ${missing[*]}" >&2
        echo "       Run ./scripts/build.sh for the firmware and Emu68," >&2
        echo "       and ./scripts/build-aros.sh full for the AROS ELF." >&2
        exit 1
    fi
fi

mkdir -p "$(dirname "$OUT")"

if [ "$PACK" = 0 ]; then
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
fi

echo "[sd] collecting the card contents"

# mtools enforces DOS device-name rules that FAT itself does not: it refuses to
# create a file called AUX, CON, PRN, NUL, COM1-9 or LPT1-9, anywhere in the
# tree. AROS's FAT handler has no such rule, and the full distribution ships
# Devs/DOSDrivers/AUX -- the AUX: mountlist. Copying the tree straight in then
# fails on that one file with no message at all and a bare exit 1, which reads
# like the card being too small or the tree being missing.
#
# Stage through a hard-link farm, which copies no data, and drop those names
# there: the build tree is never modified, and what was left out is named
# rather than silently missing.
STAGE="$(dirname "$OUT")/.sdstage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
for d in "${DIRS[@]}"; do
    cp -al "$DIST/$d" "$STAGE/$d"
done
cp -al "$DIST/AROS.boot" "$STAGE/AROS.boot"

# Wanderer draws a drawer from its .info file, and that file sits *beside* the
# drawer rather than inside it. Copying only the directories therefore produces
# a boot volume whose window is empty: every drawer is present and none of them
# is visible. Only the ones whose drawer is actually on the card are taken --
# an .info without its drawer is an icon that opens onto nothing.
for d in "${DIRS[@]}"; do
    if [ -f "$DIST/$d.info" ]; then
        cp -al "$DIST/$d.info" "$STAGE/$d.info"
    fi
done

if [ "$PI" = 1 ]; then
    for f in "${BOOT_FILES[@]}"; do
        cp -al "$f" "$STAGE/$(basename "$f")"
    done

    # config.txt and cmdline.txt are written here rather than kept as files in
    # the repository: they name what this script just copied, and the two
    # drifting apart is a card that stops with no message worth reading.
    cat > "$STAGE/config.txt" <<'EOF'
# Bellatrix on a Raspberry Pi 3. Written by scripts/make-sdcard.sh --pi.

kernel=Emu68.img.gz
arm_64bit=1
initramfs aros-emu68-m68k.elf

disable_splash=1
avoid_warnings=1
gpu_mem=32

hdmi_group=2
hdmi_mode=82

# Serial console on GPIO 14/15. Bluetooth owns the PL011 on a Pi 3, so the
# console comes out of the mini-UART instead, and the mini-UART's baud rate
# follows the core clock -- pinning that clock is what keeps it readable.
enable_uart=1
core_freq=400
core_freq_min=400
EOF

    # One line, no trailing newline: the firmware hands this to Emu68 as the
    # boot arguments, and nocomposition is what puts AROS on the screen.
    printf 'nocomposition' > "$STAGE/cmdline.txt"
fi

# --pack hands out the card's contents instead of a card: the recipient formats
# their own FAT32 partition, at whatever size their card happens to be, and
# unpacks this into it. No image is written -- the size of the medium is theirs
# to decide.
#
# tar carries the DOS device names that the staging step keeps aside for mtools,
# since nothing in this path goes through mtools.
if [ "$PACK" = 1 ]; then
    ARCHIVE="$(dirname "$OUT")/bellatrix-pi3.tar.xz"
    echo "[sd] packing $(basename "$ARCHIVE")"
    tar -C "$STAGE" -cf - . | xz -T0 -9 > "$ARCHIVE"
    rm -rf "$STAGE"
    echo "[sd] $(basename "$ARCHIVE")  ($(stat -c%s "$ARCHIVE") bytes)"
    exit 0
fi

while IFS= read -r f; do
    [ -n "$f" ] || continue
    echo "[sd] skipping $f -- mtools will not write a DOS device name"
    rm -f "$STAGE/$f"
done < <(cd "$STAGE" && find . -type f | sed 's|^\./||' |
         grep -iE '(^|/)(AUX|CON|PRN|NUL|COM[1-9]|LPT[1-9])(\.[^/]*)?$' || true)

for d in "${DIRS[@]}"; do
    mcopy -i "$OUT@@1M" -s -Q "$STAGE/$d" ::
done
for f in "$STAGE"/*; do
    if [ -f "$f" ]; then
        mcopy -i "$OUT@@1M" -Q "$f" ::
    fi
done

rm -rf "$STAGE"

echo "[sd] $(basename "$OUT")  ($(stat -c%s "$OUT") bytes)"
mdir -i "$OUT@@1M" :: | tail -n +2

if [ "$PI" = 1 ]; then
    echo "[sd] write it with:  sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync"
fi
