#!/usr/bin/env bash
# Build ODFileSystem handler binary (m68k) using the project's Docker toolchain.
# Produces external/ODFileSystem/build/amiga/ODFileSystem
#
# Usage: scripts/build-odfs.sh [output_path]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ODFS_DIR="$REPO_ROOT/external/ODFileSystem"
DOCKER_WRAPPER="$REPO_ROOT/emu68/build-scripts/build-m68k-amigaos"
COMPAT_HDR="$SCRIPT_DIR/odfs_crosstools_compat.h"
OUTPUT="${1:-$ODFS_DIR/build/amiga/ODFileSystem}"

if [ ! -x "$DOCKER_WRAPPER" ]; then
    echo "ERROR: Docker wrapper not found at $DOCKER_WRAPPER" >&2
    exit 1
fi

if [ ! -f "$COMPAT_HDR" ]; then
    echo "ERROR: compat header not found at $COMPAT_HDR" >&2
    exit 1
fi

# Minimal ISO-9660/RR/Joliet feature set — enough for Amiga ISOs and AROS.
# UDF, HFS, CDDA disabled to keep binary size down.
FEATURES="\
 FEATURE_ISO9660=1 \
 FEATURE_ROCK_RIDGE=1 \
 FEATURE_JOLIET=1 \
 FEATURE_MULTISESSION=1 \
 FEATURE_UDF=0 \
 FEATURE_HFS=0 \
 FEATURE_HFSPLUS=0 \
 FEATURE_CDDA=0"

# Full CFLAGS override:
#  - Remove -Werror (crosstools NDK has minor incompatibilities)
#  - Add -include for our compatibility shim (fixes UtilityBase + TD_READ64)
#  - Disable serial debug and logging (reduce binary size)
CFLAGS="-Os -m68000 -mtune=68020-60 -msoft-float -noixemul -nostartfiles \
 -Wall -Wextra -Wno-error \
 -Wstrict-prototypes -Wmissing-prototypes \
 -Wno-array-bounds -Wno-unused-parameter \
 -DAMIGA \
 -DODFS_AMIGA_DATE=\\\"bellatrix\\\" \
 -DODFS_GIT_VERSION=\\\"embedded\\\" \
 -DODFS_SERIAL_DEBUG=0 \
 -DODFS_FEATURE_LOG=0 \
 -DODFS_FEATURE_ISO9660=1 \
 -DODFS_FEATURE_ROCK_RIDGE=1 \
 -DODFS_FEATURE_JOLIET=1 \
 -DODFS_FEATURE_MULTISESSION=1 \
 -DODFS_FEATURE_UDF=0 \
 -DODFS_FEATURE_HFS=0 \
 -DODFS_FEATURE_HFSPLUS=0 \
 -DODFS_FEATURE_CDDA=0 \
 -DODFS_FEATURE_CACHE_BLOCK=1 \
 -DODFS_FEATURE_CACHE_META=0 \
 -DODFS_FEATURE_CACHE_STREAM=0 \
 -include $COMPAT_HDR"

echo "[build-odfs] Building ODFileSystem (m68k)..."
(cd "$REPO_ROOT" && TTY_ENABLED="" "$DOCKER_WRAPPER" bash -c \
    "make -C external/ODFileSystem amiga \
        CFLAGS='$CFLAGS' \
        $FEATURES \
        SERIAL_DEBUG=0 \
        AMIGA_BUILD=build/amiga \
        2>&1")

BUILT="$ODFS_DIR/build/amiga/ODFileSystem"
if [ ! -f "$BUILT" ]; then
    echo "ERROR: build failed, $BUILT not found" >&2
    exit 1
fi

SIZE=$(wc -c < "$BUILT")
echo "[build-odfs] Built: $BUILT  size=$SIZE bytes"

if [ "$OUTPUT" != "$BUILT" ]; then
    cp "$BUILT" "$OUTPUT"
    echo "[build-odfs] Copied to $OUTPUT"
fi
