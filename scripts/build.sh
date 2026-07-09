#!/usr/bin/env bash
# bellatrix/scripts/build.sh
#
# Compile Emu68 with VARIANT=bellatrix.
# Assumes setup.sh has been run and patches are applied.
#
# Usage:
#   ./scripts/build.sh              # incremental build
#   ./scripts/build.sh clean        # wipe and rebuild
#
# Debug env vars:
#   BTRACE_FILTER=0x0004            # initial btrace filter (chipset-only)
#   BTRACE_FILTER=0xFFFF            # initial btrace filter (all accesses)
#   (default is 0x0001 = unimplemented only)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
EMU68="$ROOT/emu68"
BUILD="$EMU68/build-bellatrix"
INSTALL="$EMU68/install-bellatrix"
TOOLCHAIN="$EMU68/toolchains/aarch64-linux-gnu.cmake"
BT_FW_CACHE_DIR="$ROOT/.cache/bellatrix/btstack"
BT_FW_DEVICE="${BELLATRIX_BT_FIRMWARE_DEVICE:-BCM43430A1}"
BT_FW_URL_DEFAULT="https://github.com/OpenELEC/misc-firmware/raw/master/firmware/brcm/${BT_FW_DEVICE}.hcd"

prepare_bt_patchram() {
    local device="$1"
    local build_dir="$2"
    local cache_dir="$BT_FW_CACHE_DIR"
    local source_override="${BELLATRIX_BT_FIRMWARE_FILE:-}"
    local hcd_file=""
    local generated_dir="$build_dir/generated-bt-fw"
    local generated_c="$generated_dir/${device}.c"

    mkdir -p "$cache_dir" "$generated_dir"

    if [ -n "$source_override" ] && [ -f "$source_override" ]; then
        hcd_file="$source_override"
        echo "[BUILD] bt firmware: using override $hcd_file" >&2
    else
        for candidate in \
            "$cache_dir/${device}.hcd" \
            "$ROOT/firmware/brcm/${device}.hcd" \
            "$ROOT/assets/firmware/brcm/${device}.hcd" \
            "/lib/firmware/brcm/${device}.hcd" \
            "/usr/lib/firmware/brcm/${device}.hcd"
        do
            if [ -f "$candidate" ]; then
                hcd_file="$candidate"
                echo "[BUILD] bt firmware: found $hcd_file" >&2
                break
            fi
        done
    fi

    if [ -z "$hcd_file" ] && [ "${BELLATRIX_BT_FIRMWARE_FETCH:-1}" = "1" ]; then
        local url="${BELLATRIX_BT_FIRMWARE_URL:-$BT_FW_URL_DEFAULT}"
        local cached_hcd="$cache_dir/${device}.hcd"
        echo "[BUILD] bt firmware: downloading ${device}.hcd during build" >&2
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL "$url" -o "$cached_hcd" || true
        elif command -v wget >/dev/null 2>&1; then
            wget -qO "$cached_hcd" "$url" || true
        fi
        if [ -f "$cached_hcd" ] && [ -s "$cached_hcd" ]; then
            hcd_file="$cached_hcd"
            echo "[BUILD] bt firmware: cached at $hcd_file" >&2
        fi
    fi

    if [ -z "$hcd_file" ]; then
        echo "[BUILD] bt firmware: not found; continuing without embedded PatchRAM" >&2
        return 1
    fi

    python3 "$ROOT/external/btstack/chipset/bcm/convert_hcd.py" "$hcd_file" "$generated_dir" >/dev/null
    echo "$generated_c"
    return 0
}

hide_modified_files() {
    local modified
    modified="$(git -C "$ROOT" ls-files -m -- "$EMU68" "$ROOT/external/cherryusb" || true)"
    if [ -n "$modified" ]; then
        echo "Silencing modified submodule files from git status..."
        while IFS= read -r file; do
            [ -n "$file" ] && git -C "$ROOT" update-index --assume-unchanged "$file"
        done <<< "$modified"
    fi
}

CPU_BACKEND="${BELLATRIX_CPU_BACKEND:-emu68}"
MUSASHI_CPU="${BELLATRIX_MUSASHI_CPU:-${HARNESS_CPU:-68040}}"

case "$CPU_BACKEND" in
    emu68|"")
        BUILD="$EMU68/build-bellatrix-rigel"
        INSTALL="$EMU68/install-bellatrix-rigel"
        MUSASHI_CPU_FLAG="OFF"
        ;;
    musashi)
        case "$MUSASHI_CPU" in
            68000|68010|68ec020|68020|68030|68040)
                ;;
            *)
                echo "ERROR: invalid BELLATRIX_MUSASHI_CPU: $MUSASHI_CPU"
                echo "Valid values: 68000, 68010, 68ec020, 68020, 68030, 68040"
                exit 1
                ;;
        esac
        BUILD="$EMU68/build-bellatrix-rigel-musashi"
        INSTALL="$EMU68/install-bellatrix-rigel-musashi"
        MUSASHI_CPU_FLAG="ON"
        ;;
    *)
        echo "ERROR: invalid BELLATRIX_CPU_BACKEND: $CPU_BACKEND"
        echo "Valid values: emu68, musashi"
        exit 1
        ;;
esac

if [ "${1:-}" = "clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD" "$INSTALL"
fi

mkdir -p "$BUILD" "$INSTALL"
cd "$BUILD"

EXTRA_DEFINES=""
if [ -n "${BTRACE_FILTER:-}" ]; then
    EXTRA_DEFINES="-DBELLATRIX_BTRACE_INIT_FILTER=${BTRACE_FILTER}"
    echo "[BUILD] btrace init filter: ${BTRACE_FILTER}"
fi

MULTICORE_BUILD="${BELLATRIX_MULTICORE_BUILD:-0}"
MULTICORE_LOGS="${BELLATRIX_MULTICORE_LOGS:-${BELLATRIX_LOGS:-${CORE_LOG:-0}}}"
if [ "$MULTICORE_BUILD" != "1" ]; then
    MULTICORE_LOGS="0"
fi
BTSTACK_ENABLED="${BELLATRIX_BTSTACK:-0}"
USBSTACK_ENABLED="${BELLATRIX_USBSTACK:-0}"
USB_MSC_ENABLED="${BELLATRIX_USB_MSC:-1}"
HDMI_AUDIO_ENABLED="${BELLATRIX_HDMI_AUDIO:-0}"
EMU68_BOARDS_MODE="${BELLATRIX_EMU68_BOARDS_MODE:-legacy}"
OSD_ENABLED="${BELLATRIX_OSD:-1}"
LAUNCHER_ENABLED="${BELLATRIX_LAUNCHER:-1}"

echo "[BUILD] cpu backend: $CPU_BACKEND"
if [ "$CPU_BACKEND" = "musashi" ]; then
    echo "[BUILD] musashi cpu model: $MUSASHI_CPU"
fi
echo "[BUILD] chipset backend: rigel"

MULTICORE_FLAG="OFF"
if [ "$MULTICORE_BUILD" = "1" ]; then
    MULTICORE_FLAG="ON"
    echo "[BUILD] multicore build: enabled (Core1=CPU Core2=Chipset/Rigel Core3=IO)"
fi

if [ "$MULTICORE_BUILD" != "1" ]; then
    MULTICORE_FLAG="OFF"
    echo "[BUILD] multicore build: disabled (single-core)"
fi

CORELOG_FLAG="OFF"
if [ "$MULTICORE_LOGS" = "1" ]; then
    CORELOG_FLAG="ON"
    echo "[BUILD] core log: enabled ([CORE0-HOST] [CORE1-CPU] [CORE2-CHIPSET] [CORE3-IO] [XCORE-*])"
else
    echo "[BUILD] core log: disabled"
fi

if [ "$BTSTACK_ENABLED" = "1" ]; then
    echo "[BUILD] btstack: enabled"
else
    echo "[BUILD] btstack: disabled"
fi

if [ "$HDMI_AUDIO_ENABLED" = "1" ]; then
    echo "[BUILD] hdmi audio: enabled (real Pi hardware only -- do not use with QEMU)"
else
    echo "[BUILD] hdmi audio: disabled"
fi

if [ "$USBSTACK_ENABLED" = "1" ]; then
    echo "[BUILD] usb stack: enabled (CherryUSB scaffold)"
else
    echo "[BUILD] usb stack: disabled"
fi

if [ "${BELLATRIX_USB_LOG:-0}" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_USB_LOG=1"
    echo "[BUILD] usb log: enabled"
else
    echo "[BUILD] usb log: disabled"
fi

OSD_FLAG="OFF"
if [ "$OSD_ENABLED" = "1" ]; then
    OSD_FLAG="ON"
    echo "[BUILD] OSD overlay: enabled"
else
    echo "[BUILD] OSD overlay: disabled"
fi

LAUNCHER_FLAG="OFF"
if [ "$LAUNCHER_ENABLED" = "1" ]; then
    LAUNCHER_FLAG="ON"
    echo "[BUILD] launcher: enabled (SD FAT32 ADF selector)"
else
    echo "[BUILD] launcher: disabled"
fi

case "$EMU68_BOARDS_MODE" in
    boards)
        EMU68_BOARDS_ENABLED="ON"
        echo "[BUILD] emu68 boards: enabled"
        ;;
    legacy)
        EMU68_BOARDS_ENABLED="OFF"
        echo "[BUILD] emu68 boards: disabled (legacy Bellatrix Fast RAM path)"
        ;;
    *)
        echo "ERROR: invalid BELLATRIX_EMU68_BOARDS_MODE: $EMU68_BOARDS_MODE"
        echo "Valid values: boards, legacy"
        exit 1
        ;;
esac

# BELLATRIX_SERIAL_LOOPBACK=1      → full internal TX->RX echo
# BELLATRIX_SERIAL_LOOPBACK=probe  → short detection echo, then disable
#
# PL011 belongs to Bluetooth in every build; kprintf and Paula's serial
# bridge share the mini-UART (see AI_context/issue_logging_miniuart.md).
UART_LOG_FLAG=""
case "${BELLATRIX_SERIAL:-}" in
    log)
        UART_LOG_FLAG="-DBELLATRIX_UART_LOG=1"
        EXTRA_DEFINES="$EXTRA_DEFINES ${UART_LOG_FLAG}"
        echo "[BUILD] Serial backend: log (Paula TX → kprintf [SERIAL], no UART bridge)"
        ;;
    miniuart|"")
        echo "[BUILD] Serial backend: miniUART (default)"
        ;;
    *)
        echo "[BUILD] Serial backend: miniUART (unknown BELLATRIX_SERIAL=${BELLATRIX_SERIAL}, using fallback)"
        ;;
esac

if [ "${BELLATRIX_SERIAL_LOOPBACK:-0}" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_UART_LOOPBACK_MODE=1"
    echo "[BUILD] Serial loopback: full internal echo enabled"
elif [ "${BELLATRIX_SERIAL_LOOPBACK:-0}" = "probe" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_UART_LOOPBACK_MODE=2"
    echo "[BUILD] Serial loopback: probe echo enabled"
fi

if [ "${BELLATRIX_RIGEL_TRACE_BUILD:-0}" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_RIGEL_TRACE_BUILD=1"
    echo "[BUILD] Rigel trace: enabled unconditionally (bare-metal)"
fi

if [ "${BELLATRIX_TRACE_BUILD:-0}" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_TRACE_BUILD=1"
    echo "[BUILD] Bellatrix trace: enabled unconditionally (bare-metal)"
fi

if [ "${BELLATRIX_EMU68_API_TRACE:-0}" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_EMU68_API_TRACE=1"
    echo "[BUILD] Emu68 API trace: enabled"
fi

if [ "${BELLATRIX_EMU68_API_AUTODUMP:-0}" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_EMU68_API_AUTODUMP=1"
    echo "[BUILD] Emu68 API auto stats dump: enabled"
fi

PROFILE_FLAG="OFF"
if [ "${BELLATRIX_PROFILE:-0}" = "1" ]; then
    PROFILE_FLAG="ON"
fi

BT_PATCHRAM_SOURCE=""
if [ "$BTSTACK_ENABLED" = "1" ]; then
    if BT_PATCHRAM_SOURCE="$(prepare_bt_patchram "$BT_FW_DEVICE" "$BUILD")"; then
        echo "[BUILD] bt firmware: generated source $BT_PATCHRAM_SOURCE"
    else
        BT_PATCHRAM_SOURCE=""
    fi
fi

cmake "$EMU68" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL" \
    -DTARGET=raspi64 \
    -DVARIANT=bellatrix \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_C_FLAGS="$EXTRA_DEFINES" \
    -DCMAKE_CXX_FLAGS="$EXTRA_DEFINES" \
    -DBELLATRIX_ENABLE_EMU68_BOARDS="$EMU68_BOARDS_ENABLED" \
    -DBELLATRIX_USE_MUSASHI_CPU="$MUSASHI_CPU_FLAG" \
    -DBELLATRIX_MUSASHI_CPU="$MUSASHI_CPU" \
    -DBELLATRIX_ENABLE_BTSTACK="$BTSTACK_ENABLED" \
    -DBELLATRIX_ENABLE_USBSTACK="$USBSTACK_ENABLED" \
    -DBELLATRIX_ENABLE_USB_MSC="$USB_MSC_ENABLED" \
    -DBELLATRIX_ENABLE_HDMI_AUDIO="$HDMI_AUDIO_ENABLED" \
    -DBELLATRIX_BTSTACK_PATCHRAM_SOURCE="$BT_PATCHRAM_SOURCE" \
    -DBELLATRIX_OSD="$OSD_FLAG" \
    -DBELLATRIX_ENABLE_MULTICORE="$MULTICORE_FLAG" \
    -DBELLATRIX_CORE_LOG="$CORELOG_FLAG" \
    -DBELLATRIX_LAUNCHER="$LAUNCHER_FLAG" \
    -DBELLATRIX_PROFILE="$PROFILE_FLAG"

make -j"$(nproc)"
make install

hide_modified_files

echo ""
echo "Build complete. Image: $INSTALL/Emu68.img"
