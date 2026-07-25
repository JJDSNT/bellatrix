#!/usr/bin/env bash
# scripts/flash.sh
#
# Copy the Bellatrix image to a mounted SD card.
#
# Usage:
#   ./scripts/flash.sh /media/user/BOOT
#   TFTP_HOST=192.168.1.10 ./scripts/flash.sh tftp

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
. "$SCRIPT_DIR/bellatrix-image.sh"

if [ $# -eq 0 ]; then
    echo "Usage: $0 <mount-point>  or  $0 tftp"
    exit 1
fi

CPU_BACKEND="${BELLATRIX_CPU_BACKEND:-emu68}"
MUSASHI_CPU="${BELLATRIX_MUSASHI_CPU:-68040}"
MULTICORE_BUILD="${BELLATRIX_MULTICORE_BUILD:-0}"
IMAGE_NAME="$(bellatrix_image_name "$CPU_BACKEND" "$MUSASHI_CPU" "$MULTICORE_BUILD")"
INSTALL="${BELLATRIX_INSTALL_DIR:-$ROOT/out/firmware}"
IMAGE="${BELLATRIX_IMAGE:-$ROOT/out/images/$IMAGE_NAME}"
CONFIG="$INSTALL/config.txt"

[ -f "$IMAGE" ] || { echo "ERROR: image not found — run scripts/build.sh first"; exit 1; }

if [ "$1" = "tftp" ]; then
    HOST="${TFTP_HOST:-192.168.1.10}"
    echo "Uploading via TFTP to $HOST..."
    tftp "$HOST" -m binary -c put "$IMAGE" kernel8.img
    echo "Done."
else
    MOUNT="$1"
    [ -d "$MOUNT" ] || { echo "ERROR: '$MOUNT' is not a directory"; exit 1; }
    echo "Copying to $MOUNT..."
    cp "$IMAGE" "$MOUNT/kernel8.img"
    [ -f "$CONFIG" ] && cp "$CONFIG" "$MOUNT/config.txt"
    sync
    echo "Done. Eject and boot."
fi
