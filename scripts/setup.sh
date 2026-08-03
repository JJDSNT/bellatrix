#!/usr/bin/env bash
#
# Prepare the submodules in external/ for building: check them out at their
# pinned commits and apply the patch series in patches/.
#
#   ./scripts/setup.sh            apply the series (idempotent)
#   ./scripts/setup.sh --verify   report state, exit 1 if anything is not applied
#   ./scripts/setup.sh --reset    discard submodule changes and re-apply
#
# A series lives in patches/<name>/ and belongs to external/<name>. Nothing
# here is hardcoded per submodule: adding a series means adding the directory.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCHES="$ROOT/patches"

MODE="apply"
case "${1:-}" in
    "")        MODE="apply"  ;;
    --verify)  MODE="verify" ;;
    --reset)   MODE="reset"  ;;
    *) echo "usage: $0 [--verify|--reset]" >&2; exit 2 ;;
esac

command -v git >/dev/null || { echo "ERROR: git not found" >&2; exit 1; }

# Tree hash of a submodule's working tree, computed in a scratch index so the
# real one is left alone.
worktree_tree() {
    local repo="$1" idx
    idx="$(mktemp)"
    GIT_INDEX_FILE="$idx" git -C "$repo" read-tree HEAD
    GIT_INDEX_FILE="$idx" git -C "$repo" add -A
    GIT_INDEX_FILE="$idx" git -C "$repo" write-tree
    rm -f "$idx"
}

# Tree hash the series *should* produce, derived from the patches themselves —
# so there is no expected hash to keep in sync anywhere.
#
# Patches are applied to the scratch index in numeric order because a series
# may build on itself: a later patch can edit a region an earlier one reshaped.
series_tree() {
    local repo="$1" dir="$2" idx p
    idx="$(mktemp)"
    GIT_INDEX_FILE="$idx" git -C "$repo" read-tree HEAD
    for p in "$dir"/[0-9]*.patch; do
        if ! GIT_INDEX_FILE="$idx" git -C "$repo" apply --cached "$p" 2>/dev/null; then
            rm -f "$idx"
            echo "BROKEN:$(basename "$p")"
            return 0
        fi
    done
    GIT_INDEX_FILE="$idx" git -C "$repo" write-tree
    rm -f "$idx"
}

# pristine | applied | dirty | broken:<patch>
series_state() {
    local repo="$1" dir="$2" head cur want
    head="$(git -C "$repo" rev-parse HEAD^{tree})"
    cur="$(worktree_tree "$repo")"
    want="$(series_tree "$repo" "$dir")"

    case "$want" in BROKEN:*) echo "broken:${want#BROKEN:}"; return ;; esac

    if   [ "$cur" = "$want" ]; then echo applied
    elif [ "$cur" = "$head" ]; then echo pristine
    else echo dirty
    fi
}

apply_series() {
    local repo="$1" dir="$2" p
    for p in "$dir"/[0-9]*.patch; do
        echo "    applying $(basename "$p")"
        git -C "$repo" apply "$p"
    done
}

# Submodule names are the directory names under patches/.
mapfile -t SERIES < <(find "$PATCHES" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort)
[ "${#SERIES[@]}" -gt 0 ] || { echo "no patch series found in $PATCHES" >&2; exit 1; }

failed=0

for name in "${SERIES[@]}"; do
    dir="$PATCHES/$name"
    repo="$ROOT/external/$name"

    echo "=== $name ==="

    if [ ! -e "$repo/.git" ]; then
        if [ "$MODE" = verify ]; then
            echo "    NOT INITIALISED"
            failed=1
            continue
        fi
        echo "    initialising submodule"
        git -C "$ROOT" submodule update --init "external/$name"
    fi

    # Nested submodules, if any — a missing one fails deep inside the build
    # rather than here, which is hard to trace back.
    if [ -f "$repo/.gitmodules" ] && [ "$MODE" != verify ]; then
        git -C "$repo" submodule update --init --recursive >/dev/null
    fi

    if [ "$MODE" = reset ]; then
        echo "    resetting to pinned commit"
        git -C "$repo" reset -q --hard
        git -C "$repo" clean -qfd
    fi

    state="$(series_state "$repo" "$dir")"

    case "$state" in
        broken:*)
            echo "    ERROR: ${state#broken:} does not apply to the pinned commit"
            echo "           the series has drifted from the submodule — see docs/$name.md"
            failed=1
            ;;
        applied)
            echo "    already applied ($(ls "$dir"/[0-9]*.patch | wc -l) patches)"
            ;;
        pristine)
            if [ "$MODE" = verify ]; then
                echo "    NOT APPLIED"
                failed=1
            else
                apply_series "$repo" "$dir"
                [ "$(series_state "$repo" "$dir")" = applied ] \
                    || { echo "    ERROR: series did not produce the expected tree"; failed=1; }
            fi
            ;;
        dirty)
            echo "    ERROR: working tree differs from both the pinned commit and the series"
            echo "           local edits? re-run with --reset to discard them"
            failed=1
            ;;
    esac
done

echo
if [ "$failed" -ne 0 ]; then
    echo "setup incomplete."
    exit 1
fi

case "$MODE" in
    verify) echo "all series applied." ;;
    *)      echo "setup complete." ;;
esac
