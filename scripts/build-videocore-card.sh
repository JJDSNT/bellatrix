#!/usr/bin/env bash
# Build external/VideoCore.card (Emu68's VideoCore P96 driver) for the Bellatrix
# bare-metal RTG path, using the project's Docker m68k-amigaos toolchain.
#
# The card is CMake-based and links -lamiga (from the bebbo m68k-amigaos
# toolchain) plus its devicetree/unicam resource deps (nested submodules). It is
# normally built as a subdirectory of Emu68-tools, but the `amiga` link name is
# just the toolchain's libamiga — so no Emu68-tools superproject is required.
# Inside the amigadev container the default compiler IS m68k-amigaos-gcc, so no
# CMake toolchain file is needed either.
#
# See rtg-baremetal.md (Etapa 1). Runtime deps on the guest are satisfied by
# Emu68 v1.0.7 (devicetree.resource); unicam.resource is optional/unused.
#
# Usage: scripts/build-videocore-card.sh [output_dir]
#   output_dir  defaults to out/videocore/
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CARD_SRC="$REPO_ROOT/external/VideoCore.card"
OUTPUT_DIR="${1:-$REPO_ROOT/out/videocore}"

if [ ! -f "$CARD_SRC/devicetree.resource/CMakeLists.txt" ] || \
   [ ! -f "$CARD_SRC/unicam.resource/CMakeLists.txt" ] || \
   [ ! -f "$CARD_SRC/unicam.resource/mailbox.resource/CMakeLists.txt" ]; then
    echo "ERROR: VideoCore.card nested submodules missing. Run:" >&2
    echo "  git -C external/VideoCore.card submodule update --init --recursive" >&2
    exit 1
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker not found" >&2
    exit 1
fi

case "$OUTPUT_DIR" in
    "$REPO_ROOT"/*) ;;
    *)
        echo "ERROR: output_dir must be inside $REPO_ROOT" >&2
        exit 1
        ;;
esac
OUTPUT_REL="${OUTPUT_DIR#"$REPO_ROOT"/}"

# Build in the container's /tmp. CMake configure probes are unreliable when
# their build tree is a Docker/WSL bind mount.
echo "[videocore] Configuring + building VideoCore.card (m68k-amigaos, docker)..."
mkdir -p "$OUTPUT_DIR"
docker run --rm \
    -v "$REPO_ROOT:/work" \
    amigadev/crosstools:m68k-amigaos bash -c "
    cmake -S /work/external/VideoCore.card -B /tmp/videocore-build \
          -DM68K_CPU=custom -DM68K_FPU=custom -DM68K_CRT=custom \
          -DCMAKE_BUILD_TYPE=Release &&
    cmake --build /tmp/videocore-build --target VideoCore.card -j\$(nproc) &&
    cp /tmp/videocore-build/VideoCore.card /work/$OUTPUT_REL/VideoCore.card &&
    chown $(id -u):$(id -g) /work/$OUTPUT_REL/VideoCore.card
"

echo "[videocore] Built: $OUTPUT_DIR/VideoCore.card  size=$(wc -c < "$OUTPUT_DIR/VideoCore.card") bytes"
