#!/usr/bin/env bash
# scripts/make_sdcard_image.sh
#
# Build a bootable Raspberry Pi SD card image for Bellatrix/Emu68
# using a single FAT32 partition image.
#
# Output:
#   ./out/sdcard.img
#
# Usage:
#   ./scripts/make_sdcard_image.sh
#   SD_SIZE_MB=256 ./scripts/make_sdcard_image.sh
#   OUTPUT=./out/bellatrix-sd.img ./scripts/make_sdcard_image.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
OUT_DIR="$ROOT/out"

INSTALL="${BELLATRIX_INSTALL_DIR:-$ROOT/out/firmware}"
CONFIG_SRC="$INSTALL/config.txt"
BOOT_DIR="$ROOT/out/boot"
IMAGES_DIR="$ROOT/out/images"

OUTPUT="${OUTPUT:-$OUT_DIR/sdcard.img}"
SD_SIZE_MB="${SD_SIZE_MB:-128}"
VOLUME_LABEL="${VOLUME_LABEL:-BELLATRIX}"

TEMP_MOUNT=""
LOOP_DEV=""

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: required command not found: $1"
        exit 1
    }
}

cleanup() {
    set +e

    if [ -n "$TEMP_MOUNT" ] && mountpoint -q "$TEMP_MOUNT"; then
        sudo umount "$TEMP_MOUNT"
    fi

    if [ -n "$LOOP_DEV" ]; then
        sudo losetup -d "$LOOP_DEV" >/dev/null 2>&1 || true
    fi

    if [ -n "$TEMP_MOUNT" ] && [ -d "$TEMP_MOUNT" ]; then
        rmdir "$TEMP_MOUNT" >/dev/null 2>&1 || true
    fi
}

trap cleanup EXIT

require_cmd dd
require_cmd mkfs.vfat
require_cmd losetup
require_cmd mount
require_cmd umount
require_cmd mkdir
require_cmd cp
require_cmd sync

[ -f "$INSTALL/u-boot.bin" ] || {
    echo "ERROR: U-Boot not found: $INSTALL/u-boot.bin"
    echo "Run scripts/build-all.sh first."
    exit 1
}
[ -f "$BOOT_DIR/boot.scr" ] || { echo "ERROR: boot.scr not found"; exit 1; }

mkdir -p "$OUT_DIR"

echo "[SD] Creating blank image: $OUTPUT (${SD_SIZE_MB}MB)"
rm -f "$OUTPUT"
dd if=/dev/zero of="$OUTPUT" bs=1M count="$SD_SIZE_MB" status=progress

echo "[SD] Formatting image as FAT32"
mkfs.vfat -F 32 -n "$VOLUME_LABEL" "$OUTPUT"

TEMP_MOUNT="$(mktemp -d /tmp/bellatrix-sd-XXXXXX)"

echo "[SD] Attaching loop device"
LOOP_DEV="$(sudo losetup --find --show "$OUTPUT")"

echo "[SD] Mounting $LOOP_DEV at $TEMP_MOUNT"
sudo mount "$LOOP_DEV" "$TEMP_MOUNT"

echo "[SD] Copying firmware, U-Boot menu, kernels and ROMs"
sudo cp "$INSTALL"/* "$TEMP_MOUNT/"
sudo mkdir -p "$TEMP_MOUNT/images" "$TEMP_MOUNT/roms"
sudo cp "$IMAGES_DIR"/*.img "$TEMP_MOUNT/images/"
sudo cp "$BOOT_DIR/boot.scr" "$TEMP_MOUNT/boot.scr"
sudo cp "$BOOT_DIR"/roms/* "$TEMP_MOUNT/roms/"

if [ -f "$CONFIG_SRC" ]; then
    echo "[SD] Using shared config.txt"
    sudo cp "$CONFIG_SRC" "$TEMP_MOUNT/config.txt"
else
    echo "[SD] No config.txt found in install-bellatrix, generating minimal config"
    cat <<'EOF' | sudo tee "$TEMP_MOUNT/config.txt" >/dev/null
arm_64bit=1
enable_uart=1
kernel=u-boot.bin
disable_splash=1
EOF
fi

echo "[SD] Flushing writes"
sync

echo "[SD] Final image contents:"
ls -lah "$TEMP_MOUNT"

echo "[SD] Unmounting"
sudo umount "$TEMP_MOUNT"

echo "[SD] Detaching loop device"
sudo losetup -d "$LOOP_DEV"
LOOP_DEV=""

rmdir "$TEMP_MOUNT"
TEMP_MOUNT=""

echo
echo "Done."
echo "Image created at:"
echo "  $OUTPUT"
