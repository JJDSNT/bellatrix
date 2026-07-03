#!/usr/bin/env bash
# Build cards/bellatrix.card/rtg.rom (DiagArea loader + relocated P96 card)
# using the project's Docker-based m68k toolchain.
#
# Usage: scripts/build-rtg-rom.sh [output_path]
#   output_path  defaults to cards/bellatrix.card/rtg.rom
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CARD_DIR="$REPO_ROOT/cards/bellatrix.card"
DOCKER_WRAPPER="$REPO_ROOT/emu68/build-scripts/build-m68k-amigaos"
OUTPUT="${1:-$CARD_DIR/rtg.rom}"

if [ ! -x "$DOCKER_WRAPPER" ]; then
    echo "ERROR: Docker wrapper not found at $DOCKER_WRAPPER" >&2
    exit 1
fi

echo "[rtg-rom] Step 1/2 — building bellatrix.card (m68k-amigaos-gcc)..."
(cd "$REPO_ROOT" && TTY_ENABLED="" "$DOCKER_WRAPPER" bash -c \
    "make -C cards/bellatrix.card clean all")

echo "[rtg-rom] Step 2/2 — assembling CardLoader + linking rtg.rom (vasm)..."
(cd "$REPO_ROOT" && TTY_ENABLED="" "$DOCKER_WRAPPER" bash -c \
    "make -C cards/bellatrix.card/bootrom clean all")

BUILT="$CARD_DIR/rtg.rom"
if [ ! -f "$BUILT" ]; then
    echo "ERROR: build failed, $BUILT not found" >&2
    exit 1
fi

SIZE=$(wc -c < "$BUILT")
echo "[rtg-rom] Built: $BUILT  size=$SIZE bytes"

if [ "$OUTPUT" != "$BUILT" ]; then
    cp "$BUILT" "$OUTPUT"
    echo "[rtg-rom] Copied to $OUTPUT"
fi
