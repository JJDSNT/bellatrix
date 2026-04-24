#!/usr/bin/env bash
# bellatrix/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS="$ROOT/scripts"

LAUNCHER_DIR="$ROOT/tools/launcher"
LAUNCHER_BIN="$LAUNCHER_DIR/bin/bellatrix-launcher"
ROMS_DIR="$ROOT/src/roms"

HARNESS_BUILD_DIR="$ROOT/out/harness"
HARNESS_BIN="$HARNESS_BUILD_DIR/harness"

usage() {
    cat <<EOF
Usage:
  ./run.sh [mode] [options]

Modes:
  qemu                Build and run in QEMU (default)
  harness             Build and run the Musashi validation harness (Linux host)
  raspi <mount>       Flash to SD card
  tftp                Upload via TFTP

QEMU options (env vars):
  EMU_PROFILE=<name>  Runtime profile: bellatrix or emu68 (default: bellatrix)
  KICKSTART=<file>    Pass a Kickstart ROM as initrd (optional)
  DISPLAY_MODE=<mode> QEMU display mode: gtk or none (default: gtk)
  BOOTARGS=<string>   Emu68 boot arguments (default: "enable_cache")
                      Key options: enable_cache (required for KS/bare-metal),
                      debug (JIT block stats), disassemble (M68k+ARM side-by-side)
  NO_TUI=1            Skip launcher TUI even if KICKSTART/DISPLAY_MODE are unset

Harness options (env vars):
  KICKSTART=<file>    ROM to run (required, or selected via TUI)
  FRAMES=<n>          Stop after N frames and exit (headless mode)
  CYCLES=<n>          Stop after N M68K cycles and exit (headless mode)
  (no FRAMES/CYCLES)  Interactive: SDL2 window, runs until closed or Esc

Examples:
  ./run.sh
  ./run.sh qemu
  ./run.sh harness
  KICKSTART=src/roms/DiagROM.rom ./run.sh harness
  KICKSTART=src/roms/aros.rom FRAMES=50 ./run.sh harness
  KICKSTART=src/roms/KS31.rom CYCLES=5000000 ./run.sh harness
  EMU_PROFILE=emu68 ./run.sh qemu
  EMU_PROFILE=bellatrix KICKSTART=src/roms/KS13.rom ./run.sh qemu
  DISPLAY_MODE=none ./run.sh qemu
  KICKSTART=src/roms/KS13.rom BOOTARGS="enable_cache debug" ./run.sh qemu
  KICKSTART=src/roms/KS13.rom BOOTARGS="enable_cache disassemble" ./run.sh qemu
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

build_harness() {
    mkdir -p "$HARNESS_BUILD_DIR"
    (
        cd "$HARNESS_BUILD_DIR"
        cmake "$ROOT/tools/harness" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF > /dev/null
        make -j"$(nproc)"
    )
}

set_profile_paths() {
    case "$1" in
        bellatrix)
            INSTALL="$ROOT/emu68/install-bellatrix"
            IMAGE="$INSTALL/Emu68.img"
            DTB="$INSTALL/bcm2710-rpi-3-b.dtb"
            BUILD_KIND="bellatrix"
            ;;
        emu68)
            INSTALL="$ROOT/emu68/build"
            IMAGE="$INSTALL/Emu68.img"
            DTB="$INSTALL/firmware/bcm2710-rpi-3-b.dtb"
            BUILD_KIND="emu68"
            ;;
        *)
            echo "ERROR: invalid EMU_PROFILE: $1"
            echo "Valid values: bellatrix, emu68"
            exit 1
            ;;
    esac
}

load_launcher_selection() {
    build_launcher

    local tmpfile
    tmpfile="$(mktemp)"
    trap 'rm -f "$tmpfile"' RETURN

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
            EMU_PROFILE)
                EMU_PROFILE="$value"
                ;;
            BOOTARGS)
                BOOTARGS="$value"
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
    qemu|raspi|tftp|harness)
        shift || true
        ;;
    *)
        echo "Unknown mode: $MODE"
        echo
        usage
        exit 1
        ;;
esac

# ---------------------------------------------------------------------------
# Harness mode — handled entirely here, no Emu68 build needed
# ---------------------------------------------------------------------------
if [ "$MODE" = "harness" ]; then
    echo "[BUILD] Harness (Musashi, native)"
    build_harness

    # Pick ROM via TUI if not set
    if [ -z "${KICKSTART:-}" ]; then
        load_launcher_selection
    fi

    [ -n "${KICKSTART:-}" ] || { echo "ERROR: no ROM selected"; exit 1; }
    [ -f "$KICKSTART" ]     || { echo "ERROR: ROM not found: $KICKSTART"; exit 1; }

    HARNESS_ARGS=("$KICKSTART")

    if [ -n "${CYCLES:-}" ]; then
        HARNESS_ARGS+=(--cycles "$CYCLES")
    elif [ -n "${FRAMES:-}" ]; then
        HARNESS_ARGS+=(--frames "$FRAMES")
    fi
    # Without FRAMES or CYCLES: interactive SDL2 window (default)

    echo "[RUN] Harness: $KICKSTART"
    exec "$HARNESS_BIN" "${HARNESS_ARGS[@]}"
fi

# ---------------------------------------------------------------------------
# QEMU / raspi / tftp modes — Emu68 build path
# ---------------------------------------------------------------------------
EMU_PROFILE="${EMU_PROFILE:-bellatrix}"
set_profile_paths "$EMU_PROFILE"

case "$MODE" in
    qemu)
        if [ "${NO_TUI:-0}" != "1" ] && { [ -z "${KICKSTART:-}" ] || [ -z "${DISPLAY_MODE:-}" ]; }; then
            load_launcher_selection
            EMU_PROFILE="${EMU_PROFILE:-bellatrix}"
            set_profile_paths "$EMU_PROFILE"
        fi
        ;;
esac

case "$BUILD_KIND" in
    bellatrix)
        echo "[BUILD] Profile: bellatrix"
        "$SCRIPTS/setup.sh"
        "$SCRIPTS/build.sh"
        ;;
    emu68)
        echo "[BUILD] Profile: emu68"
        echo "[BUILD] Using existing Emu68 build artifacts from: $INSTALL"
        ;;
esac

[ -f "$IMAGE" ] || { echo "ERROR: image not found: $IMAGE"; exit 1; }

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

        # BOOTARGS:
        #   enable_cache  — required for KS/bare-metal (enables JIT fast path via CACR_IE)
        #   debug         — print JIT block stats for every compiled M68k block
        #   disassemble   — print M68k + AArch64 side-by-side for each compiled block
        # Default: enable_cache only (minimal noise, JIT works correctly)
        QEMU_ARGS=(
            -M raspi3b
            -accel tcg,tb-size=64
            -kernel "$IMAGE"
            -dtb "$DTB"
            -serial stdio
            -display "$DISPLAY_ARG"
            -append "${BOOTARGS:-enable_cache}"
        )

        if [ -n "${KICKSTART:-}" ]; then
            [ -f "$KICKSTART" ] || { echo "ERROR: Kickstart not found: $KICKSTART"; exit 1; }
            QEMU_ARGS+=(-initrd "$KICKSTART")
            echo "[RUN] QEMU profile: $EMU_PROFILE"
            echo "[RUN] QEMU with Kickstart: $KICKSTART"
        else
            echo "[RUN] QEMU profile: $EMU_PROFILE"
            echo "[RUN] QEMU (no Kickstart — btrace-only mode)"
        fi

        echo "[RUN] Image: $IMAGE"
        echo "[RUN] DTB:   $DTB"
        echo "[RUN] Display mode: $DISPLAY_MODE"
        exec qemu-system-aarch64 "${QEMU_ARGS[@]}"
        ;;
    raspi)
        MOUNT="${1:-}"
        [ -n "$MOUNT" ] || { echo "Usage: ./run.sh raspi /media/user/BOOT"; exit 1; }

        if [ "$BUILD_KIND" != "bellatrix" ]; then
            echo "ERROR: raspi mode currently supports only EMU_PROFILE=bellatrix"
            exit 1
        fi

        "$SCRIPTS/flash.sh" "$MOUNT"
        echo "[DONE] SD ready."
        ;;
    tftp)
        if [ "$BUILD_KIND" != "bellatrix" ]; then
            echo "ERROR: tftp mode currently supports only EMU_PROFILE=bellatrix"
            exit 1
        fi

        "$SCRIPTS/flash.sh" tftp
        ;;
esac
