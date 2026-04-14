#!/usr/bin/env bash
# scripts/setup.sh
#
# Apply Bellatrix patches to the emu68 submodule and install build
# prerequisites. Run once after 'git submodule update --init'.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
EMU68="$ROOT/emu68"
PATCHES="$ROOT/patches"

# ---- Prerequisites check ----
check_cmd() {
    command -v "$1" &>/dev/null || { echo "ERROR: '$1' not found. Install it first."; exit 1; }
}
check_cmd git
check_cmd cmake
check_cmd aarch64-linux-gnu-gcc

# ---- Submodule init ----
cd "$ROOT"
git submodule update --init --recursive

# ---- Apply patches (idempotent) ----
cd "$EMU68"
if git log --oneline HEAD | grep -q "bellatrix"; then
    echo "Patches already applied — skipping."
else
    echo "Applying Bellatrix patches..."
    git apply "$PATCHES/0001-add-bellatrix-variant-cmake.patch"
    git apply "$PATCHES/0002-add-bellatrix-bus-hook.patch"
    echo "Patches applied."
fi

# ---- Big-endian stub header ----
STUBS=/usr/aarch64-linux-gnu/include/gnu/stubs-lp64_be.h
if [ ! -f "$STUBS" ]; then
    echo "Installing big-endian stub header..."
    sudo cp /usr/aarch64-linux-gnu/include/gnu/stubs-lp64.h "$STUBS"
fi

echo "Setup complete. Run scripts/build.sh to compile."
