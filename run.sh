#!/usr/bin/env bash
# bellatrix/run.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS="$ROOT/scripts"
. "$SCRIPTS/bellatrix-image.sh"

LAUNCHER_DIR="$ROOT/tools/launcher"
LAUNCHER_BIN="$LAUNCHER_DIR/bin/bellatrix-launcher"
ROMS_DIR="$ROOT/src/roms"

HARNESS_BUILD_DIR="$ROOT/out/harness"
HARNESS_BIN="$HARNESS_BUILD_DIR/harness"

set_harness_paths() {
    HARNESS_BUILD_DIR="$ROOT/out/harness-rigel"
    HARNESS_BIN="$HARNESS_BUILD_DIR/harness"
}

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

Common options (env vars):
  BELLATRIX_MULTICORE_BUILD=1
                      Enable Bellatrix multicore build/runtime (default: off)
  BELLATRIX_LOGS=1   Enable Bellatrix/Rigel trace logs; also enables
                      multicore/core logs only when multicore build is enabled
                      (default: off)
  BELLATRIX_BTSTACK=1 Enable BTStack in Bellatrix build (default: off)
  BELLATRIX_USBSTACK=1
                      Enable CherryUSB host stack scaffold in Bellatrix build
                      (default: off)
  BELLATRIX_USB_MSC=1 Enable CherryUSB USB mass-storage class when USB stack is
                      enabled (default: on)
  BELLATRIX_RTG=1   Enable the Bellatrix RTG board where supported
                      (default: off)
  BELLATRIX_HDMI_AUDIO=1
                      Enable direct-register HDMI audio output (default: off).
                      Real Pi 3B hardware only -- QEMU's raspi3b machine does
                      not model this MMIO and will hang on boot if enabled.
  BELLATRIX_QEMU_USB_KBD=1
                      In qemu mode, attach a virtual USB keyboard device
                      (default: on when BELLATRIX_USBSTACK=1)
  BELLATRIX_USB_POINTER=<mouse|tablet>
                      In qemu mode with USB enabled, choose the companion USB
                      pointer device to attach alongside the keyboard
                      (default: mouse)
  BELLATRIX_EMU68_BOARDS_MODE=<boards|legacy>
                      Build Bellatrix either with Emu68 expansion boards
                      enabled or with the legacy Bellatrix Fast RAM path
                      (default: legacy)
  BELLATRIX_CHIPSET_BACKEND=<legacy|rigel>
                      Select Bellatrix chipset backend
                      (default: rigel)
  EMU_PROFILE=<name>  Runtime profile: bellatrix, bellatrix-musashi or emu68
                      (default: bellatrix)
  BELLATRIX_Z2_RAM_SIZE=<off|1|2|4|8>
                      Append Emu68 Zorro II RAM board size to BOOTARGS for
                      Bellatrix/QEMU testing (default: off)
  BELLATRIX_Z3_68040=<auto|0|1>
                      Register Emu68's native 68040 support ROM through the
                      shared Bellatrix Z3 lifecycle (experimental; default: 0)
  BELLATRIX_BT_FIRMWARE_FILE=<path>
                      Use a local BCM PatchRAM .hcd file during build
  BELLATRIX_BT_FIRMWARE_FETCH=1
                      If firmware is missing locally, try downloading it during
                      build and embed it in the image (default: 1)
  BELLATRIX_BT_FIRMWARE_URL=<url>
                      Override the build-time firmware download URL
  CORE_LOG=1          Legacy alias for multicore/core logs

QEMU options (env vars):
  EMU_PROFILE=<name>  Runtime profile: bellatrix or emu68 (default: bellatrix)
  KICKSTART=<file>    Pass a Kickstart ROM as initrd (optional)
  BOOTARGS=<string>   Emu68 boot arguments (default: "enable_cache")
                      Key options: enable_cache (required for KS/bare-metal),
                      debug (JIT block stats), disassemble (M68k+ARM side-by-side)
  DISPLAY_MODE=<mode> QEMU display mode override: gtk or none (default: gtk)
  NO_TUI=1            Skip launcher TUI even if KICKSTART is unset

Harness options (env vars):
  KICKSTART=<file>    ROM to run (required, or selected via TUI)
  ADF=<file>          Mount ADF image as DF0 (optional)
  HDF=<file>          Attach HDF (RDB) image as IDE hard disk (harness only)
  FRAMES=<n>          Stop after N frames and exit (headless mode)
  CYCLES=<n>          Stop after N M68K cycles and exit (headless mode)
  HARNESS_CPU=<type>  Musashi CPU type: 68000, 68010, 68ec020, 68020, 68030, 68040
                      (default: 68000)
  HARNESS_SDL_VSYNC=0 Disable the SDL VSync request in interactive mode and run
                      without display pacing when the selected renderer allows it
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
  HARNESS_MOUSE_LMB_SCRIPT=<frame:count,...>
                      Hold Amiga left mouse button for diagnostic scripted clicks
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
    set_harness_paths
    mkdir -p "$HARNESS_BUILD_DIR"

    local HARNESS_CFLAGS=""
    local core_log="${BELLATRIX_MULTICORE_LOGS:-${CORE_LOG:-0}}"
    if [ "$core_log" = "1" ]; then
        HARNESS_CFLAGS="-DBELLATRIX_CORE_LOG"
        echo "[BUILD] core log: enabled ([CORE0-CPU] [CORE1-GFX] [CORE2-PAULA] [CORE3-IO] [XCORE-*])"
    fi

    (
        cd "$HARNESS_BUILD_DIR"
        cmake "$ROOT/tools/harness" \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF \
            ${HARNESS_CFLAGS:+-DCMAKE_C_FLAGS="$HARNESS_CFLAGS"} \
            > /dev/null
        make -j"$(nproc)"
    )
}

build_cd_board() {
    # CD automount is now handled by ODFileSystem in the RIPPLE second ROM bank.
    # The legacy src/boards/resident/ DiagArea approach has been superseded.
    if [ ! -d "${ROOT}/src/boards/resident" ]; then
        return 0
    fi
    local DOCKER_WRAPPER="${ROOT}/emu68/build-scripts/build-m68k-amigaos"
    if [ ! -x "$DOCKER_WRAPPER" ]; then
        echo "[CD-BOARD] Docker wrapper not found — skipping ROM build"
        return 0
    fi
    echo "[BUILD] CD automount ROM (m68k DiagArea)"
    TTY_ENABLED="" "$DOCKER_WRAPPER" make -C "${ROOT}/src/boards/resident"
}

set_profile_paths() {
    local image_name

    case "$1" in
        bellatrix)
            image_name="$(bellatrix_image_name emu68 68040 "${BELLATRIX_MULTICORE_BUILD:-0}")"
            INSTALL="$ROOT/out/firmware"
            IMAGE="$ROOT/out/images/$image_name"
            DTB="$INSTALL/bcm2710-rpi-3-b.dtb"
            BUILD_KIND="bellatrix"
            BELLATRIX_CPU_BACKEND_PROFILE="emu68"
            ;;
        bellatrix-musashi)
            image_name="$(bellatrix_image_name musashi "${HARNESS_CPU:-${BELLATRIX_MUSASHI_CPU:-68040}}" "${BELLATRIX_MULTICORE_BUILD:-0}")"
            INSTALL="$ROOT/out/firmware"
            IMAGE="$ROOT/out/images/$image_name"
            DTB="$INSTALL/bcm2710-rpi-3-b.dtb"
            BUILD_KIND="bellatrix"
            BELLATRIX_CPU_BACKEND_PROFILE="musashi"
            ;;
        emu68)
            INSTALL="$ROOT/emu68/build"
            IMAGE="$INSTALL/Emu68.img"
            DTB="$INSTALL/firmware/bcm2710-rpi-3-b.dtb"
            BUILD_KIND="emu68"
            BELLATRIX_CPU_BACKEND_PROFILE=""
            ;;
        *)
            echo "ERROR: invalid EMU_PROFILE: $1"
            echo "Valid values: bellatrix, bellatrix-musashi, emu68"
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
            ADF)
                ADF="$value"
                ;;
            ISO)
                ISO="$value"
                ;;
            HDF)
                HDF="$value"
                ;;
            DISPLAY_MODE)
                DISPLAY_MODE="$value"
                ;;
            EMU_PROFILE)
                EMU_PROFILE="$value"
                ;;
            HARNESS_CPU)
                HARNESS_CPU="$value"
                ;;
            BOOTARGS)
                BOOTARGS="$value"
                ;;
            BELLATRIX_MULTICORE_BUILD)
                BELLATRIX_MULTICORE_BUILD="$value"
                ;;
            BELLATRIX_MULTICORE_LOGS)
                BELLATRIX_MULTICORE_LOGS="$value"
                ;;
            BELLATRIX_LOGS)
                BELLATRIX_LOGS="$value"
                ;;
            BELLATRIX_BTSTACK)
                BELLATRIX_BTSTACK="$value"
                ;;
            BELLATRIX_USBSTACK)
                BELLATRIX_USBSTACK="$value"
                ;;
            BELLATRIX_HDMI_AUDIO)
                BELLATRIX_HDMI_AUDIO="$value"
                ;;
            BELLATRIX_USB_MSC)
                BELLATRIX_USB_MSC="$value"
                ;;
            BELLATRIX_RTG)
                BELLATRIX_RTG="$value"
                # An explicit shell override must win over the launcher's
                # persisted profile (for example HARNESS_RTG=1 ./run.sh
                # harness).  Only inherit the profile when it was not set.
                HARNESS_RTG="${HARNESS_RTG:-$value}"
                ;;
            BELLATRIX_USB_POINTER)
                BELLATRIX_USB_POINTER="$value"
                ;;
            BELLATRIX_EMU68_BOARDS_MODE)
                BELLATRIX_EMU68_BOARDS_MODE="$value"
                ;;
            BELLATRIX_CHIPSET_BACKEND)
                BELLATRIX_CHIPSET_BACKEND="$value"
                ;;
            BELLATRIX_TRACE_LOGS)
                BELLATRIX_TRACE_LOGS="$value"
                ;;
            BELLATRIX_TRACE)
                BELLATRIX_TRACE="$value"
                ;;
            BELLATRIX_RIGEL_TRACE)
                BELLATRIX_RIGEL_TRACE="$value"
                ;;
            BELLATRIX_PERF_LOGS_OFF)
                BELLATRIX_PERF_LOGS_OFF="$value"
                ;;
            BELLATRIX_PROFILE)
                BELLATRIX_PROFILE="$value"
                ;;
            BELLATRIX_Z2_RAM_SIZE)
                BELLATRIX_Z2_RAM_SIZE="$value"
                ;;
            BELLATRIX_SERIAL)
                BELLATRIX_SERIAL="$value"
                ;;
            BELLATRIX_OSD)
                BELLATRIX_OSD="$value"
                ;;
            BELLATRIX_LAUNCHER)
                BELLATRIX_LAUNCHER="$value"
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
    # Pick ROM via TUI if not set
    if [ -z "${KICKSTART:-}" ]; then
        load_launcher_selection
    fi

    echo "[BUILD] Harness (Musashi, native)"
    echo "[BUILD] Harness chipset backend: ${BELLATRIX_CHIPSET_BACKEND:-rigel}"
    build_harness

    # Auto-build cdmount.rom when running with an ISO
    if [ -n "${ISO:-}" ]; then
        build_cd_board
    fi

    [ -n "${KICKSTART:-}" ] || { echo "ERROR: no ROM selected"; exit 1; }
    [ -f "$KICKSTART" ]     || { echo "ERROR: ROM not found: $KICKSTART"; exit 1; }

    HARNESS_ARGS=("$KICKSTART")

    if [ -n "${HARNESS_CPU:-}" ]; then
        HARNESS_ARGS+=(--cpu "$HARNESS_CPU")
    fi

    if [ -n "${ADF:-}" ]; then
        [ -f "$ADF" ] || { echo "ERROR: ADF not found: $ADF"; exit 1; }
        HARNESS_ARGS+=(--adf "$ADF")
    fi

    if [ -n "${ISO:-}" ]; then
        [ -f "$ISO" ] || { echo "ERROR: ISO not found: $ISO"; exit 1; }
        HARNESS_ARGS+=(--iso "$ISO")
    fi

    if [ -n "${HDF:-}" ]; then
        [ -f "$HDF" ] || { echo "ERROR: HDF not found: $HDF"; exit 1; }
        HARNESS_ARGS+=(--hdf "$HDF")
    fi

    PLUGINS_DIR="${PLUGINS_DIR:-$HARNESS_BUILD_DIR/plugins}"
    if [ -d "$PLUGINS_DIR" ]; then
        HARNESS_ARGS+=(--plugins "$PLUGINS_DIR")
    fi

    if [ -n "${CYCLES:-}" ]; then
        HARNESS_ARGS+=(--cycles "$CYCLES")
    elif [ -n "${FRAMES:-}" ]; then
        HARNESS_ARGS+=(--frames "$FRAMES")
    fi
    # Without FRAMES or CYCLES: interactive SDL2 window (default)

    echo "[RUN] Harness ROM: $KICKSTART"
    echo "[RUN] Harness CPU: ${HARNESS_CPU:-68040}"
    if [ -n "${ADF:-}" ]; then
        echo "[RUN] Harness DF0: $ADF"
    else
        echo "[RUN] Harness DF0: no disk"
    fi
    if [ -n "${ISO:-}" ]; then
        echo "[RUN] Harness CD-ROM: $ISO"
    fi
    if [ -n "${HDF:-}" ]; then
        echo "[RUN] Harness HD: $HDF"
    fi
    if [ -d "$PLUGINS_DIR" ]; then
        echo "[RUN] Harness plugins: $PLUGINS_DIR"
    fi
    if [ "${HARNESS_SDL_VSYNC:-1}" = "0" ]; then
        echo "[RUN] Harness SDL VSync: disabled (HARNESS_SDL_VSYNC=0)"
    else
        echo "[RUN] Harness SDL VSync: enabled (default)"
    fi

    TRACE_LOGS="${BELLATRIX_LOGS:-${BELLATRIX_TRACE_LOGS:-${BELLATRIX_TRACE:-${BELLATRIX_RIGEL_TRACE:-0}}}}"
    export HARNESS_RTG="${HARNESS_RTG:-${BELLATRIX_RTG:-0}}"
    export BELLATRIX_LOGS="$TRACE_LOGS"
    export BELLATRIX_TRACE_LOGS="$TRACE_LOGS"
    export BELLATRIX_TRACE="$TRACE_LOGS"
    export BELLATRIX_RIGEL_TRACE="$TRACE_LOGS"
    exec "$HARNESS_BIN" "${HARNESS_ARGS[@]}"
fi

if [ "$MODE" = "harness-serial" ]; then
    if [ -z "${KICKSTART:-}" ]; then
        load_launcher_selection
    fi

    echo "[BUILD] Harness (Musashi, native)"
    echo "[BUILD] Harness chipset backend: ${BELLATRIX_CHIPSET_BACKEND:-rigel}"
    build_harness

    if [ -n "${ISO:-}" ]; then
        build_cd_board
    fi

    [ -n "${KICKSTART:-}" ] || { echo "ERROR: no ROM selected"; exit 1; }
    [ -f "$KICKSTART" ]     || { echo "ERROR: ROM not found: $KICKSTART"; exit 1; }
    HARNESS_ARGS=("$KICKSTART")

    if [ -n "${HARNESS_CPU:-}" ]; then
        HARNESS_ARGS+=(--cpu "$HARNESS_CPU")
    fi

    if [ -n "${ADF:-}" ]; then
        [ -f "$ADF" ] || { echo "ERROR: ADF not found: $ADF"; exit 1; }
        HARNESS_ARGS+=(--adf "$ADF")
    fi

    if [ -n "${ISO:-}" ]; then
        [ -f "$ISO" ] || { echo "ERROR: ISO not found: $ISO"; exit 1; }
        HARNESS_ARGS+=(--iso "$ISO")
    fi

    if [ -n "${HDF:-}" ]; then
        [ -f "$HDF" ] || { echo "ERROR: HDF not found: $HDF"; exit 1; }
        HARNESS_ARGS+=(--hdf "$HDF")
    fi

    PLUGINS_DIR="${PLUGINS_DIR:-$HARNESS_BUILD_DIR/plugins}"
    if [ -d "$PLUGINS_DIR" ]; then
        HARNESS_ARGS+=(--plugins "$PLUGINS_DIR")
    fi

    if [ -n "${CYCLES:-}" ]; then
        HARNESS_ARGS+=(--cycles "$CYCLES")
    elif [ -n "${FRAMES:-}" ]; then
        HARNESS_ARGS+=(--frames "$FRAMES")
    fi

    echo "[RUN] Harness ROM: $KICKSTART"
    echo "[RUN] Harness CPU: ${HARNESS_CPU:-68040}"
    if [ -n "${ADF:-}" ]; then
        echo "[RUN] Harness DF0: $ADF"
    else
        echo "[RUN] Harness DF0: no disk"
    fi
    if [ -n "${ISO:-}" ]; then
        echo "[RUN] Harness CD-ROM: $ISO"
    fi
    if [ -n "${HDF:-}" ]; then
        echo "[RUN] Harness HD: $HDF"
    fi
    if [ -d "$PLUGINS_DIR" ]; then
        echo "[RUN] Harness plugins: $PLUGINS_DIR"
    fi

    TRACE_LOGS="${BELLATRIX_LOGS:-${BELLATRIX_TRACE_LOGS:-${BELLATRIX_TRACE:-${BELLATRIX_RIGEL_TRACE:-0}}}}"
    export HARNESS_RTG="${HARNESS_RTG:-${BELLATRIX_RTG:-0}}"
    export BELLATRIX_LOGS="$TRACE_LOGS"
    export BELLATRIX_TRACE_LOGS="$TRACE_LOGS"
    export BELLATRIX_TRACE="$TRACE_LOGS"
    export BELLATRIX_RIGEL_TRACE="$TRACE_LOGS"

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

case "$MODE" in
    harness|harness-serial)
        echo "ERROR: internal harness dispatch failed"
        exit 1
        ;;
esac

# ---------------------------------------------------------------------------
# QEMU / raspi / tftp modes — Emu68 build path
# ---------------------------------------------------------------------------
EMU_PROFILE="${EMU_PROFILE:-bellatrix}"
set_profile_paths "$EMU_PROFILE"

case "$MODE" in
    qemu)
        if [ "${NO_TUI:-0}" != "1" ] && [ -z "${KICKSTART:-}" ]; then
            load_launcher_selection
            EMU_PROFILE="${EMU_PROFILE:-bellatrix}"
            set_profile_paths "$EMU_PROFILE"
        fi
        ;;
esac

case "$BUILD_KIND" in
    bellatrix)
        echo "[BUILD] Profile: bellatrix"
        export BELLATRIX_MULTICORE_BUILD="${BELLATRIX_MULTICORE_BUILD:-0}"
        TRACE_LOGS="${BELLATRIX_LOGS:-${BELLATRIX_TRACE_LOGS:-${BELLATRIX_TRACE:-${BELLATRIX_RIGEL_TRACE:-0}}}}"
        CORE_LOGS_REQUESTED="${BELLATRIX_MULTICORE_LOGS:-${CORE_LOG:-$TRACE_LOGS}}"
        if [ "$BELLATRIX_MULTICORE_BUILD" = "1" ]; then
            export BELLATRIX_MULTICORE_LOGS="$CORE_LOGS_REQUESTED"
        else
            export BELLATRIX_MULTICORE_LOGS="0"
        fi
        export BELLATRIX_LOGS="$TRACE_LOGS"
        export BELLATRIX_TRACE_LOGS="$TRACE_LOGS"
        export BELLATRIX_TRACE="$TRACE_LOGS"
        export BELLATRIX_RIGEL_TRACE="$TRACE_LOGS"
        export BELLATRIX_BTSTACK="${BELLATRIX_BTSTACK:-0}"
        export BELLATRIX_USBSTACK="${BELLATRIX_USBSTACK:-0}"
        export BELLATRIX_USB_MSC="${BELLATRIX_USB_MSC:-1}"
        export BELLATRIX_RTG="${BELLATRIX_RTG:-0}"
        # Harness RTG is bellatrix.card -> SDL.  Bare metal builds and ships
        # Emu68's VideoCore.card, whose SetSwitch path controls the HVS.
        export BELLATRIX_RTG_VIDEOCORE="${BELLATRIX_RTG_VIDEOCORE:-$BELLATRIX_RTG}"
        export BELLATRIX_HDMI_AUDIO="${BELLATRIX_HDMI_AUDIO:-0}"
        export BELLATRIX_EMU68_BOARDS_MODE="${BELLATRIX_EMU68_BOARDS_MODE:-legacy}"
        export BELLATRIX_PROFILE="${BELLATRIX_PROFILE:-0}"
        export BELLATRIX_CHIPSET_BACKEND="${BELLATRIX_CHIPSET_BACKEND:-rigel}"
        export BELLATRIX_OSD="${BELLATRIX_OSD:-1}"
        export BELLATRIX_LAUNCHER="${BELLATRIX_LAUNCHER:-1}"
        export BELLATRIX_CPU_BACKEND="${BELLATRIX_CPU_BACKEND_PROFILE:-emu68}"
        if [ "${BELLATRIX_PERF_LOGS_OFF:-0}" = "1" ]; then
            echo "[BUILD] Performance log mute: on"
        fi
        export BELLATRIX_TRACE_BUILD="$TRACE_LOGS"
        export BELLATRIX_RIGEL_TRACE_BUILD="$TRACE_LOGS"
        _SERIAL_RAW="${BELLATRIX_SERIAL:-miniuart}"
        case "$_SERIAL_RAW" in
            log)      export BELLATRIX_SERIAL="log"      ;;
            miniuart) export BELLATRIX_SERIAL="miniuart" ;;
            *)        export BELLATRIX_SERIAL="miniuart" ;;
        esac
        export CORE_LOG="$BELLATRIX_MULTICORE_LOGS"
        "$SCRIPTS/setup.sh"
        "$SCRIPTS/build.sh"
        set_profile_paths "$EMU_PROFILE"
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
        Z2_RAM_SIZE="${BELLATRIX_Z2_RAM_SIZE:-off}"
        FINAL_BOOTARGS="${BOOTARGS:-enable_cache}"
        case "$Z2_RAM_SIZE" in
            off|"")
                ;;
            1|2|4|8)
                if [[ " $FINAL_BOOTARGS " != *" z2_ram_size="* ]]; then
                    FINAL_BOOTARGS="$FINAL_BOOTARGS z2_ram_size=$Z2_RAM_SIZE"
                fi
                ;;
            *)
                echo "ERROR: invalid BELLATRIX_Z2_RAM_SIZE: $Z2_RAM_SIZE"
                echo "Valid values: off, 1, 2, 4, 8"
                exit 1
                ;;
        esac
        #
        # AMIGA_SERIAL (env var):
        #   unset / log   — Amiga serial → mini-UART → second QEMU serial → /dev/stderr
        #                   Output appears mixed with the kprintf debug log (default)
        #   pty           — Amiga serial → mini-UART → second QEMU serial → PTY
        #                   Script pauses until you connect screen, then boots guest:
        #                     screen /dev/pts/X 9600
        QEMU_ARGS=(
            -M raspi3b
            -accel tcg,tb-size=64
            -kernel "$IMAGE"
            -dtb "$DTB"
            -serial null
            -display "$DISPLAY_ARG"
            -append "$FINAL_BOOTARGS"
        )

        if [ "${AMIGA_SERIAL:-log}" != "pty" ]; then
            QEMU_ARGS+=(-serial stdio)
        fi

        QEMU_USB_KBD="${BELLATRIX_QEMU_USB_KBD:-}"
        QEMU_USB_POINTER="${BELLATRIX_USB_POINTER:-mouse}"
        if [ -z "$QEMU_USB_KBD" ] && [ "${BELLATRIX_USBSTACK:-0}" = "1" ]; then
            QEMU_USB_KBD="1"
        fi
        if [ "$QEMU_USB_KBD" = "1" ]; then
            case "$QEMU_USB_POINTER" in
                mouse)
                    QEMU_ARGS+=(-device usb-kbd -device usb-mouse)
                    echo "[RUN] QEMU USB input: enabled (-device usb-kbd -device usb-mouse)"
                    ;;
                tablet)
                    QEMU_ARGS+=(-device usb-kbd -device usb-tablet)
                    echo "[RUN] QEMU USB input: enabled (-device usb-kbd -device usb-tablet)"
                    ;;
                *)
                    echo "ERROR: invalid BELLATRIX_USB_POINTER: $QEMU_USB_POINTER"
                    echo "Valid values: mouse, tablet"
                    exit 1
                    ;;
            esac
        else
            echo "[RUN] QEMU USB input: disabled"
        fi

        if [ -n "${KICKSTART:-}" ]; then
            [ -f "$KICKSTART" ] || { echo "ERROR: Kickstart not found: $KICKSTART"; exit 1; }
            QEMU_ARGS+=(-initrd "$KICKSTART")
            echo "[RUN] QEMU profile: $EMU_PROFILE"
            echo "[RUN] QEMU with Kickstart: $KICKSTART"
        else
            echo "[RUN] QEMU profile: $EMU_PROFILE"
            echo "[RUN] QEMU (no Kickstart — btrace-only mode)"
        fi

        # Inject ADF via QEMU generic loader at physical 0x18000000.
        # Bellatrix's launcher detects the 'DOS' boot-block magic there when
        # the EMMC init fails (no physical SD card in QEMU).
        if [ -n "${ADF:-}" ] && [ "${BELLATRIX_LAUNCHER:-1}" = "1" ]; then
            [ -f "$ADF" ] || { echo "ERROR: ADF not found: $ADF"; exit 1; }
            QEMU_ARGS+=(-device "loader,file=$ADF,addr=0x18000000,force-raw=on")
            echo "[RUN] QEMU DF0 loader: $ADF → phys 0x18000000"
        fi

        # Inject ISO via QEMU generic loader at physical 0x20000000.
        # Bellatrix's launcher detects the ISO 9660 PVD signature there and
        # attaches the image to the GAYLE ATAPI CD-ROM device.
        if [ -n "${ISO:-}" ] && [ "${BELLATRIX_LAUNCHER:-1}" = "1" ]; then
            [ -f "$ISO" ] || { echo "ERROR: ISO not found: $ISO"; exit 1; }
            QEMU_ARGS+=(-device "loader,file=$ISO,addr=0x20000000,force-raw=on")
            echo "[RUN] QEMU CD-ROM loader: $ISO → phys 0x20000000"
        fi

        echo "[RUN] Image: $IMAGE"
        echo "[RUN] DTB:   $DTB"
        echo "[RUN] Display mode: $DISPLAY_MODE"
        if [ "$BUILD_KIND" = "bellatrix" ]; then
            echo "[RUN] CPU backend profile: ${BELLATRIX_CPU_BACKEND_PROFILE:-emu68}"
        fi
        echo "[RUN] Chipset backend: ${BELLATRIX_CHIPSET_BACKEND:-rigel}"
        echo "[RUN] RTG: ${BELLATRIX_RTG:-0}"
        echo "[RUN] Emu68 boards mode: ${BELLATRIX_EMU68_BOARDS_MODE:-legacy}"
        echo "[RUN] Boot args: $FINAL_BOOTARGS"

        if [ "${AMIGA_SERIAL:-log}" = "pty" ]; then
            # Detect the PTY by watching /dev/pts/ for new entries — reliable regardless
            # of which fd QEMU uses to print the "char device redirected" message.
            _MON_SOCK="/tmp/bellatrix-qemu-mon-$$.sock"
            trap 'rm -f "$_MON_SOCK"' EXIT

            # Snapshot existing PTY numbers before starting QEMU
            _PTS_BEFORE=$(ls /dev/pts/ 2>/dev/null | grep -E '^[0-9]+$' | sort)

            QEMU_ARGS+=(
                -chardev pty,id=amiga_serial
                -serial chardev:amiga_serial
                -S
                -monitor "unix:${_MON_SOCK},server=on,wait=off"
            )

            # stdin from /dev/null so QEMU doesn't compete with our read below
            qemu-system-aarch64 "${QEMU_ARGS[@]}" </dev/null &
            _QEMU_PID=$!

            # wait up to 5 s for a new /dev/pts/N to appear
            _PTY=""
            for _ in $(seq 1 100); do
                _PTS_AFTER=$(ls /dev/pts/ 2>/dev/null | grep -E '^[0-9]+$' | sort)
                _NEW=$(comm -13 <(echo "$_PTS_BEFORE") <(echo "$_PTS_AFTER") | head -1)
                if [ -n "$_NEW" ]; then
                    _PTY="/dev/pts/$_NEW"
                    break
                fi
                sleep 0.05
            done

            if [ -z "$_PTY" ]; then
                echo "ERROR: no new PTY appeared after starting QEMU" >&2
                kill "$_QEMU_PID" 2>/dev/null; wait "$_QEMU_PID" 2>/dev/null || true
                exit 1
            fi

            echo "" >&2
            echo "[SERIAL] Amiga serial PTY: $_PTY" >&2
            echo "[SERIAL] Guest is PAUSED. In another terminal:" >&2
            echo "[SERIAL]   screen $_PTY 9600" >&2
            printf '[SERIAL] Press Enter here to boot... ' >&2
            read -r _ </dev/tty

            # resume guest via QEMU monitor
            printf 'cont\n' | nc -U "$_MON_SOCK" >/dev/null 2>&1 || true

            wait "$_QEMU_PID"
            _QEMU_EXIT=$?
            exit "$_QEMU_EXIT"
        fi

        exec qemu-system-aarch64 "${QEMU_ARGS[@]}"
        ;;
    raspi)
        MOUNT="${1:-}"
        [ -n "$MOUNT" ] || { echo "Usage: ./run.sh raspi /media/user/BOOT"; exit 1; }

        if [ "$BUILD_KIND" != "bellatrix" ]; then
            echo "ERROR: raspi mode currently supports only Bellatrix profiles"
            exit 1
        fi

        export BELLATRIX_INSTALL_DIR="$INSTALL"
        "$SCRIPTS/flash.sh" "$MOUNT"
        echo "[DONE] SD ready."
        ;;
    tftp)
        if [ "$BUILD_KIND" != "bellatrix" ]; then
            echo "ERROR: tftp mode currently supports only Bellatrix profiles"
            exit 1
        fi

        export BELLATRIX_INSTALL_DIR="$INSTALL"
        "$SCRIPTS/flash.sh" tftp
        ;;
esac
