#!/usr/bin/env bash
# bellatrix/scripts/setup.sh
#
# Apply Bellatrix patches to the emu68 submodule, ensure ignore rules
# for generated artifacts, and install build prerequisites.
# Safe to run multiple times.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
EMU68="$ROOT/emu68"
PATCHES="$ROOT/patches"
EMU68_GITIGNORE="$EMU68/.gitignore"

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

    if [ ! -f "$patch" ]; then
        echo "Skipping missing patch: $name"
        return 0
    fi

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

ensure_gitignore_entry() {
    local entry="$1"

    touch "$EMU68_GITIGNORE"

    if ! grep -Fxq "$entry" "$EMU68_GITIGNORE"; then
        echo "Adding $entry to emu68/.gitignore"
        printf '%s\n' "$entry" >> "$EMU68_GITIGNORE"
    fi
}

ensure_emu68_gitignore() {
    ensure_gitignore_entry "/build-bellatrix/"
    ensure_gitignore_entry "/install-bellatrix/"
}

hide_local_gitignore_change() {
    if [ -f "$EMU68_GITIGNORE" ]; then
        git update-index --assume-unchanged "$EMU68_GITIGNORE" || true
    fi
}

install_be_stub_if_needed() {
    local stubs
    stubs=/usr/aarch64-linux-gnu/include/gnu/stubs-lp64_be.h

    if [ ! -f "$stubs" ]; then
        echo "Installing big-endian stub header..."
        sudo cp /usr/aarch64-linux-gnu/include/gnu/stubs-lp64.h "$stubs"
    fi
}

check_cmd git
check_cmd cmake
check_cmd aarch64-linux-gnu-gcc

cd "$ROOT"
git submodule update --init --recursive

ensure_emu68_gitignore

cd "$EMU68"

for patch in "$PATCHES"/000*.patch; do
    [ -e "$patch" ] || continue
    apply_patch_if_needed "$patch"
done

hide_local_gitignore_change

install_be_stub_if_needed

echo "Setup complete. Run scripts/build.sh to compile."