#!/usr/bin/env bash
#
# run.sh (root)
#
# Usage:
#   ./run.sh qemu
#   ./run.sh raspi /media/user/BOOT
#   ./run.sh tftp

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS="$ROOT/scripts"
INSTALL="$ROOT/emu68/install-bellatrix"
IMAGE="$INSTALL/Emu68.img"

MODE="${1:-qemu}"

# --- setup + build ---
"$SCRIPTS/setup.sh"
"$SCRIPTS/build.sh"

[ -f "$IMAGE" ] || { echo "ERROR: image not found"; exit 1; }

# --- run modes ---
if [ "$MODE" = "qemu" ]; then
    echo "[RUN] QEMU (raspi3b)"

    qemu-system-aarch64 \
        -M raspi3b \
        -serial stdio \
        -kernel "$IMAGE"

elif [ "$MODE" = "raspi" ]; then
    MOUNT="${2:-}"

    [ -n "$MOUNT" ] || { echo "Usage: $0 raspi /media/user/BOOT"; exit 1; }

    "$SCRIPTS/flash.sh" "$MOUNT"

    echo "[DONE] SD ready. Insert into Raspberry Pi."

elif [ "$MODE" = "tftp" ]; then
    "$SCRIPTS/flash.sh" tftp

else
    echo "Usage:"
    echo "  ./run.sh qemu"
    echo "  ./run.sh raspi /media/user/BOOT"
    echo "  ./run.sh tftp"
    exit 1
fi