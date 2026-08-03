#!/usr/bin/env bash
#
# Set up, build and run, in one command. From a fresh clone:
#
#   ./run.sh
#
# Options:
#   --no-build      run whatever is already in out/images/
#   --clean         wipe the build directory first
#   --gui           open the QEMU window (default is headless, serial only)
#   -- <args...>    everything after -- goes to qemu-system-aarch64
#
# Serial goes to stdout; Ctrl-A X quits QEMU.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="$ROOT/out/images/Emu68.img"
DTB="$ROOT/out/firmware/bcm2710-rpi-3-b.dtb"

BUILD=1
CLEAN=""
DISPLAY_ARG="none"
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) BUILD=0 ;;
        --clean)    CLEAN="clean" ;;
        --gui)      DISPLAY_ARG="gtk" ;;
        --)         shift; EXTRA=("$@"); break ;;
        -h|--help)  sed -n '2,15p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

if [ "$BUILD" = 1 ]; then
    "$ROOT/scripts/build.sh" ${CLEAN:+$CLEAN}
fi

[ -f "$IMAGE" ] || { echo "ERROR: $IMAGE not found — run without --no-build" >&2; exit 1; }
[ -f "$DTB" ]   || { echo "ERROR: $DTB not found — run without --no-build" >&2; exit 1; }

command -v qemu-system-aarch64 >/dev/null \
    || { echo "ERROR: qemu-system-aarch64 not found" >&2; exit 1; }

# On raspi3b the first -serial is the PL011 that Emu68 logs to, and the second
# is the mini-UART. Getting the order wrong is silence, not an error.
echo "[run] qemu raspi3b — Ctrl-A X to quit"
exec qemu-system-aarch64 \
    -M raspi3b \
    -accel tcg,tb-size=64 \
    -kernel "$IMAGE" \
    -dtb "$DTB" \
    -serial stdio \
    -serial null \
    -display "$DISPLAY_ARG" \
    -append "${BOOTARGS:-enable_cache}" \
    "${EXTRA[@]}"
