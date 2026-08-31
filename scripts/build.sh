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
#   out/firmware/      SD-card payload: selected kernel, DTBs and Pi firmware
#   out/images/        Bellatrix.img with Rigel, Emu68.img without it

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
# Rigel is part of the machine now, not an experiment bolted onto it.
# CONFIG_RIGEL=0 still builds the chipset-less Emu68.img.
RIGEL="${CONFIG_RIGEL:-1}"
RIGEL_SELFTEST="${CONFIG_RIGEL_SELFTEST:-0}"
RIGEL_CHIPSET_CORE="${CONFIG_RIGEL_CHIPSET_CORE:-1}"
RIGEL_CONSOLE_SINK="${CONFIG_RIGEL_CONSOLE_SINK:-1}"

case "$RIGEL" in
    0|1) ;;
    *) echo "ERROR: CONFIG_RIGEL must be 0 or 1" >&2; exit 2 ;;
esac
case "$RIGEL_SELFTEST" in
    0|1) ;;
    *) echo "ERROR: CONFIG_RIGEL_SELFTEST must be 0 or 1" >&2; exit 2 ;;
esac
if [ "$RIGEL_SELFTEST" = 1 ] && [ "$RIGEL" != 1 ]; then
    echo "ERROR: CONFIG_RIGEL_SELFTEST=1 requires CONFIG_RIGEL=1" >&2
    exit 2
fi

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

echo "[build] configuring (TARGET=$TARGET VARIANT=$VARIANT CONFIG_RIGEL=$RIGEL CONFIG_RIGEL_SELFTEST=$RIGEL_SELFTEST)"
cmake -S "$ROOT/external/emu68" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/aarch64-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTARGET="$TARGET" \
    -DVARIANT="$VARIANT" \
    -DCONFIG_RIGEL="$RIGEL" \
    -DCONFIG_RIGEL_SELFTEST="$RIGEL_SELFTEST" \
    -DCONFIG_RIGEL_CHIPSET_CORE="$RIGEL_CHIPSET_CORE" \
    -DCONFIG_RIGEL_CONSOLE_SINK="$RIGEL_CONSOLE_SINK" \
    -DCMAKE_INSTALL_PREFIX="$FIRMWARE" \
    >/dev/null

echo "[build] compiling"
cmake --build "$BUILD" -j"$(nproc)"

echo "[build] installing firmware payload"
cmake --install "$BUILD" >/dev/null

# The build only emits the gzipped image; QEMU's -kernel and the SD card both
# want it plain.
mkdir -p "$IMAGES"
if [ "$RIGEL" = 1 ]; then
    IMAGE_NAME="Bellatrix.img"
else
    IMAGE_NAME="Emu68.img"
fi
gunzip -c "$FIRMWARE/Emu68.img.gz" > "$IMAGES/$IMAGE_NAME"
# Without the chipset the composition is already called Emu68, so the copy
# would have the same source and destination -- which cp refuses, and set -e
# turns into a failed build.
if [ "$IMAGE_NAME.gz" != "Emu68.img.gz" ]; then
    cp "$FIRMWARE/Emu68.img.gz" "$FIRMWARE/$IMAGE_NAME.gz"
fi
printf '%s\n' "$RIGEL" > "$IMAGES/Emu68.config-rigel"
printf '%s\n' "$IMAGE_NAME" > "$IMAGES/Emu68.kernel-name"

echo "[build] out/images/$IMAGE_NAME  ($(stat -c%s "$IMAGES/$IMAGE_NAME") bytes)"
