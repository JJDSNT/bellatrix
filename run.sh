#!/usr/bin/env bash
# bellatrix/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS="$ROOT/scripts"
INSTALL="$ROOT/emu68/install-bellatrix"
IMAGE="$INSTALL/Emu68.img"
DTB="$INSTALL/bcm2710-rpi-3-b.dtb"

LAUNCHER_DIR="$ROOT/tools/launcher"
LAUNCHER_BIN="$LAUNCHER_DIR/bin/bellatrix-launcher"
ROMS_DIR="$ROOT/src/roms"

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
  DISPLAY_MODE=<mode> QEMU display mode: gtk or none (default: gtk)
  BOOTARGS=<string>   Extra boot arguments (default: "console=ttyAMA0")
  NO_TUI=1            Skip launcher TUI even if KICKSTART/DISPLAY_MODE are unset

Examples:
  ./run.sh
  ./run.sh qemu
  KICKSTART=src/roms/KS13.rom ./run.sh qemu
  DISPLAY_MODE=none ./run.sh qemu
  ./run.sh raspi /media/user/BOOT
  ./run.sh tftp
EOF
}

build_launcher() {
    mkdir -p "$LAUNCHER_DIR/bin"
    (
        cd "$LAUNCHER_DIR"
        go mod download
        go build -o "$LAUNCHER_BIN" .
    )
}

load_launcher_selection() {
    build_launcher

    local tmpfile
    tmpfile="$(mktemp)"
    trap 'rm -f "$tmpfile"' RETURN

    # TUI ligada diretamente ao terminal.
    "$LAUNCHER_BIN" "$ROMS_DIR" "$tmpfile" < /dev/tty > /dev/tty 2> /dev/tty

    [ -f "$tmpfile" ] || {
        echo "ERROR: launcher did not produce output"
        exit 1
    }

    while IFS='=' read -r key value; do
        case "$key" in
            KICKSTART)
                KICKSTART="$value"
                ;;
            DISPLAY_MODE)
                DISPLAY_MODE="$value"
                ;;
        esac
    done < "$tmpfile"

    rm -f "$tmpfile"
    trap - RETURN
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

case "$MODE" in
    qemu)
        if [ "${NO_TUI:-0}" != "1" ] && { [ -z "${KICKSTART:-}" ] || [ -z "${DISPLAY_MODE:-}" ]; }; then
            load_launcher_selection
        fi
        ;;
esac

"$SCRIPTS/setup.sh"
"$SCRIPTS/build.sh"

[ -f "$IMAGE" ] || { echo "ERROR: image not found"; exit 1; }

case "$MODE" in
    qemu)
        [ -f "$DTB" ] || { echo "ERROR: DTB not found at $DTB"; exit 1; }

        DISPLAY_MODE="${DISPLAY_MODE:-gtk}"

        case "$DISPLAY_MODE" in
            gtk)
                DISPLAY_ARG="gtk,zoom-to-fit=on,window-close=on"
                ;;
            none)
                DISPLAY_ARG="none"
                ;;
            *)
                echo "ERROR: invalid DISPLAY_MODE: $DISPLAY_MODE"
                echo "Valid values: gtk, none"
                exit 1
                ;;
        esac

        QEMU_ARGS=(
            -M raspi3b
            -kernel "$IMAGE"
            -dtb "$DTB"
            -serial stdio
            -display "$DISPLAY_ARG"
            -append "${BOOTARGS:-console=ttyAMA0}"
        )

        if [ -n "${KICKSTART:-}" ]; then
            [ -f "$KICKSTART" ] || { echo "ERROR: Kickstart not found: $KICKSTART"; exit 1; }
            QEMU_ARGS+=(-initrd "$KICKSTART")
            echo "[RUN] QEMU with Kickstart: $KICKSTART"
        else
            echo "[RUN] QEMU (no Kickstart — btrace-only mode)"
        fi

        echo "[RUN] Display mode: $DISPLAY_MODE"
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