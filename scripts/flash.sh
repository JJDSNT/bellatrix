#!/usr/bin/env bash
# scripts/flash.sh
#
# Copy the complete Bellatrix U-Boot environment to a mounted SD card.
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

INSTALL="${BELLATRIX_INSTALL_DIR:-$ROOT/out/firmware}"
BOOT_DIR="$ROOT/out/boot"
IMAGES_DIR="$ROOT/out/images"

[ -f "$INSTALL/u-boot.bin" ] || { echo "ERROR: U-Boot not found — run scripts/build-all.sh first"; exit 1; }
[ -f "$BOOT_DIR/boot.scr" ] || { echo "ERROR: boot menu not found — run scripts/build-all.sh first"; exit 1; }

if [ "$1" = "tftp" ]; then
    CPU_BACKEND="${BELLATRIX_CPU_BACKEND:-emu68}"
    MUSASHI_CPU="${BELLATRIX_MUSASHI_CPU:-68040}"
    MULTICORE_BUILD="${BELLATRIX_MULTICORE_BUILD:-0}"
    IMAGE_NAME="$(bellatrix_image_name "$CPU_BACKEND" "$MUSASHI_CPU" "$MULTICORE_BUILD")"
    IMAGE="${BELLATRIX_IMAGE:-$IMAGES_DIR/$IMAGE_NAME}"
    HOST="${TFTP_HOST:-192.168.1.10}"
    echo "Uploading legacy single-image boot via TFTP to $HOST..."
    tftp "$HOST" -m binary -c put "$IMAGE" kernel8.img
    echo "Done."
else
    MOUNT="$1"
    [ -d "$MOUNT" ] || { echo "ERROR: '$MOUNT' is not a directory"; exit 1; }
    echo "Copying U-Boot environment to $MOUNT..."
    cp "$INSTALL"/* "$MOUNT/"
    mkdir -p "$MOUNT/images" "$MOUNT/roms"
    cp "$IMAGES_DIR"/*.img "$MOUNT/images/"
    cp "$BOOT_DIR/boot.scr" "$MOUNT/boot.scr"
    cp "$BOOT_DIR"/roms/* "$MOUNT/roms/"
    sync
    echo "Done. Eject and boot."
fi
