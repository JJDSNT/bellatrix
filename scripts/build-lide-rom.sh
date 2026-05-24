#!/usr/bin/env bash
# Build lide.rom from source using the project's Docker-based m68k toolchain.
# Produces external/lide.device/lide.rom (32 KB nibble-wide ROM).
#
# Usage: scripts/build-lide-rom.sh [output_path]
#   output_path  defaults to external/lide.device/lide.rom
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LIDE_DIR="$REPO_ROOT/external/lide.device"
DOCKER_WRAPPER="$REPO_ROOT/emu68/build-scripts/build-m68k-amigaos"
NDK_INCLUDE=/opt/m68k-amigaos/m68k-amigaos/ndk-include
OUTPUT="${1:-$LIDE_DIR/lide.rom}"

if [ ! -x "$DOCKER_WRAPPER" ]; then
    echo "ERROR: Docker wrapper not found at $DOCKER_WRAPPER" >&2
    exit 1
fi

echo "[lide-rom] Step 1/4 — building lide.device..."
# amigadev/crosstools GCC 6.5.0b doesn't pull in execbase.h / expansionbase.h
# transitively as liv2/amiga-gcc does.  We set CFLAGS inside a bash -c so the
# environment variable reaches the container (the Docker wrapper doesn't pass
# host env vars via --env).
TTY_ENABLED="" "$DOCKER_WRAPPER" bash -c \
    "CFLAGS='-include stdint.h -include exec/execbase.h -include libraries/expansionbase.h' \
     make -C '$LIDE_DIR' lide.device -s"

echo "[lide-rom] Step 2/4 — assembling bootloader (vasmm68k_mot -Fhunk)..."
mkdir -p "$LIDE_DIR/bootrom/obj"
# -Fbin causes section-overlap errors with this codebase; use -Fhunk
# then extract the raw bytes with hunk_to_bin.py.
TTY_ENABLED="" "$DOCKER_WRAPPER" vasmm68k_mot \
    -Fhunk -quiet -align \
    -DROM -DBYTEWIDE \
    -I"$NDK_INCLUDE" \
    -o "$LIDE_DIR/bootrom/obj/bootldr.hunk" \
    "$LIDE_DIR/bootrom/bootldr.S"
python3 "$SCRIPT_DIR/hunk_to_bin.py" \
    "$LIDE_DIR/bootrom/obj/bootldr.hunk" \
    "$LIDE_DIR/bootrom/obj/bootldr"

echo "[lide-rom] Step 3/4 — nibble-encoding bootloader..."
(cd "$LIDE_DIR/bootrom" && python3 mungerom.py)

echo "[lide-rom] Step 4/4 — assembling ROM image..."
python3 "$SCRIPT_DIR/make_lide_rom.py" "$LIDE_DIR" "$OUTPUT"

echo "[lide-rom] Done: $OUTPUT"
