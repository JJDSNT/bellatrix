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

hide_modified_files() {
    local modified
    modified="$(git -C "$ROOT" ls-files -m -- "$EMU68" || true)"
    if [ -n "$modified" ]; then
        echo "Silencing modified emu68 files from git status..."
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

if [ "${CORE_LOG:-0}" = "1" ]; then
    EXTRA_DEFINES="$EXTRA_DEFINES -DBELLATRIX_CORE_LOG"
    echo "[BUILD] core log: enabled ([CORE0-CPU] [CORE1-GFX] [CORE2-PAULA] [CORE3-IO] [XCORE-*])"
fi

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

cmake "$EMU68" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL" \
    -DTARGET=raspi64 \
    -DVARIANT=bellatrix \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DBELLATRIX_UART_PL011="$PL011_FLAG" \
    ${EXTRA_DEFINES:+-DCMAKE_C_FLAGS="$EXTRA_DEFINES"}

make -j"$(nproc)"
make install

hide_modified_files

echo ""
echo "Build complete. Image: $INSTALL/Emu68.img"
