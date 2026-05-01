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
  harness-serial      Run harness with DiagROM serial PTY and open screen
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
  ADF=<file>          Mount ADF image as DF0 (optional)
  FRAMES=<n>          Stop after N frames and exit (headless mode)
  CYCLES=<n>          Stop after N M68K cycles and exit (headless mode)
  HARNESS_SERIAL_MODE=<mode>
                      Serial presentation: line, raw, ansi, pty (default: line)
  HARNESS_SERIAL_NOWAIT=1
                      In pty mode, do not wait for Enter before boot
  HARNESS_SERIAL_BOOTKEY=<char>
                      Inject one early RX byte into Paula UART (default frame 5)
  HARNESS_SERIAL_BOOTKEY_FRAME=<n>
                      Frame to inject HARNESS_SERIAL_BOOTKEY (default: 5)
  HARNESS_SERIAL_INJECT=<char>
                      Inject one later RX byte into Paula UART (for example: 4)
  HARNESS_SERIAL_INJECT_FRAME=<n>
                      Frame to inject HARNESS_SERIAL_INJECT (default: 300)
  HARNESS_SERIAL_SCRIPT=<frame:char,...>
                      Inject a serial sequence, for example: 900:0x20,1050:4
  HARNESS_SERIAL_HOLD=<char>
                      Repeat one serial byte across multiple frames
  HARNESS_SERIAL_HOLD_FRAME=<n>
                      First frame for HARNESS_SERIAL_HOLD
  HARNESS_SERIAL_HOLD_COUNT=<n>
                      Number of repeated injections for HARNESS_SERIAL_HOLD
  HARNESS_SERIAL_HOLD_STEP=<n>
                      Frame delta between repeated injections (default: 1)
  HARNESS_MOUSE_RMB_FRAME=<n>
                      First frame to hold the Amiga right mouse button down
  HARNESS_MOUSE_RMB_COUNT=<n>
                      Number of frames to keep the Amiga right mouse button down
  HARNESS_MOUSE_RMB_PORT=<0|1>
                      Controller port for RMB hold (default: 0)
  HARNESS_SERIAL_AFTER_RMB=<char>
                      After a real SDL right-click, inject this serial byte
  HARNESS_SERIAL_AFTER_RMB_DELAY=<n>
                      Frames to wait after the SDL right-click before injection
  HARNESS_SERIAL_AFTER_RMB_PORT=<0|1>
                      Mouse port that arms HARNESS_SERIAL_AFTER_RMB (default: 0)
  (no FRAMES/CYCLES)  Interactive: SDL2 window, runs until closed or Esc

Examples:
  ./run.sh
  ./run.sh qemu
  ./run.sh harness
  KICKSTART=src/roms/DiagROM.rom ./run.sh harness-serial
  HARNESS_SERIAL_BOOTKEY=' ' HARNESS_SERIAL_INJECT=4 ./run.sh harness
  HARNESS_SERIAL_SCRIPT="900:0x20,1050:4" KICKSTART=src/roms/DiagROM.rom ./run.sh harness
  HARNESS_SERIAL_HOLD=0x20 HARNESS_SERIAL_HOLD_FRAME=900 HARNESS_SERIAL_HOLD_COUNT=40 HARNESS_SERIAL_INJECT=4 HARNESS_SERIAL_INJECT_FRAME=1050 KICKSTART=src/roms/DiagROM.rom ./run.sh harness
  HARNESS_MOUSE_RMB_FRAME=900 HARNESS_MOUSE_RMB_COUNT=60 HARNESS_SERIAL_INJECT=4 HARNESS_SERIAL_INJECT_FRAME=1050 KICKSTART=src/roms/DiagROM.rom ./run.sh harness
  HARNESS_SERIAL_AFTER_RMB=4 HARNESS_SERIAL_AFTER_RMB_DELAY=80 KICKSTART=src/roms/DiagROM.rom ./run.sh harness
  KICKSTART=src/roms/DiagROM.rom ./run.sh harness
  KICKSTART=src/roms/aros.rom FRAMES=50 ./run.sh harness
  KICKSTART=src/roms/KS31.rom CYCLES=5000000 ./run.sh harness
  KICKSTART=src/roms/KS13.rom ADF=disks/Workbench13.adf ./run.sh harness
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
    qemu|raspi|tftp|harness|harness-serial)
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

    if [ -n "${ADF:-}" ]; then
        [ -f "$ADF" ] || { echo "ERROR: ADF not found: $ADF"; exit 1; }
        HARNESS_ARGS+=(--adf "$ADF")
    fi

    if [ -n "${CYCLES:-}" ]; then
        HARNESS_ARGS+=(--cycles "$CYCLES")
    elif [ -n "${FRAMES:-}" ]; then
        HARNESS_ARGS+=(--frames "$FRAMES")
    fi
    # Without FRAMES or CYCLES: interactive SDL2 window (default)

    echo "[RUN] Harness ROM: $KICKSTART"
    if [ -n "${ADF:-}" ]; then
        echo "[RUN] Harness DF0: $ADF"
    else
        echo "[RUN] Harness DF0: no disk"
    fi

    exec "$HARNESS_BIN" "${HARNESS_ARGS[@]}"
fi

if [ "$MODE" = "harness-serial" ]; then
    echo "[BUILD] Harness (Musashi, native)"
    build_harness

    if [ -z "${KICKSTART:-}" ]; then
        load_launcher_selection
    fi

    [ -n "${KICKSTART:-}" ] || { echo "ERROR: no ROM selected"; exit 1; }
    [ -f "$KICKSTART" ]     || { echo "ERROR: ROM not found: $KICKSTART"; exit 1; }
    HARNESS_ARGS=("$KICKSTART")

    if [ -n "${ADF:-}" ]; then
        [ -f "$ADF" ] || { echo "ERROR: ADF not found: $ADF"; exit 1; }
        HARNESS_ARGS+=(--adf "$ADF")
    fi

    if [ -n "${CYCLES:-}" ]; then
        HARNESS_ARGS+=(--cycles "$CYCLES")
    elif [ -n "${FRAMES:-}" ]; then
        HARNESS_ARGS+=(--frames "$FRAMES")
    fi

    echo "[RUN] Harness ROM: $KICKSTART"
    if [ -n "${ADF:-}" ]; then
        echo "[RUN] Harness DF0: $ADF"
    else
        echo "[RUN] Harness DF0: no disk"
    fi

    tmpfile="$(mktemp)"
    trap 'rm -f "$tmpfile"; [ -n "${HARNESS_PID:-}" ] && kill "$HARNESS_PID" 2>/dev/null || true' EXIT

    env HARNESS_SERIAL_MODE=pty "${HARNESS_BIN}" "${HARNESS_ARGS[@]}" \
        > >(tee "$tmpfile") 2> >(tee -a "$tmpfile" >&2) &
    HARNESS_PID=$!

    pty_path=""
    for _ in $(seq 1 200); do
        if grep -q 'Serial PTY ready:' "$tmpfile"; then
            pty_path="$(sed -n 's/.*Serial PTY ready: \(\/dev\/pts\/[0-9][0-9]*\).*/\1/p' "$tmpfile" | tail -n1)"
            break
        fi
        sleep 0.05
    done

    [ -n "$pty_path" ] || {
        echo "ERROR: could not discover harness serial PTY"
        wait "$HARNESS_PID"
        exit 1
    }

    echo
    echo "[SERIAL] Open another terminal and run:"
    echo "  screen $pty_path 9600"
    echo
    printf '[SERIAL] Press Enter here after attaching to continue boot... '
    read -r _
    printf '\n' > /proc/"$HARNESS_PID"/fd/0
    wait "$HARNESS_PID"
    exit $?
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
