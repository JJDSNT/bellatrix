#!/usr/bin/env bash
#
# Boot under QEMU. Does not build unless asked.
#
#   ./run.sh                boot whatever is built — AROS if its ELF exists,
#                           otherwise Emu68 on its own
#   ./run.sh --no-aros      Emu68 alone, even if the AROS ELF is there
#   ./run.sh --no-sd        AROS without an SD card
#   ./run.sh --sd PATH      a different card
#   ./run.sh --headless     serial only, no window
#   ./run.sh --gui          force a window even with no display detected
#                           GUI boots include a USB tablet
#   ./run.sh --serial FILE  send the serial console to a file instead of stdio
#   ./run.sh --debug FLAGS  adds sysdebug=FLAGS to the kernel arguments
#   ./run.sh --build        build Emu68 first (not done by default)
#   ./run.sh --clean        rebuild Emu68 from scratch first (implies --build)
#   ./run.sh --no-build     accepted and ignored; building is already off
#   ./run.sh -- <args...>   everything after -- goes to qemu
#
# BELLATRIX_RIGEL=0/1 forces the classic chipset off or on for this boot; the
# default is whatever the last build put in the image. BOOTARGS replaces the
# kernel command line outright.
#
# QEMU emulates the Raspberry Pi. Emu68 is the bare-metal owner; when AROS is
# in play, Emu68 loads its m68k ELF from -initrd. Ctrl-A X quits.
#
# Pieces, and what builds them:
#   out/images/{Bellatrix,Emu68}.img scripts/build.sh   (--build, or run it)
#   out/firmware/bcm2710-*.dtb    scripts/build.sh
#   out/aros/aros-emu68-m68k.elf  scripts/build-aros.sh (never built here — it
#                                 also builds a cross toolchain)
#   out/aros/sd.img               scripts/make-sdcard.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_NAME="$(cat "$ROOT/out/images/Emu68.kernel-name" 2>/dev/null || echo Emu68.img)"
KERNEL="$ROOT/out/images/$KERNEL_NAME"
DTB="$ROOT/out/firmware/bcm2710-rpi-3-b.dtb"
INITRD="$ROOT/out/aros/aros-emu68-m68k.elf"
SD="$ROOT/out/aros/sd.img"
RIGEL_STAMP="$ROOT/out/images/Emu68.config-rigel"
MONITOR="${BELLATRIX_QEMU_MONITOR:-/tmp/emu68-monitor.sock}"

# Off by default. Booting and building are different intentions, and a run
# that silently rebuilds takes an unpredictable amount of time -- which matters
# when the thing being measured is how long a boot takes.
BUILD=0
CLEAN=""
# A window by default when there is a display to put it on — AROS is a
# graphical system and the framebuffer is usually the point. Falls back to
# serial-only automatically, so this still works over SSH or in CI.
if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    DISPLAY_ARG="gtk"
else
    DISPLAY_ARG="none"
fi
WANT_AROS="auto"
USE_SD=1
DEBUG=""
# Where the AUX mini-UART console goes. stdio for a human; a file for anything
# unattended, which is what scripts/boot-timing.py uses so it can read the log
# while the run is still going. PL011 remains disconnected here for Bluetooth.
SERIAL="stdio"
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --build)    BUILD=1 ;;
        --no-build) BUILD=0 ;;
        --clean)    CLEAN="clean"; BUILD=1 ;;
        --gui)      DISPLAY_ARG="gtk" ;;
        --headless) DISPLAY_ARG="none" ;;
        --serial)   SERIAL="file:$2"; shift ;;
        --no-aros)  WANT_AROS="no" ;;
        --no-sd)    USE_SD=0 ;;
        --sd)       SD="$2"; shift ;;
        --debug)    DEBUG="$2"; shift ;;
        --)         shift; EXTRA=("$@"); break ;;
        -h|--help)  sed -n '2,27p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

[ "$BUILD" = 1 ] && "$ROOT/scripts/build.sh" ${CLEAN:+$CLEAN}

for f in "$KERNEL" "$DTB"; do
    [ -f "$f" ] || { echo "ERROR: missing $f — run scripts/build.sh" >&2; exit 1; }
done

command -v qemu-system-aarch64 >/dev/null \
    || { echo "ERROR: qemu-system-aarch64 not found" >&2; exit 1; }

case "$WANT_AROS" in
    no)   AROS=0 ;;
    auto) [ -f "$INITRD" ] && AROS=1 || AROS=0 ;;
esac

QEMU=(
    qemu-system-aarch64
    -M raspi3b
    -accel tcg,tb-size=64
    -kernel "$KERNEL"
    -dtb "$DTB"
    # QEMU raspi3b exposes PL011 first and AUX mini-UART second. Bellatrix logs
    # only through the latter; PL011 is deliberately reserved for Bluetooth.
    -serial null
    -serial "$SERIAL"
    -display "$DISPLAY_ARG"
    -no-reboot
    -monitor "unix:$MONITOR,server,nowait"
)

# The raspi3b machine provides the DWC2 USB host controller, but QEMU does not
# populate it with a pointing device automatically.  Use an absolute USB HID
# tablet for graphical runs: it does not depend on frontend pointer capture,
# and QEMU's SDL relative usb-mouse path reverses both motion axes here.
if [ "$DISPLAY_ARG" != "none" ]; then
    QEMU+=(-device usb-tablet)
fi

if [ "$AROS" = 1 ]; then
    # nocomposition is currently required to see anything on the framebuffer.
    # Without it DEVS:Monitors/Compositor installs successfully and then nothing
    # reaches the display: the boot runs to completion, Wanderer loads
    # muimaster.library and its Zune icon classes, and the screen stays on the
    # Emu68 logo.
    BOOTARGS="${BOOTARGS:-nocomposition}"
    # The chipset is a boot argument now, on both sides of the boundary: `rigel`
    # tells Emu68 to build the classic machine and tells AROS to expect it. The
    # default follows the last build -- an image with no chipset in it has
    # nothing to enable -- and BELLATRIX_RIGEL=0/1 overrides that, which is how
    # one image gets booted both ways without rebuilding.
    if [ "${BELLATRIX_RIGEL:-$(cat "$RIGEL_STAMP" 2>/dev/null || echo 0)}" = 1 ]; then
        BOOTARGS="$BOOTARGS rigel"
    fi
    QEMU+=(-initrd "$INITRD")
    if [ "$USE_SD" = 1 ]; then
        [ -f "$SD" ] || { echo "ERROR: missing $SD — run scripts/make-sdcard.sh" >&2; exit 1; }
        QEMU+=(-drive "file=$SD,if=sd,format=raw")
    fi
    WHAT="Emu68 + AROS"
    [ "$USE_SD" = 1 ] && WHAT="$WHAT + $(basename "$SD")"
else
    BOOTARGS="${BOOTARGS:-enable_cache}"
    WHAT="Emu68 alone"
    [ -f "$INITRD" ] || echo "[run] no AROS ELF — run scripts/build-aros.sh to boot it too"
fi

[ -n "$DEBUG" ] && BOOTARGS="$BOOTARGS sysdebug=$DEBUG"
QEMU+=(-append "$BOOTARGS")

echo "[run] $WHAT | display: $DISPLAY_ARG | append: $BOOTARGS"
echo "[run] Ctrl-A X to quit; 'nc -U $MONITOR' for the QEMU monitor"
exec "${QEMU[@]}" "${EXTRA[@]}"
