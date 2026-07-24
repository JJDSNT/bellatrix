#!/usr/bin/env bash
# tests/qemu-headless.sh
#
# Build Bellatrix and run it in QEMU headless (no display, no TUI).
# Captures serial output for TIMEOUT seconds, then exits.
#
# Usage:
#   ./tests/qemu-headless.sh                     # build + run, 10s capture
#   TIMEOUT=30 ./tests/qemu-headless.sh          # longer capture
#   KICKSTART=src/roms/KS13.rom ./tests/qemu-headless.sh
#   BUILD=0 ./tests/qemu-headless.sh             # skip build, run only
#   BELLATRIX_CPU_BACKEND=musashi BUILD=0 \
#       KICKSTART=src/roms/KS13.rom ./tests/qemu-headless.sh
#
# Exit codes:
#   0  QEMU ran and guest progress was observed
#   1  build failed, QEMU did not start, or the guest made no progress

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CPU_BACKEND="${BELLATRIX_CPU_BACKEND:-emu68}"
case "$CPU_BACKEND" in
    emu68)   INSTALL="$ROOT/emu68/install-bellatrix-rigel" ;;
    musashi) INSTALL="$ROOT/emu68/install-bellatrix-rigel-musashi" ;;
    *) echo "ERROR: unsupported CPU backend: $CPU_BACKEND"; exit 1 ;;
esac
IMAGE="$INSTALL/Emu68.img"
DTB="$INSTALL/bcm2710-rpi-3-b.dtb"
TIMEOUT="${TIMEOUT:-10}"
BUILD="${BUILD:-1}"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [ "$BUILD" = "1" ]; then
    echo "[BUILD] Running setup + build..."
    "$ROOT/scripts/setup.sh"
    "$ROOT/scripts/build.sh"
fi

[ -f "$IMAGE" ] || { echo "ERROR: image not found: $IMAGE"; exit 1; }
[ -f "$DTB"   ] || { echo "ERROR: DTB not found: $DTB";   exit 1; }

# ---------------------------------------------------------------------------
# QEMU args
# ---------------------------------------------------------------------------
QEMU_ARGS=(
    -M raspi3b
    -kernel "$IMAGE"
    -dtb "$DTB"
    # PL011/UART0 is owned by Bluetooth from the first stage. QEMU exposes
    # AUX miniUART as the second serial device, which is Bellatrix's log.
    -serial null
    -serial stdio
    -display none
    -append "console=ttyS0"
)

if [ -n "${KICKSTART:-}" ]; then
    [ -f "$KICKSTART" ] || { echo "ERROR: Kickstart not found: $KICKSTART"; exit 1; }
    QEMU_ARGS+=(-initrd "$KICKSTART")
    echo "[RUN] Kickstart: $KICKSTART"
else
    echo "[RUN] No Kickstart (btrace-only mode)"
fi

echo "[RUN] QEMU headless — capturing serial for ${TIMEOUT}s..."
echo "[RUN] CPU backend: $CPU_BACKEND"
echo "---"

# A timeout is the expected way to stop QEMU. Preserve the serial stream so
# the smoke test can distinguish a slow TCG sample at PERF=0 from a genuinely
# static chipset timeline.
set +e
timeout "$TIMEOUT" qemu-system-aarch64 "${QEMU_ARGS[@]}" 2>&1 | tee "$LOG"
QEMU_STATUS="${PIPESTATUS[0]}"
set -e

echo "---"
if [ "$QEMU_STATUS" -ne 0 ] && [ "$QEMU_STATUS" -ne 124 ]; then
    echo "ERROR: QEMU exited with status $QEMU_STATUS"
    exit 1
fi

if ! grep -Eq '\[SC-PROGRESS\] frame=[1-9][0-9]*|\[RIGEL-FRAME\].*frame=[1-9][0-9]*|\[PERF\] realtime=[1-9][0-9]*%' "$LOG"; then
    echo "ERROR: no guest/chipset progress observed in ${TIMEOUT}s"
    exit 1
fi

echo "[DONE] ${TIMEOUT}s capture complete; guest progress observed."
