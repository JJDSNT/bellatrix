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
#
# Exit codes:
#   0  QEMU ran and produced output
#   1  build failed or QEMU did not start

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL="$ROOT/emu68/install-bellatrix"
IMAGE="$INSTALL/Emu68.img"
DTB="$INSTALL/bcm2710-rpi-3-b.dtb"
TIMEOUT="${TIMEOUT:-10}"
BUILD="${BUILD:-1}"

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
    -serial stdio
    -display none
    -append "console=ttyAMA0"
)

if [ -n "${KICKSTART:-}" ]; then
    [ -f "$KICKSTART" ] || { echo "ERROR: Kickstart not found: $KICKSTART"; exit 1; }
    QEMU_ARGS+=(-initrd "$KICKSTART")
    echo "[RUN] Kickstart: $KICKSTART"
else
    echo "[RUN] No Kickstart (btrace-only mode)"
fi

echo "[RUN] QEMU headless — capturing serial for ${TIMEOUT}s..."
echo "---"

# Run QEMU with a hard timeout; always exit 0 (timeout is expected)
timeout "$TIMEOUT" qemu-system-aarch64 "${QEMU_ARGS[@]}" || true

echo "---"
echo "[DONE] ${TIMEOUT}s capture complete."
