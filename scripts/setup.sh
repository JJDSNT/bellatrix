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
    local repo="$1" dir="$2" idx nf
    idx="$(mktemp)"
    GIT_INDEX_FILE="$idx" git -C "$repo" read-tree HEAD
    GIT_INDEX_FILE="$idx" git -C "$repo" add -A
    # Files the series creates are in the exclude file so they stay out of the
    # submodule's status; stage them by name so the tree still reflects them.
    nf="$(series_new_files "$dir")"
    [ -n "$nf" ] && GIT_INDEX_FILE="$idx" git -C "$repo" add -f -- $nf 2>/dev/null
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
    cur="$(worktree_tree "$repo" "$dir")"
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

# The files a series touches, read off the patches themselves.
series_files() {
    grep -h '^diff --git' "$1"/[0-9]*.patch | sed 's|^diff --git a/||; s| b/.*||' | sort -u
}

# Of those, the ones the series creates. They are untracked in the submodule
# once applied, so skip-worktree cannot quiet them — they go in the exclude
# file instead, and worktree_tree() force-stages them by name so the tree
# comparison still sees them.
series_new_files() {
    awk '/^diff --git /{f=$4; sub(/^b\//,"",f); next} /^new file mode /{print f}' \
        "$1"/[0-9]*.patch | sort -u
}

# Stop git reporting the applied series as local modifications *inside* the
# submodule. The parent repository is handled by 'ignore = dirty' in
# .gitmodules; this is the same thing one level down.
#
# The bit must come off before any reset: 'git reset --hard' silently skips
# skip-worktree entries, so leaving it set turns --reset into a no-op that
# still prints as though it worked.
#
# This only changes what git reports, never what is on disk, so series_state()
# is unaffected — it reads the working tree through a scratch index.
mark_series_files() {
    local repo="$1" dir="$2" flag="$3" f
    for f in $(series_files "$dir"); do
        git -C "$repo" update-index "$flag" "$f" 2>/dev/null || true
    done
}

# Directories of ours that are injected into a submodule as symlinks.
#
# A top-level directory named after a submodule mirrors that submodule's tree:
# aros/arch/m68k-emu68 belongs at external/aros/arch/m68k-emu68. The injection
# point is the first level that does *not* already exist upstream — `arch` does,
# `arch/m68k-emu68` does not, so that is what gets linked.
#
# Symlinked rather than copied on purpose: there is one copy of these files, and
# editing them inside the submodule tree edits this repository.
inject_dirs() {
    local repo="$1" ours="$2" rel="${3:-}" entry sub
    for entry in "$ours"/*/; do
        [ -d "$entry" ] || continue
        sub="${rel:+$rel/}$(basename "$entry")"
        if [ -e "$repo/$sub" ] && [ ! -L "$repo/$sub" ]; then
            inject_dirs "$repo" "$entry" "$sub"     # exists upstream, descend
        else
            echo "$sub"                             # injection point
        fi
    done
}

# Register everything the series adds to the submodule in its exclude file:
# the files the patches create, and the directories we inject. Must happen
# before series_state() runs: that function stages the working tree to compute
# a tree hash, and an unexcluded symlink would count as a change and make an
# otherwise correctly patched submodule look dirty.
#
# In a submodule .git is a file pointing elsewhere, so ask git for the path
# rather than assuming a directory.
register_excludes() {
    local repo="$1" dir="$2" ours="$3" sub gitdir excl
    gitdir="$(cd "$repo" && git rev-parse --absolute-git-dir)" || return 0
    excl="$gitdir/info/exclude"
    mkdir -p "$(dirname "$excl")"
    [ -f "$excl" ] || : > "$excl"

    for sub in $(series_new_files "$dir"); do
        grep -qxF "/$sub" "$excl" 2>/dev/null || echo "/$sub" >> "$excl"
    done

    [ -d "$ours" ] || return 0
    for sub in $(inject_dirs "$repo" "$ours"); do
        grep -qxF "/$sub" "$excl" 2>/dev/null || echo "/$sub" >> "$excl"
    done
}

link_injections() {
    local repo="$1" ours="$2" sub target
    for sub in $(inject_dirs "$repo" "$ours"); do
        target="$(realpath --relative-to="$repo/$(dirname "$sub")" "$ours/$sub")"
        if [ -L "$repo/$sub" ] && [ "$(readlink "$repo/$sub")" = "$target" ]; then
            echo "    linked already: $sub"
        else
            rm -rf "$repo/$sub"
            ln -s "$target" "$repo/$sub"
            echo "    linked $sub -> $target"
        fi
    done
}

unlink_injections() {
    local repo="$1" ours="$2" sub
    for sub in $(inject_dirs "$repo" "$ours"); do
        [ -L "$repo/$sub" ] && rm -f "$repo/$sub"
    done
    return 0
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

    ours="$ROOT/$name"
    register_excludes "$repo" "$dir" "$ours"

    if [ "$MODE" = reset ]; then
        echo "    resetting to pinned commit"
        mark_series_files "$repo" "$dir" --no-skip-worktree
        [ -d "$ours" ] && unlink_injections "$repo" "$ours"
        # The files the series creates are in the exclude file, so plain
        # 'git clean -fd' leaves them behind. Remove them by name rather than
        # reaching for -x, which would also delete a build tree living in the
        # submodule.
        for f in $(series_new_files "$dir"); do rm -f "$repo/$f"; done
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
            [ "$MODE" = verify ] || mark_series_files "$repo" "$dir" --skip-worktree
            ;;
        pristine)
            if [ "$MODE" = verify ]; then
                echo "    NOT APPLIED"
                failed=1
            else
                apply_series "$repo" "$dir"
                if [ "$(series_state "$repo" "$dir")" = applied ]; then
                    mark_series_files "$repo" "$dir" --skip-worktree
                else
                    echo "    ERROR: series did not produce the expected tree"
                    failed=1
                fi
            fi
            ;;
        dirty)
            echo "    ERROR: working tree differs from both the pinned commit and the series"
            echo "           local edits? re-run with --reset to discard them"
            failed=1
            ;;
    esac

    if [ -d "$ours" ] && [ "$state" != "dirty" ]; then
        case "$state:$MODE" in
            *:verify)
                for sub in $(inject_dirs "$repo" "$ours"); do
                    if [ -L "$repo/$sub" ]; then
                        echo "    linked $sub"
                    else
                        echo "    NOT LINKED $sub"
                        failed=1
                    fi
                done
                ;;
            broken:*) ;;
            *) link_injections "$repo" "$ours" ;;
        esac
    fi
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
