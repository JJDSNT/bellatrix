#!/usr/bin/env bash
# Build cdmount.rom using the project's Docker-based m68k toolchain.
# Usage: src/machine/boards/resident/build.sh [clean]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
DOCKER_WRAPPER="${REPO_ROOT}/emu68/build-scripts/build-m68k-amigaos"

if [ ! -x "$DOCKER_WRAPPER" ]; then
    echo "ERROR: Docker wrapper not found at $DOCKER_WRAPPER" >&2
    exit 1
fi

TTY_ENABLED="" "$DOCKER_WRAPPER" make -C "$SCRIPT_DIR" "$@"
