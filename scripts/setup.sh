#!/usr/bin/env bash
# bellatrix/scripts/setup.sh
#
# Apply Bellatrix patches to submodules, ensure ignore rules
# for generated artifacts, and install build prerequisites.
#
# Usage:
#   ./scripts/setup.sh              # apply patches if not already applied (default)
#   ./scripts/setup.sh --verify     # check all patches are correctly applied; exit 1 if not
#   ./scripts/setup.sh --reset      # reset submodules to pinned commits and re-apply from scratch

set -euo pipefail

SETUP_MODE="apply"
if [ "${1:-}" = "--verify" ]; then
    SETUP_MODE="verify"
elif [ "${1:-}" = "--reset" ]; then
    SETUP_MODE="reset"
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
EMU68="$ROOT/emu68"
CHERRYUSB="$ROOT/external/cherryusb"
BTSTACK="$ROOT/external/btstack"
MUSASHI="$ROOT/external/musashi"
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

    if [ "$name" = "0002-add-bellatrix-bus-hook.patch" ]; then
        if grep -Fq 'bellatrix_bus_access((uint32_t)far, 0, size, BUS_READ);' src/aarch64/vectors.c &&
           grep -Fq 'bellatrix_init();' src/aarch64/start.c; then
            echo "Patch already applied (built-in integration detected): $name"
            return 0
        fi
    fi

    if [ "$name" = "0003-bellatrix-execution-loop.patch" ]; then
        if grep -Fq 'bellatrix_emu68_report_jit_progress(_v30_now' src/ExecutionLoop.c &&
           grep -Fq 'emu68_api_dispatch_quantum_progress(_v30_now' src/ExecutionLoop.c &&
           grep -Fq 'cpu/emu68/emu68_api.h' src/ExecutionLoop.c; then
            echo "Patch already applied (built-in integration detected): $name"
            return 0
        fi
    fi

    if [ "$name" = "0001-add-bellatrix-variant-cmake.patch" ]; then
        if grep -Fq 'set(SUPPORTED_VARIANTS "none" "pistorm" "pistorm32lite" "bellatrix")' CMakeLists.txt &&
           grep -Fq 'elseif(${VARIANT} STREQUAL "bellatrix")' CMakeLists.txt; then
            echo "Patch already applied (built-in Bellatrix variant detected): $name"
            return 0
        fi
    fi

    if [ "$name" = "0007-bellatrix-boot-sequence.patch" ]; then
        if grep -Fq 'bellatrix_core1_entry();' src/aarch64/start.c &&
           grep -Fq 'bellatrix_launch_cpu_and_park(' src/aarch64/start.c &&
           grep -Fq 'static struct M68KState bellatrix_m68k_state;' src/aarch64/start.c &&
           grep -Fq 'if (bellatrix_cpu_backend_owns_execution_loop())' src/aarch64/start.c &&
           grep -Fq '        return;' src/aarch64/start.c; then
            echo "Patch already applied (built-in boot sequence detected): $name"
            return 0
        fi
    fi

    if [ "$name" = "0019-emu68-tlsf-hardening.patch" ]; then
        if grep -Fq 'kprintf("[TLSF] ERROR! MERGE overflow:' src/tlsf.c &&
           grep -Fq 'kprintf("[TLSF] ERROR! Double-free detected' src/tlsf.c &&
           grep -Fq 'uintptr_t old_size = GET_SIZE(b);' src/tlsf.c; then
            echo "Patch already applied (TLSF hardening detected): $name"
            return 0
        fi
    fi

    if [ "$name" = "0021-emu68-public-bus-dispatch.patch" ]; then
        if grep -Fq 'BELLATRIX_HAVE_EMU68_API' src/aarch64/vectors.c &&
           grep -Fq 'emu68_api_dispatch_bus_access((uint32_t)far' src/aarch64/vectors.c &&
           grep -Fq 'EMU68_SPACE_DATA' src/aarch64/vectors.c; then
            echo "Patch already applied (built-in public bus dispatch detected): $name"
            return 0
        fi
    fi

    if git apply --check "$patch" >/dev/null 2>&1; then
        echo "Applying patch: $name"
        git apply "$patch"
        return 0
    fi

    echo "ERROR: patch does not apply cleanly: $name"
    exit 1
}

apply_submodule_patch_if_needed() {
    local repo="$1"
    local patch="$2"
    local name
    name="$(basename "$patch")"

    if [ ! -f "$patch" ]; then
        echo "Skipping missing patch: $name"
        return 0
    fi

    if [ ! -e "$repo/.git" ]; then
        echo "ERROR: submodule not initialized: $repo (run: git submodule update --init $repo)"
        exit 1
    fi

    if git -C "$repo" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "Patch already applied: $name"
        return 0
    fi

    if git -C "$repo" apply --check "$patch" >/dev/null 2>&1; then
        echo "Applying patch: $name"
        git -C "$repo" apply "$patch"
        return 0
    fi

    echo "ERROR: patch does not apply cleanly to $(basename "$repo"): $name"
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
    ensure_gitignore_entry "/build-bellatrix-rigel/"
    ensure_gitignore_entry "/install-bellatrix-rigel/"
    ensure_gitignore_entry "/build-bellatrix-rigel-musashi/"
    ensure_gitignore_entry "/install-bellatrix-rigel-musashi/"
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

EMU68_PATCHES=(
    "$PATCHES/0001-add-bellatrix-variant-cmake.patch"
    "$PATCHES/0002-add-bellatrix-bus-hook.patch"
    "$PATCHES/0003-bellatrix-execution-loop.patch"
    "$PATCHES/0007-bellatrix-boot-sequence.patch"
    "$PATCHES/0008-bellatrix-console-redirect.patch"
    "$PATCHES/0009-bellatrix-boot-config.patch"
    "$PATCHES/0010-bellatrix-z2ram-fixes.patch"
    "$PATCHES/0019-emu68-tlsf-hardening.patch"
    "$PATCHES/0020-emu68-stop-liveness.patch"
    "$PATCHES/0021-emu68-public-bus-dispatch.patch"
    "$PATCHES/0025-emu68-machine-continuation.patch"
    "$PATCHES/0026-emu68-mmu-unmap.patch"
)

CHERRYUSB_PATCHES=(
    "$PATCHES/0004-bellatrix-cherryusb-dwc2-host.patch"
)

BTSTACK_PATCHES=(
    "$PATCHES/0005-btstack-baremetal-bcm-init.patch"
)

MUSASHI_PATCHES=(
    "$PATCHES/0006-musashi-enable-instruction-hook.patch"
    "$PATCHES/0013-musashi-fsave-an-addressing-mode.patch"
    "$PATCHES/0014-musashi-fpu-test-condition-mask.patch"
    "$PATCHES/0015-musashi-fline-trap-diagnostic.patch"
    "$PATCHES/0017-musashi-fpu-missing-opmodes.patch"
    "$PATCHES/0018-musashi-widen-fpu-cpu-gate.patch"
)

LIDE_PATCHES=(
    "$PATCHES/0011-lide-deferred-cdboot.patch"
)

ODFS_PATCHES=(
    "$PATCHES/0012-odfs-cd01-romtag.patch"
)

# ---------------------------------------------------------------------------
# --verify: check all patches are correctly applied, exit 1 on failure
#
# For each patch the check runs in two tiers:
#   1. git apply --reverse --check  (strict: content + context must match)
#   2. content string-match fallback (for patches whose context has drifted
#      relative to upstream emu68; logged as "content match" with a hint to
#      regenerate the patch)
# Tier-2 matches count as OK (the patch is applied), but print a warning so
# the drift is visible. Only a complete miss is reported as FAIL.
# ---------------------------------------------------------------------------
check_emu68_patch_applied() {
    local patch="$1"
    local name
    name="$(basename "$patch")"

    if git apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "  OK  $name"
        return 0
    fi

    if [ "$name" = "0001-add-bellatrix-variant-cmake.patch" ]; then
        if grep -Fq 'set(SUPPORTED_VARIANTS "none" "pistorm" "pistorm32lite" "bellatrix")' CMakeLists.txt &&
           grep -Fq 'elseif(${VARIANT} STREQUAL "bellatrix")' CMakeLists.txt; then
            echo "  OK  $name  [content match; context drifted — regenerate patch]"
            return 0
        fi
    fi

    if [ "$name" = "0002-add-bellatrix-bus-hook.patch" ]; then
        if grep -Fq 'bellatrix_bus_access((uint32_t)far, 0, size, BUS_READ);' src/aarch64/vectors.c &&
           grep -Fq 'bellatrix_init();' src/aarch64/start.c; then
            echo "  OK  $name  [content match; context drifted — regenerate patch]"
            return 0
        fi
    fi

    if [ "$name" = "0003-bellatrix-execution-loop.patch" ]; then
        if grep -Fq 'bellatrix_emu68_report_jit_progress(_v30_now' src/ExecutionLoop.c &&
           grep -Fq 'emu68_api_dispatch_quantum_progress(_v30_now' src/ExecutionLoop.c &&
           grep -Fq 'cpu/emu68/emu68_api.h' src/ExecutionLoop.c; then
            echo "  OK  $name  [content match; context drifted — regenerate patch]"
            return 0
        fi
    fi

    if [ "$name" = "0007-bellatrix-boot-sequence.patch" ]; then
        if grep -Fq 'bellatrix_core1_entry();' src/aarch64/start.c &&
           grep -Fq 'bellatrix_launch_cpu_and_park(' src/aarch64/start.c &&
           grep -Fq 'static struct M68KState bellatrix_m68k_state;' src/aarch64/start.c &&
           grep -Fq 'if (bellatrix_cpu_backend_owns_execution_loop())' src/aarch64/start.c &&
           grep -Fq '        return;' src/aarch64/start.c; then
            echo "  OK  $name  [content match; context drifted — regenerate patch]"
            return 0
        fi
    fi

    if [ "$name" = "0019-emu68-tlsf-hardening.patch" ]; then
        if grep -Fq 'kprintf("[TLSF] ERROR! MERGE overflow:' src/tlsf.c &&
           grep -Fq 'kprintf("[TLSF] ERROR! Double-free detected' src/tlsf.c &&
           grep -Fq 'uintptr_t old_size = GET_SIZE(b);' src/tlsf.c; then
            echo "  OK  $name  [content match; whitespace normalized in patch]"
            return 0
        fi
    fi

    if [ "$name" = "0021-emu68-public-bus-dispatch.patch" ]; then
        if grep -Fq 'BELLATRIX_HAVE_EMU68_API' src/aarch64/vectors.c &&
           grep -Fq 'emu68_api_dispatch_bus_access((uint32_t)far' src/aarch64/vectors.c &&
           grep -Fq 'EMU68_SPACE_DATA' src/aarch64/vectors.c; then
            echo "  OK  $name  [content match; context drifted — regenerate patch]"
            return 0
        fi
    fi

    echo "  FAIL $name"
    return 1
}

if [ "$SETUP_MODE" = "verify" ]; then
    check_cmd git
    cd "$ROOT"
    git submodule update --init emu68 external/cherryusb external/btstack external/musashi external/rigel external/lide.device external/ODFileSystem >/dev/null 2>&1 || true

    failed=0
    echo "=== Patch verification ==="

    echo "--- emu68 ---"
    cd "$EMU68"
    for patch in "${EMU68_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        check_emu68_patch_applied "$patch" || failed=1
    done

    echo "--- external/cherryusb ---"
    for patch in "${CHERRYUSB_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        name="$(basename "$patch")"
        if git -C "$CHERRYUSB" apply --reverse --check "$patch" >/dev/null 2>&1; then
            echo "  OK  $name"
        else
            echo "  FAIL $name"
            failed=1
        fi
    done

    echo "--- external/btstack ---"
    for patch in "${BTSTACK_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        name="$(basename "$patch")"
        if git -C "$BTSTACK" apply --reverse --check "$patch" >/dev/null 2>&1; then
            echo "  OK  $name"
        else
            echo "  FAIL $name"
            failed=1
        fi
    done

    echo "--- external/musashi ---"
    for patch in "${MUSASHI_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        name="$(basename "$patch")"
        if git -C "$MUSASHI" apply --reverse --check "$patch" >/dev/null 2>&1; then
            echo "  OK  $name"
        else
            echo "  FAIL $name"
            failed=1
        fi
    done

    echo ""
    if [ "$failed" -eq 0 ]; then
        echo "All patches verified."
        exit 0
    else
        echo "ERROR: patch state mismatch — run ./scripts/setup.sh --reset to restore."
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# --reset: reset submodules to pinned commits and re-apply all patches
# ---------------------------------------------------------------------------
if [ "$SETUP_MODE" = "reset" ]; then
    check_cmd git
    echo "=== Resetting submodules to pinned commits ==="

    cd "$ROOT"

    # Clear any assume-unchanged flags set by build.sh in the parent repo
    git ls-files -v -- emu68 external/cherryusb external/btstack external/musashi 2>/dev/null \
        | awk '/^h/{print $2}' \
        | xargs -r git update-index --no-assume-unchanged || true

    # Clear assume-unchanged flags within the emu68 submodule itself (e.g. .gitignore)
    git -C "$EMU68" ls-files -v 2>/dev/null \
        | awk '/^h/{print $2}' \
        | xargs -r git -C "$EMU68" update-index --no-assume-unchanged || true

    echo "Resetting emu68..."
    git submodule update --force -- emu68

    echo "Resetting external/cherryusb..."
    git submodule update --force -- external/cherryusb

    echo "Resetting external/btstack..."
    git submodule update --force -- external/btstack

    echo "Resetting external/musashi..."
    git submodule update --force -- external/musashi

    echo "Submodules reset. Applying patches..."

    ensure_emu68_gitignore

    cd "$EMU68"
    for patch in "${EMU68_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        apply_patch_if_needed "$patch"
    done

    hide_local_gitignore_change

    cd "$ROOT"
    for patch in "${CHERRYUSB_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        apply_submodule_patch_if_needed "$CHERRYUSB" "$patch"
    done

    for patch in "${BTSTACK_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        apply_submodule_patch_if_needed "$BTSTACK" "$patch"
    done

    for patch in "${MUSASHI_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        apply_submodule_patch_if_needed "$MUSASHI" "$patch"
    done

    for patch in "${LIDE_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        apply_submodule_patch_if_needed "$ROOT/external/lide.device" "$patch"
    done

    for patch in "${ODFS_PATCHES[@]}"; do
        [ -f "$patch" ] || continue
        apply_submodule_patch_if_needed "$ROOT/external/ODFileSystem" "$patch"
    done

    install_be_stub_if_needed

    echo ""
    echo "Reset complete. Verify with: ./scripts/setup.sh --verify"
    exit 0
fi

# ---------------------------------------------------------------------------
# apply (default): idempotent — apply patches only if not already present
# ---------------------------------------------------------------------------
check_cmd git
check_cmd cmake
check_cmd aarch64-linux-gnu-gcc

cd "$ROOT"
git submodule sync -- external/cherryusb >/dev/null 2>&1 || true
git submodule update --init emu68 external/musashi external/cherryusb external/btstack external/rigel external/lide.device external/ODFileSystem

# emu68's own nested submodules — required for the bare-metal build
# (BELLATRIX_CPU_BACKEND=musashi or emu68) to configure via CMake.
# Missing these fails with "does not contain a CMakeLists.txt" deep
# into the build, not at setup time, so it's easy to miss (ISSUE-0035
# investigation, 2026-07-03).
git -C emu68 submodule update --init external/capstone external/libdeflate external/tiny-stl

ensure_emu68_gitignore

cd "$EMU68"

for patch in "${EMU68_PATCHES[@]}"; do
    [ -e "$patch" ] || continue
    apply_patch_if_needed "$patch"
done

hide_local_gitignore_change

cd "$ROOT"
for patch in "${CHERRYUSB_PATCHES[@]}"; do
    [ -f "$patch" ] || continue
    apply_submodule_patch_if_needed "$CHERRYUSB" "$patch"
done

for patch in "${BTSTACK_PATCHES[@]}"; do
    [ -f "$patch" ] || continue
    apply_submodule_patch_if_needed "$BTSTACK" "$patch"
done

for patch in "${MUSASHI_PATCHES[@]}"; do
    [ -f "$patch" ] || continue
    apply_submodule_patch_if_needed "$MUSASHI" "$patch"
done

for patch in "${LIDE_PATCHES[@]}"; do
    [ -f "$patch" ] || continue
    apply_submodule_patch_if_needed "$ROOT/external/lide.device" "$patch"
done

for patch in "${ODFS_PATCHES[@]}"; do
    [ -f "$patch" ] || continue
    apply_submodule_patch_if_needed "$ROOT/external/ODFileSystem" "$patch"
done

install_be_stub_if_needed

echo "Setup complete. Run scripts/build.sh to compile."
