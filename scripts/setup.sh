#!/usr/bin/env bash
# scripts/setup.sh
#
# Apply Bellatrix patches to the emu68 submodule and install build
# prerequisites. Safe to run multiple times.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
EMU68="$ROOT/emu68"
PATCHES="$ROOT/patches"

check_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "ERROR: '$1' not found. Install it first."
        exit 1
    }
}

apply_patch_if_needed() {
    local patch="$1"
    local name
    name="$(basename "$patch")"

    if git apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "Patch already applied: $name"
        return 0
    fi

    if git apply --check "$patch" >/dev/null 2>&1; then
        echo "Applying patch: $name"
        git apply "$patch"
        return 0
    fi

    echo "ERROR: patch does not apply cleanly: $name"
    exit 1
}

hide_modified_files() {
    local modified

    modified="$(git ls-files -m || true)"
    if [ -n "$modified" ]; then
        echo "Silencing patched files from git status..."
        while IFS= read -r file; do
            [ -n "$file" ] && git update-index --assume-unchanged "$file"
        done <<< "$modified"
    fi
}

check_cmd git
check_cmd cmake
check_cmd aarch64-linux-gnu-gcc

cd "$ROOT"
git submodule update --init --recursive

cd "$EMU68"

apply_patch_if_needed "$PATCHES/0001-add-bellatrix-variant-cmake.patch"
apply_patch_if_needed "$PATCHES/0002-add-bellatrix-bus-hook.patch"

hide_modified_files

STUBS=/usr/aarch64-linux-gnu/include/gnu/stubs-lp64_be.h
if [ ! -f "$STUBS" ]; then
    echo "Installing big-endian stub header..."
    sudo cp /usr/aarch64-linux-gnu/include/gnu/stubs-lp64.h "$STUBS"
fi

echo "Setup complete. Run scripts/build.sh to compile."