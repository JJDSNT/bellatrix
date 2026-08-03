#!/usr/bin/env bash
#
# Boot AROS/m68k under Emu68 in QEMU.
#
#   ./run-aros.sh                 headless, serial on stdout
#   ./run-aros.sh --gui           framebuffer in a window, serial still on stdout
#   ./run-aros.sh --no-sd         no SD card (boots to the Emu68 logo and stops)
#   ./run-aros.sh --sd path.img   another card
#   ./run-aros.sh --debug X       adds sysdebug=X to the kernel arguments
#   ./run-aros.sh -- <args...>    everything after -- goes to qemu
#
# QEMU emulates the Raspberry Pi; Emu68 is the bare-metal owner and loads the
# m68k ELF from -initrd. Ctrl-A X quits.
#
# Pieces, and what builds them:
#   out/images/Emu68.img          scripts/build.sh
#   out/firmware/bcm2710-...dtb   scripts/build.sh (downloaded by Emu68's cmake)
#   out/aros/aros-emu68-m68k.elf  scripts/build-aros.sh
#   out/aros/sd.img               scripts/make-sdcard.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL="$ROOT/out/images/Emu68.img"
DTB="$ROOT/out/firmware/bcm2710-rpi-3-b.dtb"
INITRD="$ROOT/out/aros/aros-emu68-m68k.elf"
SD="$ROOT/out/aros/sd.img"
MONITOR="${BELLATRIX_QEMU_MONITOR:-/tmp/emu68-monitor.sock}"

DISPLAY_ARG="none"
USE_SD=1
DEBUG=""
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --gui)    DISPLAY_ARG="gtk" ;;
        --no-sd)  USE_SD=0 ;;
        --sd)     SD="$2"; shift ;;
        --debug)  DEBUG="$2"; shift ;;
        --)       shift; EXTRA=("$@"); break ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

for f in "$KERNEL:scripts/build.sh" "$DTB:scripts/build.sh" "$INITRD:scripts/build-aros.sh"; do
    p="${f%%:*}"; how="${f##*:}"
    [ -f "$p" ] || { echo "ERROR: missing $p — run $how" >&2; exit 1; }
done

# nocomposition is currently required to see anything on the framebuffer.
# Without it DEVS:Monitors/Compositor installs successfully and then nothing
# reaches the display: the boot runs to completion, Wanderer loads
# muimaster.library and its Zune icon classes, and the screen stays on the
# Emu68 logo. emu68gfx is presumably missing something the software compositor
# expects of a driver it has taken over.
BOOTARGS="${BOOTARGS:-nocomposition}"
[ -n "$DEBUG" ] && BOOTARGS="$BOOTARGS sysdebug=$DEBUG"

QEMU=(
    qemu-system-aarch64
    -M raspi3b
    -kernel "$KERNEL"
    -dtb "$DTB"
    -initrd "$INITRD"
    -append "$BOOTARGS"
    -serial stdio
    -display "$DISPLAY_ARG"
    -no-reboot
    -monitor "unix:$MONITOR,server,nowait"
)

if [ "$USE_SD" = 1 ]; then
    [ -f "$SD" ] || { echo "ERROR: missing $SD — run scripts/make-sdcard.sh" >&2; exit 1; }
    QEMU+=(-drive "file=$SD,if=sd,format=raw")
fi

echo "[run] $BOOTARGS${USE_SD:+ | sd: $(basename "$SD")} | monitor: $MONITOR"
echo "[run] Ctrl-A X to quit; 'nc -U $MONITOR' for the QEMU monitor"
exec "${QEMU[@]}" "${EXTRA[@]}"
