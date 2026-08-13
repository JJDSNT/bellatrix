#!/usr/bin/env bash
#
# Build the bare-metal image. Runs setup.sh first if the patch series is not
# applied yet, so a fresh clone needs nothing else.
#
#   ./scripts/build.sh          incremental
#   ./scripts/build.sh clean    wipe the build directory first
#
# Everything lands under out/ at the project root:
#
#   out/build/emu68/   cmake build tree
#   out/firmware/      SD-card payload: Emu68.img.gz, DTBs, start.elf, config.txt
#   out/images/        Emu68.img, uncompressed, for QEMU and for flashing

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/out/build/emu68"
FIRMWARE="$ROOT/out/firmware"
IMAGES="$ROOT/out/images"

TARGET="${BELLATRIX_TARGET:-raspi64}"

# The Bellatrix variant is the machine this repository is about: it compiles
# src/machine and lets it decide the memory policy instead of inheriting the
# host's flat memory (patches/emu68/0006, cmake/bellatrix-variant.cmake).
#
# BELLATRIX_VARIANT=none still builds stock Emu68, which is what an A/B against
# the unmodified upstream behaviour needs.
VARIANT="${BELLATRIX_VARIANT:-bellatrix}"

if [ "${1:-}" = "clean" ]; then
    echo "[build] wiping $BUILD"
    rm -rf "$BUILD"
elif [ -n "${1:-}" ]; then
    echo "usage: $0 [clean]" >&2
    exit 2
fi

for tool in cmake make aarch64-linux-gnu-gcc; do
    command -v "$tool" >/dev/null || { echo "ERROR: $tool not found" >&2; exit 1; }
done

# Emu68 needs a big-endian variant of this header and the cross toolchain does
# not ship one. It is a copy of the little-endian stub; only its presence
# matters.
BE_STUB=/usr/aarch64-linux-gnu/include/gnu/stubs-lp64_be.h
if [ ! -f "$BE_STUB" ]; then
    echo "[build] installing big-endian stub header (needs sudo)"
    sudo cp /usr/aarch64-linux-gnu/include/gnu/stubs-lp64.h "$BE_STUB"
fi

"$ROOT/scripts/setup.sh" --verify >/dev/null 2>&1 || "$ROOT/scripts/setup.sh"

echo "[build] configuring (TARGET=$TARGET VARIANT=$VARIANT)"
cmake -S "$ROOT/external/emu68" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/aarch64-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTARGET="$TARGET" \
    -DVARIANT="$VARIANT" \
    -DCMAKE_INSTALL_PREFIX="$FIRMWARE" \
    >/dev/null

echo "[build] compiling"
cmake --build "$BUILD" -j"$(nproc)"

echo "[build] installing firmware payload"
cmake --install "$BUILD" >/dev/null

# The build only emits the gzipped image; QEMU's -kernel and the SD card both
# want it plain.
mkdir -p "$IMAGES"
gunzip -c "$FIRMWARE/Emu68.img.gz" > "$IMAGES/Emu68.img"

echo "[build] out/images/Emu68.img  ($(stat -c%s "$IMAGES/Emu68.img") bytes)"
