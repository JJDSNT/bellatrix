#!/usr/bin/env bash
# Build all six Bellatrix kernels and the U-Boot two-stage selector.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

build_variant() {
    local profile="$1"
    local backend="$2"
    local cpu="$3"
    local multicore="$4"

    BELLATRIX_RELEASE_PROFILE="$profile" \
    BELLATRIX_CPU_BACKEND="$backend" \
    BELLATRIX_MUSASHI_CPU="$cpu" \
    BELLATRIX_MULTICORE_BUILD="$multicore" \
    BELLATRIX_BUILD_VIDEOCORE_CARD=0 \
        "$SCRIPT_DIR/build.sh" "${BUILD_MODE:-}"
}

build_variant musashi_68000 musashi 68000 0
build_variant musashi_68040 musashi 68040 0
build_variant emu68 emu68 68040 0
build_variant musashi_68000 musashi 68000 1
build_variant musashi_68040 musashi 68040 1
build_variant emu68 emu68 68040 1

"$SCRIPT_DIR/build-uboot.sh" "${BUILD_MODE:-}"

echo
echo "All Bellatrix boot variants are ready under out/images/."
