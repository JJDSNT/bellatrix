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
MULTICORE_LOGS="${BELLATRIX_MULTICORE_LOGS:-${CORE_LOG:-0}}"
BTSTACK_ENABLED="${BELLATRIX_BTSTACK:-0}"
USBSTACK_ENABLED="${BELLATRIX_USBSTACK:-0}"
EMU68_BOARDS_MODE="${BELLATRIX_EMU68_BOARDS_MODE:-boards}"

if [ "$MULTICORE_BUILD" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_ENABLE_MULTICORE"
    echo "[BUILD] multicore build: enabled"
else
    echo "[BUILD] multicore build: disabled"
fi

if [ "$MULTICORE_LOGS" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_CORE_LOG"
    echo "[BUILD] core log: enabled ([CORE0-CPU] [CORE1-GFX] [CORE2-PAULA] [CORE3-IO] [XCORE-*])"
else
    echo "[BUILD] core log: disabled"
fi

if [ "$BTSTACK_ENABLED" = "1" ]; then
    echo "[BUILD] btstack: enabled"
else
    echo "[BUILD] btstack: disabled"
fi

if [ "$USBSTACK_ENABLED" = "1" ]; then
    echo "[BUILD] usb stack: enabled (CherryUSB scaffold)"
else
    echo "[BUILD] usb stack: disabled"
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

# BELLATRIX_SERIAL=pl011           → use PL011 UART on GPIO 14/15
# BELLATRIX_SERIAL_BAUD=115200     → host-side UART baud rate
# BELLATRIX_SERIAL_LOOPBACK=1      → full internal TX->RX echo
# BELLATRIX_SERIAL_LOOPBACK=probe  → short detection echo, then disable
#
# The physical host UART speed is independent from Paula's emulated SERPER
# timing. Keeping the host link at 115200 avoids a silent baud switch between
# Emu68 boot logs and the bridged Amiga serial console.
PL011_FLAG="OFF"
if [ "${BELLATRIX_SERIAL:-}" = "pl011" ]; then
    PL011_FLAG="ON"
    BAUD="${BELLATRIX_SERIAL_BAUD:-115200}"
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_UART_BAUD=${BAUD}"
    echo "[BUILD] Serial backend: PL011 (GPIO 14/15, ${BAUD} baud)"
fi

if [ "${BELLATRIX_SERIAL_LOOPBACK:-0}" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_UART_LOOPBACK_MODE=1"
    echo "[BUILD] Serial loopback: full internal echo enabled"
elif [ "${BELLATRIX_SERIAL_LOOPBACK:-0}" = "probe" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_UART_LOOPBACK_MODE=2"
    echo "[BUILD] Serial loopback: probe echo enabled"
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
    -DBELLATRIX_UART_PL011="$PL011_FLAG" \
    -DBELLATRIX_ENABLE_EMU68_BOARDS="$EMU68_BOARDS_ENABLED" \
    -DBELLATRIX_ENABLE_BTSTACK="$BTSTACK_ENABLED" \
    -DBELLATRIX_ENABLE_USBSTACK="$USBSTACK_ENABLED" \
    -DBELLATRIX_BTSTACK_PATCHRAM_SOURCE="$BT_PATCHRAM_SOURCE"

make -j"$(nproc)"
make install

hide_modified_files

echo ""
echo "Build complete. Image: $INSTALL/Emu68.img"
