#!/usr/bin/env bash
# Build the pinned U-Boot submodule for Raspberry Pi 3B (AArch64).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
UBOOT="$ROOT/external/u-boot"
BUILD="$ROOT/out/build/u-boot"
FIRMWARE="$ROOT/out/firmware"

[ -f "$UBOOT/Makefile" ] || {
    echo "ERROR: U-Boot submodule is missing." >&2
    echo "Run: git submodule update --init external/u-boot" >&2
    exit 1
}

if [ "${1:-}" = "clean" ]; then
    rm -rf "$BUILD"
fi

mkdir -p "$BUILD" "$FIRMWARE"

if [ ! -f "$FIRMWARE/config.txt" ]; then
    cp "$ROOT/scripts/config.txt" "$FIRMWARE/config.txt"
fi

make -C "$UBOOT" O="$BUILD" rpi_3_defconfig
"$UBOOT/scripts/config" --file "$BUILD/.config" \
    --enable CONFIG_CMD_BOOTMENU \
    --enable CONFIG_CMD_BOOTI \
    --enable CONFIG_CMD_FAT \
    --enable CONFIG_CMD_SOURCE \
    --enable CONFIG_USE_BOOTCOMMAND \
    --set-str CONFIG_BOOTCOMMAND \
        'fatload mmc 0:1 ${scriptaddr} boot.scr; source ${scriptaddr}'
make -C "$UBOOT" O="$BUILD" olddefconfig
make -C "$UBOOT" O="$BUILD" CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)"

cp "$BUILD/u-boot.bin" "$FIRMWARE/u-boot.bin"
"$SCRIPT_DIR/generate-uboot-menu.sh"

# Raspberry Pi firmware starts U-Boot; U-Boot then selects Bellatrix + ROM.
sed -i '/^kernel=/d' "$FIRMWARE/config.txt"
printf '\nkernel=u-boot.bin\n' >> "$FIRMWARE/config.txt"

echo "[U-BOOT] binary: $FIRMWARE/u-boot.bin"
