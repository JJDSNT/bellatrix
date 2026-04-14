#!/usr/bin/env bash
# bellatrix/scripts/build.sh
#
# Compile Emu68 with VARIANT=bellatrix.
# Assumes setup.sh has been run and patches are applied.
#
# Usage:
#   ./scripts/build.sh          # build (incremental)
#   ./scripts/build.sh clean    # wipe build directory and rebuild

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

cmake "$EMU68" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL" \
    -DTARGET=raspi64 \
    -DVARIANT=bellatrix \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"

make -j"$(nproc)"
make install

hide_modified_files

echo ""
echo "Build complete. Image: $INSTALL/Emu68.img"#!/usr/bin/env bash
# bellatrix/scripts/build.sh
#
# Compile Emu68 with VARIANT=bellatrix.
# Assumes setup.sh has been run and patches are applied.
#
# Usage:
#   ./scripts/build.sh          # build (incremental)
#   ./scripts/build.sh clean    # wipe build directory and rebuild

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
EMU68="$ROOT/emu68"
BUILD="$EMU68/build-bellatrix"
INSTALL="$EMU68/install-bellatrix"
TOOLCHAIN="$EMU68/toolchains/aarch64-linux-gnu.cmake"

if [ "${1:-}" = "clean" ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD" "$INSTALL"
fi

mkdir -p "$BUILD" "$INSTALL"
cd "$BUILD"

cmake "$EMU68" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL" \
    -DTARGET=raspi64 \
    -DVARIANT=bellatrix \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"

make -j"$(nproc)"
make install

echo ""
echo "Build complete. Image: $INSTALL/Emu68.img"