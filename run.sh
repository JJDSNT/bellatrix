#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS="$ROOT/scripts"
INSTALL="$ROOT/emu68/install-bellatrix"
IMAGE="$INSTALL/Emu68.img"
DTB="$INSTALL/bcm2710-rpi-3-b.dtb"

usage() {
    cat <<EOF
Usage:
  ./run.sh [mode] [options]

Modes:
  qemu                Build and run in QEMU (default)
  raspi <mount>       Flash to SD card
  tftp                Upload via TFTP

QEMU options (env vars):
  KICKSTART=<file>    Pass a Kickstart ROM as initrd (optional)
  BOOTARGS=<string>   Extra boot arguments (default: "console=ttyAMA0")

Examples:
  ./run.sh
  ./run.sh qemu
  KICKSTART=kickstart.rom ./run.sh qemu
  ./run.sh raspi /media/user/BOOT
  ./run.sh tftp
EOF
}

MODE="${1:-qemu}"

case "$MODE" in
    -h|--help)
        usage
        exit 0
        ;;
    qemu|raspi|tftp)
        shift || true
        ;;
    *)
        echo "Unknown mode: $MODE"
        echo
        usage
        exit 1
        ;;
esac

"$SCRIPTS/setup.sh"
"$SCRIPTS/build.sh"

[ -f "$IMAGE" ] || { echo "ERROR: image not found"; exit 1; }

case "$MODE" in
    qemu)
        [ -f "$DTB" ] || { echo "ERROR: DTB not found at $DTB"; exit 1; }

        QEMU_ARGS=(
            -M raspi3b
            -kernel "$IMAGE"
            -dtb "$DTB"
            -serial stdio
            -display none
            -append "${BOOTARGS:-console=ttyAMA0}"
        )

        # Pass Kickstart ROM as initrd if provided
        if [ -n "${KICKSTART:-}" ]; then
            [ -f "$KICKSTART" ] || { echo "ERROR: Kickstart not found: $KICKSTART"; exit 1; }
            QEMU_ARGS+=(-initrd "$KICKSTART")
            echo "[RUN] QEMU with Kickstart: $KICKSTART"
        else
            echo "[RUN] QEMU (no Kickstart — btrace-only mode)"
        fi

        exec qemu-system-aarch64 "${QEMU_ARGS[@]}"
        ;;
    raspi)
        MOUNT="${1:-}"
        [ -n "$MOUNT" ] || { echo "Usage: ./run.sh raspi /media/user/BOOT"; exit 1; }
        "$SCRIPTS/flash.sh" "$MOUNT"
        echo "[DONE] SD ready."
        ;;
    tftp)
        "$SCRIPTS/flash.sh" tftp
        ;;
esac
