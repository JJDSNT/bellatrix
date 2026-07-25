#!/usr/bin/env bash
# Generate the two-stage Bellatrix U-Boot menu and its ROM payload directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
MKIMAGE="${MKIMAGE:-$ROOT/out/build/u-boot/tools/mkimage}"
ROMS_DIR="${BELLATRIX_ROMS_DIR:-$ROOT/src/roms}"
BOOT_DIR="${BELLATRIX_BOOT_DIR:-$ROOT/out/boot}"
BOOT_CMD="$BOOT_DIR/boot.cmd"
BOOT_SCR="$BOOT_DIR/boot.scr"
BOOT_ROMS="$BOOT_DIR/roms"

[ -x "$MKIMAGE" ] || {
    echo "ERROR: mkimage not found: $MKIMAGE" >&2
    echo "Run scripts/build-uboot.sh first." >&2
    exit 1
}
[ -d "$ROMS_DIR" ] || {
    echo "ERROR: ROM directory not found: $ROMS_DIR" >&2
    exit 1
}

mkdir -p "$BOOT_DIR" "$BOOT_ROMS"
find "$BOOT_ROMS" -maxdepth 1 -type f -delete

rom_count=0
rom_entries=""
while IFS= read -r rom; do
    rom_name="$(basename "$rom")"
    case "$rom_name" in
        *"'"*|*"="*|*";"*)
            echo "WARNING: skipping ROM with unsupported menu characters: $rom_name" >&2
            continue
            ;;
    esac

    payload_name="rom-${rom_count}.rom"
    cp "$rom" "$BOOT_ROMS/$payload_name"
    rom_entries="${rom_entries}setenv bootmenu_${rom_count} '${rom_name}=setenv bellatrix_rom roms/${payload_name}; run boot_bellatrix'; "
    rom_count=$((rom_count + 1))
done < <(find "$ROMS_DIR" -maxdepth 1 -type f \
    \( -iname '*.rom' -o -iname '*.bin' \) -print | sort)

[ "$rom_count" -gt 0 ] || {
    echo "ERROR: no .rom or .bin files found in $ROMS_DIR" >&2
    exit 1
}

cat > "$BOOT_CMD" <<EOF
# Bellatrix two-stage boot menu. Generated; do not edit.
usb start
setenv kernel_addr_r 0x00080000
setenv fdt_addr_r 0x07000000
setenv ramdisk_addr_r 0x08000000

setenv boot_bellatrix 'fatload mmc 0:1 \${kernel_addr_r} images/\${bellatrix_kernel}; fatload mmc 0:1 \${ramdisk_addr_r} \${bellatrix_rom}; setenv bellatrix_rom_size \${filesize}; fatload mmc 0:1 \${fdt_addr_r} bcm2710-rpi-3-b.dtb; booti \${kernel_addr_r} \${ramdisk_addr_r}:\${bellatrix_rom_size} \${fdt_addr_r}'
setenv rom_menu "setenv bootmenu_0; setenv bootmenu_1; setenv bootmenu_2; setenv bootmenu_3; setenv bootmenu_4; setenv bootmenu_5; setenv bootmenu_6; setenv bootmenu_7; setenv bootmenu_8; setenv bootmenu_9; setenv bootmenu_10; setenv bootmenu_11; setenv bootmenu_12; setenv bootmenu_13; setenv bootmenu_14; setenv bootmenu_15; ${rom_entries}bootmenu -1"

setenv bootmenu_0 'Musashi 68000 (single-core)=setenv bellatrix_kernel bellatrix_musashi_68000.img; run rom_menu'
setenv bootmenu_1 'Musashi 68040 (single-core)=setenv bellatrix_kernel bellatrix_musashi_68040.img; run rom_menu'
setenv bootmenu_2 'Emu68 (single-core)=setenv bellatrix_kernel bellatrix_emu68.img; run rom_menu'
setenv bootmenu_3 'Musashi 68000 (multicore)=setenv bellatrix_kernel bellatrix_musashi_68000_multicore.img; run rom_menu'
setenv bootmenu_4 'Musashi 68040 (multicore)=setenv bellatrix_kernel bellatrix_musashi_68040_multicore.img; run rom_menu'
setenv bootmenu_5 'Emu68 (multicore)=setenv bellatrix_kernel bellatrix_emu68_multicore.img; run rom_menu'
setenv bootmenu_delay -1
bootmenu -1
EOF

"$MKIMAGE" -A arm64 -T script -C none -n "Bellatrix boot menu" \
    -d "$BOOT_CMD" "$BOOT_SCR"

echo "[U-BOOT] menu: $BOOT_SCR"
echo "[U-BOOT] ROMs: $rom_count ($BOOT_ROMS)"
