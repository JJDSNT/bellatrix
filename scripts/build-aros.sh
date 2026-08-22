#!/usr/bin/env bash
#
# Build the AROS m68k image for the Emu68 target. Runs setup.sh first if the
# submodules are not prepared, so a fresh clone needs nothing else.
#
#   ./scripts/build-aros.sh            incremental, ELF only
#   ./scripts/build-aros.sh full       the whole distribution, not just the ELF
#   ./scripts/build-aros.sh clean      wipe the build, keep the cross toolchain
#   ./scripts/build-aros.sh distclean  wipe everything, toolchain included
#   ./scripts/build-aros.sh --status   report what a build would cost, build nothing
#   ./scripts/build-aros.sh --toolchain-key    print the cache key, for CI
#   ./scripts/build-aros.sh --toolchain-only   build the cross compiler alone
#
# Output: out/aros/aros-emu68-m68k.elf, and with `full` a complete
#         distribution tree under out/build/aros/bin/<target>/AROS/
#
# The first build also builds an m68k-aros cross toolchain (binutils and gcc)
# from source, which takes considerably longer than the AROS build itself --
# hours against minutes, and ~650 MB. Everything here that looks like
# bookkeeping exists to avoid paying that twice:
#
#   - `clean` keeps the toolchain; `distclean` is the one that drops it;
#   - a build that would have to make the toolchain says so and asks first,
#     and refuses outright when there is no terminal to ask;
#   - `--status` answers "what would this rebuild?" without building.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/external/aros"
BUILD="$ROOT/out/build/aros"
OUT="$ROOT/out/aros"

TARGET="${BELLATRIX_AROS_TARGET:-emu68-m68k}"
ELF="bin/$TARGET/AROS/aros-$TARGET.elf"

# Frame pointers are ON by default here, which is a project decision and not
# AROS's: every m68k target upstream passes CFLAGS_OMIT_FP. The whole target is
# built with -fno-omit-frame-pointer so KrnBacktraceFromFrame() has a chain to
# walk (patches/aros/0028).
#
# Why the default is on. Without it the diagnostics that matter here report
# nothing at all. ISSUE-0037 sat on "Backtrace (0 frames)" and an empty
# "Stack trace:" in the crash requester; with this on, the same crash produced
# nine frames naming the exact call path, from the DOS packet down to the free.
# A development build that cannot say where it broke costs more than the
# register it saves.
#
#   BELLATRIX_FRAME_POINTERS=0 ./scripts/build-aros.sh    to go back
#
# Two consequences worth knowing. Boot and throughput numbers taken with this
# on are NOT comparable with the historical record in out/boot-timing.jsonl,
# which was measured without it -- a performance comparison has to fix the
# setting on both sides. And because configure reads it, changing it can only
# take effect by reconfiguring: a tree configured one way must not be quietly
# rebuilt the other, since the flag is per-object and a half-converted tree
# yields half a backtrace. FP_STAMP is what enforces that.
export BELLATRIX_FRAME_POINTERS="${BELLATRIX_FRAME_POINTERS:-1}"
FP_STAMP="$BUILD/.bellatrix-frame-pointers"

fp_state() {
    local want="$BELLATRIX_FRAME_POINTERS"
    local have
    have="$(cat "$FP_STAMP" 2>/dev/null || echo "")"

    if [ ! -f "$BUILD/mmake.config" ]; then
        [ "$want" = 1 ] && echo "on (the next configure)" || echo "off (the next configure)"
    elif [ "$have" = "$want" ]; then
        [ "$want" = 1 ] && echo "on" || echo "off"
    else
        echo "$( [ "$have" = 1 ] && echo on || echo off ) in this tree, $( [ "$want" = 1 ] && echo on || echo off ) requested — the next build reconfigures and rebuilds"
    fi
}

# The ELF alone, not the distribution. arch/m68k-emu68/mmakefile.src has
#
#   #MM- AROS-emu68-m68k : kernel-link-emu68-m68k
#   #MM kernel-link-emu68-m68k: $(BOOTELF)
#
# so AROS-<target> only depends on the link and drags in everything else --
# contrib, boost, the lot. kernel-link-<target> is the ELF and its objects.
METATARGET="kernel-link-$TARGET"

WIPE=""
MODE="build"
ASSUME_YES="${BELLATRIX_BUILD_YES:-0}"

# `full` builds the distribution, which is what the SD card is made from.
#
# This matters more than it looks. The lean target produces the kernel ELF and
# the modules that link into it; every *other* module -- libraries, Zune
# classes, the commands in C: -- comes from whatever tree make-sdcard.sh is
# pointed at. Until 2026-08-07 that was always the reference distribution, so a
# patch touching module code changed nothing that booted, silently. See
# AI_context/consolidated/history/ISSUE-0007.md.
#
# It drags in contrib and fetches external sources, so it is not the default.
for arg in "$@"; do
    case "$arg" in
        full)      METATARGET="AROS-$TARGET" ;;
        clean)     WIPE="clean" ;;
        distclean) WIPE="distclean" ;;
        --status)  MODE="status" ;;
        --toolchain-key) MODE="key" ;;
        # Just the compiler, nothing of AROS. The toolchain workflow wants this
        # and nothing else: building the lean target afterwards would add tens
        # of minutes of CI to an artefact that does not contain any of it.
        --toolchain-only) METATARGET="tools-crosstools"; ASSUME_YES=1 ;;
        --yes)     ASSUME_YES=1 ;;
        -h|--help) sed -n '3,23p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "usage: $0 [clean|distclean] [full|--toolchain-only] [--status|--toolchain-key] [--yes]" >&2; exit 2 ;;
    esac
done

# --- the cross toolchain -----------------------------------------------------
#
# AROS builds binutils and gcc for m68k into bin/<host>/tools/crosstools and
# gates each stage on a flag file *inside that directory* --
# .installflag-gcc-<version>-<cpu>, see tools/crosstools/gnu/mmakefile.src. So
# the directory surviving a wipe is exactly what makes the rebuild be skipped.
#
# The whole of bin/<host>/tools is kept rather than crosstools alone: the ld
# wrapper next to it is patched by the same rule that touches the gcc flag, and
# keeping one without the other invites a half-state for the sake of 3 MB.
host_tools_dir() {
    local d
    for d in "$BUILD"/bin/*/tools; do
        [ -d "$d/crosstools" ] && { echo "$d"; return; }
    done
    return 1
}

# What the m68k GNU toolchain is made of, and nothing else: the two selected
# versions, their exact upstream patches and the GNU crosstools recipe, plus
# any patch of ours that reaches them. Hashing all of tools/crosstools is too
# broad: the 2026-08-21 AROS update changed only GCC 16 and LLVM 23 files and
# incorrectly invalidated the published GCC 6.5/binutils 2.32 toolchain.
# A target built with LLVM would need its own key.
# Both halves end in `|| true` deliberately: no patch of ours reaching the
# toolchain is the normal case, and grep answers "no match" with status 1. Under
# `set -e` and `pipefail` that status propagates out of the group, out of the
# pipeline, and kills the caller -- which is how the first version of this
# managed to abort the build immediately after writing the stamp.
toolchain_digest() {
    local gcc_version binutils_version digest
    gcc_version="$(git -C "$SRC" show HEAD:config/gcc_def)"
    binutils_version="$(git -C "$SRC" show HEAD:config/binutils_def)"

    digest="$({
        git -C "$SRC" rev-parse \
            HEAD:config/gcc_def \
            HEAD:config/binutils_def \
            HEAD:tools/crosstools/gnu/mmakefile.src \
            HEAD:tools/crosstools/gnu/gcc-"$gcc_version"-aros.diff \
            HEAD:tools/crosstools/gnu/binutils-"$binutils_version"-aros.diff \
            2>/dev/null || true
        grep -l -E 'tools/crosstools|config/(gcc|binutils)_def' \
            "$ROOT"/patches/aros/[0-9]*.patch 2>/dev/null | sort |
            xargs -r sha256sum || true
    } | sha256sum | cut -c1-16)"

    # Compatibility with the already-published toolchain release. The old
    # algorithm produced this key while the five effective inputs above had
    # exactly the 57f9... digest. Keeping the public key avoids a pointless
    # 175 MB re-upload and, more importantly, a multi-hour rebuild for users.
    if [ "$digest" = 57f9e2fe4ed626c1 ]; then
        echo a88db85e62ede04f
    else
        echo "$digest"
    fi
}

toolchain_stamp() {
    local tools
    tools="$(host_tools_dir)" || return 1
    echo "$tools/crosstools/.bellatrix-digest"
}

# absent | unstamped | stale | ready
#
# Only `absent` costs hours. `stale` means the toolchain was demonstrably built
# from different sources; `unstamped` means it predates this bookkeeping and
# nothing can be proved either way -- which is not the same as being wrong, so
# it is kept and the next successful build records the stamp.
toolchain_state() {
    local stamp
    stamp="$(toolchain_stamp)" || { echo absent; return; }
    # The compilers sit directly in crosstools/, not in a bin/ below it, and
    # are named after the target's cpu -- emu68-m68k builds m68k-aros-gcc.
    [ -x "$(dirname "$stamp")/${TARGET##*-}-aros-gcc" ] || { echo absent; return; }
    [ -f "$stamp" ] || { echo unstamped; return; }
    if [ "$(cat "$stamp")" = "$(toolchain_digest)" ]; then
        echo ready
    else
        echo stale
    fi
}

# --- the toolchain cache -----------------------------------------------------
#
# Three hours of gcc, or fourteen seconds of tar. The toolchain is portable in
# both directions -- built here it runs on a runner, built on a runner it runs
# here -- and two measured facts are why:
#
#   - it relocates, because gcc resolves its own prefix relative to the binary,
#     so the absolute path it was built under does not matter;
#   - the only host coupling is the C library.
#
# So the key is the digest plus where it can run, and a cached copy is looked
# up before anything is compiled. The glibc in the key is the one it was built
# against; an entry is usable on any host with that version or newer, which is
# why the lookup picks the newest compatible entry rather than demanding an
# exact match.
CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/bellatrix/toolchain"
FETCH="${BELLATRIX_TOOLCHAIN_FETCH:-1}"

# awk rather than `head -1 | grep`: head closes the pipe, ldd takes SIGPIPE, and
# under pipefail the whole pipeline fails -- which made this answer "0" now and
# then, and a host with glibc 0 matches no cached toolchain at all.
host_glibc() { ldd --version 2>/dev/null | awk 'NR == 1 { print $NF }'; }
host_arch()  { echo "$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"; }

# The target cpu is in the key because crosstools/ is scoped by host, not by
# target: AROS gates its stages per cpu inside one directory
# (.installflag-gcc-<version>-m68k), so a second AROS target would build a
# different toolchain into the same place. Without the cpu here the two would
# share a cache entry and quietly overwrite each other.
toolchain_key() {
    echo "$(toolchain_digest)-${TARGET##*-}-$(host_arch)-glibc$(host_glibc)"
}

# Reads candidate file names on stdin, writes back the newest one this host can
# actually run. Silence means nothing on offer fits.
pick_compatible() {
    local prefix ours name version
    prefix="$(toolchain_digest)-${TARGET##*-}-$(host_arch)-glibc"
    ours="$(host_glibc)"
    while read -r name; do
        case "$name" in "$prefix"*.tar.xz) ;; *) continue ;; esac
        version="${name#"$prefix"}"; version="${version%.tar.xz}"
        # usable when it was built against a glibc no newer than ours
        [ "$(printf '%s\n%s\n' "$version" "$ours" | sort -V | head -1)" = "$version" ] \
            && echo "$version $name"
    done | sort -V | tail -1 | cut -d' ' -f2-
}

# Only crosstools/ travels, never the whole of tools/.
#
# Beside crosstools sit wrappers the build generates -- <cpu>-<arch>-elf-gcc and
# the aros-ld that drives collect-aros -- and they hardcode the absolute path of
# the tree they were configured for:
#
#   exec /home/you/bellatrix/out/build/aros/bin/linux-x86_64/tools/crosstools/m68k-aros-gcc …
#
# They belong to the build, not to the toolchain: configure writes them from
# config/elf-gcc.in and friends. Packing them would make the cache portable in
# name only, so they are left out, and a tree that has lost them is sent back
# through configure to get them again.
extract_toolchain() {  # $1 = tarball
    local tools
    tools="$BUILD/bin/$(host_arch)/tools"
    mkdir -p "$tools"
    tar -C "$tools" -xJf "$1"
    [ -x "$tools/crosstools/${TARGET##*-}-aros-gcc" ] \
        || { echo "[aros] the restored toolchain has no compiler in it" >&2; return 1; }

    # A configured tree with no wrappers only happens when somebody has been
    # deleting things by hand: every path that removes tools/ removes
    # mmake.config with it. Say so and reconfigure, rather than let the build
    # fail forty seconds later with "m68k-emu68-elf-gcc: not found".
    if [ -f "$BUILD/mmake.config" ] && ! ls "$tools"/*-elf-gcc >/dev/null 2>&1; then
        echo "[aros] the tree is configured but its compiler wrappers are gone —"
        echo "       reconfiguring so they are written again"
        rm -f "$BUILD/mmake.config"
    fi
}

save_toolchain() {
    local tools key
    tools="$(host_tools_dir)" || return 0
    key="$(toolchain_key)"
    [ -f "$CACHE_DIR/$key.tar.xz" ] && return 0

    mkdir -p "$CACHE_DIR"
    echo "[aros] caching the toolchain as $key.tar.xz"
    tar -C "$tools" -cf - crosstools | xz -T0 -3 > "$CACHE_DIR/$key.tar.xz.part"
    mv "$CACHE_DIR/$key.tar.xz.part" "$CACHE_DIR/$key.tar.xz"
    ( cd "$CACHE_DIR" && sha256sum "$key.tar.xz" > "$key.tar.xz.sha256" )
}

# The published tarball, when the local cache has nothing. Named after the
# digest, so the tag is the same one the toolchain workflow creates.
fetch_toolchain() {
    local slug tag name url tmp
    [ "$FETCH" = 1 ] || return 1
    command -v curl >/dev/null || return 1

    slug="$(git -C "$ROOT" remote get-url origin 2>/dev/null |
            sed -E 's#(git@github\.com:|https://github\.com/)##; s#\.git$##')" || return 1
    [ -n "$slug" ] || return 1
    tag="toolchain-$(toolchain_digest)"

    if command -v gh >/dev/null && gh release view "$tag" >/dev/null 2>&1; then
        name="$(gh release view "$tag" --json assets -q '.assets[].name' 2>/dev/null |
                pick_compatible)"
    else
        # Without gh there is nothing to list, so ask for the exact key and let
        # the download fail if it is not there.
        name="$(toolchain_key).tar.xz"
    fi
    [ -n "$name" ] || return 1

    url="https://github.com/$slug/releases/download/$tag/$name"
    tmp="$(mktemp -d)"
    echo "[aros] fetching a prebuilt toolchain: $name"
    if ! curl -fsSL "$url" -o "$tmp/$name" ||
       ! curl -fsSL "$url.sha256" -o "$tmp/$name.sha256"; then
        rm -rf "$tmp"; return 1
    fi
    if ! ( cd "$tmp" && sha256sum -c --quiet "$name.sha256" ); then
        echo "[aros] the downloaded toolchain does not match its checksum" >&2
        rm -rf "$tmp"; return 1
    fi

    extract_toolchain "$tmp/$name" || { rm -rf "$tmp"; return 1; }
    mkdir -p "$CACHE_DIR"
    mv "$tmp/$name" "$tmp/$name.sha256" "$CACHE_DIR/"
    rm -rf "$tmp"
}

restore_toolchain() {
    local name
    name="$(ls "$CACHE_DIR" 2>/dev/null | pick_compatible)"
    if [ -n "$name" ] && [ -f "$CACHE_DIR/$name" ]; then
        echo "[aros] restoring the toolchain from $CACHE_DIR/$name"
        extract_toolchain "$CACHE_DIR/$name" && return 0
        echo "[aros] that cached copy is unusable; ignoring it" >&2
    fi
    fetch_toolchain
}

# --- status ------------------------------------------------------------------

# One implementation of the key, asked for by name. The workflows used to
# recompute it in YAML, which is how the same SIGPIPE bug came to exist in three
# places at once.
if [ "$MODE" = key ]; then
    toolchain_key
    exit 0
fi

if [ "$MODE" = status ]; then
    state="$(toolchain_state)"
    echo "target:      $TARGET"
    echo "build dir:   $BUILD"
    echo "configured:  $([ -f "$BUILD/mmake.config" ] && echo yes || echo "no — the next build reconfigures")"
    echo "ccache:      $(if grep -q 'ccache' "$BUILD/config/make.cfg" 2>/dev/null; then echo "in use"
                         elif command -v ccache >/dev/null; then echo "available, not configured into this tree"
                         else echo "not installed"; fi)"
    echo "frame ptrs:  $(fp_state)"
    echo "toolchain:   $state (key $(toolchain_key))"
    [ "$state" = ready ] && echo "             $(host_tools_dir)/crosstools"
    echo "kernel ELF:  $([ -f "$BUILD/$ELF" ] && stat -c '%y' "$BUILD/$ELF" | cut -d. -f1 || echo "not built")"
    echo "dist tree:   $([ -d "$BUILD/bin/$TARGET/AROS/C" ] && echo "present ($(du -sh "$BUILD/bin/$TARGET/AROS" 2>/dev/null | cut -f1))" || echo "absent — make-sdcard.sh needs 'full'")"
    cached="$(ls "$CACHE_DIR" 2>/dev/null | grep -c '\.tar\.xz$' || true)"
    echo "cache:       ${cached:-0} in $CACHE_DIR$([ "${cached:-0}" -gt 0 ] && echo " ($(du -sh "$CACHE_DIR" 2>/dev/null | cut -f1))")"
    [ -n "$(ls "$CACHE_DIR" 2>/dev/null | pick_compatible)" ] \
        && echo "             usable here: $(ls "$CACHE_DIR" | pick_compatible)"
    echo -n "submodules:  "
    if "$ROOT/scripts/setup.sh" --verify >/dev/null 2>&1; then
        echo "verified"
    else
        echo "NOT VERIFIED — a build would stop here; run ./scripts/setup.sh --verify"
    fi
    echo
    case "$state" in
        ready)
            echo "a plain build would compile AROS only: minutes." ;;
        unstamped)
            echo "a plain build would compile AROS only: minutes."
            echo "the toolchain predates the digest stamp, so it cannot be proved to match"
            echo "these sources; the next successful build records it." ;;
        stale)
            echo "a plain build would compile AROS only: minutes, but the toolchain was"
            echo "built from different sources — 'clean' will drop it and rebuild." ;;
        absent)
            if [ -n "$(ls "$CACHE_DIR" 2>/dev/null | pick_compatible)" ]; then
                echo "a plain build would restore the toolchain from the cache (seconds), then"
                echo "compile AROS: minutes."
            else
                echo "a plain build would look for a prebuilt toolchain and, finding none,"
                echo "compile binutils and gcc first: hours, ~650 MB."
            fi ;;
    esac
    exit 0
fi

# --- wiping ------------------------------------------------------------------
#
# `clean` means "build it again", not "pay for the toolchain again". The
# distinction is worth a separate verb because the two differ by hours: the
# previous version of this script kept the ~110 MB of downloaded tarballs and
# deleted the 650 MB of compiled toolchain, which is the wrong half of the cost.
if [ "$WIPE" = distclean ]; then
    # distclean is the verb for "start over". Leaving a cached copy of the very
    # thing being discarded, ready to be restored on the next build, would make
    # it mean nothing.
    echo "[aros] wiping $BUILD, toolchain included"
    rm -rf "$BUILD"
    rm -f "$CACHE_DIR/$(toolchain_digest)-${TARGET##*-}"-*.tar.xz \
          "$CACHE_DIR/$(toolchain_digest)-${TARGET##*-}"-*.sha256
elif [ "$WIPE" = clean ] && [ -d "$BUILD" ]; then
    # The host directory whose tools/ survives, empty when nothing is worth
    # keeping. Everything else under out/build/aros goes.
    keep_host=""
    case "$(toolchain_state)" in
        ready|unstamped) keep_host="$(basename "$(dirname "$(host_tools_dir)")")" ;;
        stale) echo "[aros] the toolchain was built from different sources — not preserving it" ;;
    esac

    # Delete by exception rather than move-and-restore: no temporary copy of
    # 650 MB, and nothing left stranded if the wipe is interrupted.
    find "$BUILD" -mindepth 1 -maxdepth 1 ! -name bin -exec rm -rf {} +

    if [ -d "$BUILD/bin" ]; then
        if [ -n "$keep_host" ]; then
            find "$BUILD/bin" -mindepth 1 -maxdepth 1 \
                ! -name Sources ! -name "$keep_host" -exec rm -rf {} +
            find "$BUILD/bin/$keep_host" -mindepth 1 -maxdepth 1 \
                ! -name tools -exec rm -rf {} +
            echo "[aros] wiped the build, kept the toolchain and the downloaded sources"
        else
            find "$BUILD/bin" -mindepth 1 -maxdepth 1 ! -name Sources -exec rm -rf {} +
            echo "[aros] wiped the build, kept the downloaded sources"
        fi
    fi
fi

# What AROS's own configure insists on, checked here because it says so in one
# second and configure says so several minutes in, after the toolchain search
# and half the environment probing. pngtopnm and ppmtoilbm are the ones nobody
# expects: AROS converts its boot images at build time.
for tool in gcc g++ make flex bison python3 gperf gawk patch pngtopnm ppmtoilbm; do
    command -v "$tool" >/dev/null || {
        echo "ERROR: $tool not found" >&2
        case "$tool" in
            pngtopnm|ppmtoilbm) echo "       it comes from the netpbm package" >&2 ;;
        esac
        exit 1
    }
done

# configure also wants a python module, and says so only after several minutes
# of probing. The message it gives -- "failed to detect mako templates" -- does
# not name a package either.
python3 -c 'import mako' 2>/dev/null || {
    echo "ERROR: the python mako module is missing (package python3-mako)" >&2
    exit 1
}

# The expensive path says its price before charging it.
#
# `make` looks identical whether it compiles a handful of objects or builds gcc
# from source first, and the two differ by hours. An interactive caller is
# asked; a caller with no terminal -- CI, or an agent driving the shell -- is
# refused, because the failure mode there is discovering the cost afterwards.
if [ "$(toolchain_state)" = absent ]; then
    restore_toolchain || true
fi

if [ "$(toolchain_state)" = absent ] && [ "$ASSUME_YES" != 1 ]; then
    echo
    echo "[aros] there is no m68k cross toolchain in this build tree."
    echo "       This run would build binutils 2.32 and gcc 6.5.0 from source"
    echo "       before compiling any AROS: hours, and ~650 MB under out/build."
    echo
    if [ -t 0 ]; then
        read -r -p "       Build it now? [y/N] " reply
        case "$reply" in
            [yY]|[yY][eE][sS]) ;;
            *) echo "[aros] nothing built."; exit 1 ;;
        esac
    else
        echo "       Re-run with --yes (or BELLATRIX_BUILD_YES=1) to accept the cost."
        echo "       ./scripts/build-aros.sh --status reports this without building."
        exit 1
    fi
fi

"$ROOT/scripts/setup.sh" --verify >/dev/null 2>&1 || "$ROOT/scripts/setup.sh"

mkdir -p "$BUILD"
cd "$BUILD"

if [ -f "$BUILD/mmake.config" ] && \
   [ "$(cat "$FP_STAMP" 2>/dev/null || echo 0)" != "$BELLATRIX_FRAME_POINTERS" ]; then
    echo "[aros] BELLATRIX_FRAME_POINTERS changed to $BELLATRIX_FRAME_POINTERS —"
    echo "[aros] reconfiguring, which rebuilds the tree (the toolchain is kept)"
    rm -f "$BUILD/mmake.config"
fi

# configure is only re-run when there is nothing to build with. It regenerates
# the whole bin/<target>/gen tree, so running it needlessly is not free.
if [ ! -f "$BUILD/mmake.config" ]; then
    # --enable-ccache when ccache is installed. It is offered here and never
    # forced on an existing tree: turning it on means reconfiguring, and a
    # reconfigure regenerates gen/ and rebuilds everything -- paying a large
    # cost now for a smaller one later is a choice, not a side effect of
    # running the usual build. An existing tree adopts it at its next
    # `clean`/`distclean`, or never.
    CONFIGURE_ARGS=(--target="$TARGET")
    if command -v ccache >/dev/null; then
        CONFIGURE_ARGS+=(--enable-ccache)
        echo "[aros] configuring for $TARGET (with ccache)"
    else
        echo "[aros] configuring for $TARGET"
    fi
    "$SRC/configure" "${CONFIGURE_ARGS[@]}"
    echo "$BELLATRIX_FRAME_POINTERS" > "$FP_STAMP"
else
    echo "[aros] already configured"

    # setup.sh --reset checks the submodule out again, which rewrites every
    # file's mtime without changing a byte of it -- the content is pinned by
    # the submodule commit. AROS's Makefile then sees configure newer than
    # config.status and refuses to build at all:
    #
    #   **** The configure script must be executed before running 'make'.
    #
    # Touching config.status clears that, but on its own it is expensive:
    # config/make.cfg is generated from it, every mmakefile.src includes
    # config/aros.cfg which includes make.cfg, and the whole tree goes out of
    # date. That is why builds here were rebuilding everything after a reset.
    #
    # So bump the generated files past it in the same breath. Sound because the
    # trigger is an mtime with identical content, which is the only kind of
    # change a submodule checkout can produce.
    if [ -f "$BUILD/config.status" ] && \
       [ "$SRC/configure" -nt "$BUILD/config.status" ]; then
        echo "[aros] configure is newer only by mtime — keeping the build tree"
        touch "$BUILD/config.status"
        for cfg in "$BUILD/config/make.cfg" "$BUILD/compiler/include/geninc.cfg"; do
            [ -f "$cfg" ] && touch "$cfg"
        done
    fi
fi

# Deliberately serial.
#
# AROS's mmake does not order the crosstools stage against generation of the
# target headers. Under 'make -j' the gcc stage can be configured before
# bin/<target>/AROS/Developer/include is populated, and gcc's configure then
# fails its int64_t check against the half-built sysroot:
#
#   configure: error: error verifying int64_t uses long long
#
# The failure is silent about its real cause and lands ~15 minutes in, so it is
# worth not inviting. If this is ever parallelised, it has to be with an
# explicit dependency, not with -j.
echo "[aros] building $METATARGET (serial — see comment in this script)"
make "$METATARGET"

# The display driver is not in the ELF any more.
#
# vcgfx installs the way AROS installs display drivers -- DEVS:Drivers plus a
# DEVS:Monitors loader -- so it hangs off the distribution target, not off the
# link. A lean build would then leave whatever .hidd happened to be in the tree
# on the card, and a boot would run a driver older than the source, silently.
# That already happened once here, and it is the same trap CLAUDE.md records
# for the other modules: if a change is not in the kernel ELF, check where the
# module on the card came from.
#
# Building it after the link costs seconds and removes the question.
if [ "$METATARGET" = "kernel-link-$TARGET" ]; then
    echo "[aros] building the disk-installed drivers"
    make kernel-m68k-emu68-vcgfx
    # The USB host controller is disabled for now -- see the comment on the
    # kernel-usb alias in arch/m68k-emu68/mmakefile.src. Still built, because
    # code that stops being compiled stops being code; just not installed.
    make kernel-usb-dwc2emu68
    # The HCI transport is a disk module too, and for the same reason: the
    # Bluetooth stack loads it from DEVS:Bluetooth at runtime.
    make kernel-bthciuart
fi

# Record what the toolchain was built from, so a later `clean` can tell whether
# preserving it is sound. Written after the build because that is when it is
# true.
if stamp="$(toolchain_stamp)"; then
    toolchain_digest > "$stamp"
    save_toolchain
fi

# A good kickstart ELF has no unresolved symbols at all — measured, not assumed.
#
# This is not belt-and-braces. On 2026-08-07 a full rebuild let arch/m68k-all's
# Exec backend win a race against ours (they build into the same object path;
# see patches/aros/0018), and the ELF shipped with m68k_SwitchTail and
# m68k_DispatchFrame unresolved because our kernel_cpu.c removes them on
# purpose. The guest jumped to address zero 570 ms in. Emu68's loader printed
# "[ELF] Undefined symbol ..." at load time and nobody was reading it, so the
# failure was diagnosed from a register dump instead of from the sentence that
# named it.
#
# Fail here, where it is cheap, rather than 45 minutes into a boot harness.
if [ "$METATARGET" = "tools-crosstools" ]; then
    echo "[aros] the cross toolchain is built; nothing of AROS was."
    exit 0
fi

NM="$BUILD/bin/linux-x86_64/tools/crosstools/m68k-aros-nm"
if [ -x "$NM" ]; then
    undef="$("$NM" -u "$BUILD/$ELF" 2>/dev/null || true)"
    if [ -n "$undef" ]; then
        echo "ERROR: $ELF has unresolved symbols — it will not run:" >&2
        echo "$undef" | sed 's/^/    /' >&2
        exit 1
    fi
fi

mkdir -p "$OUT"
cp "$BUILD/$ELF" "$OUT/"
echo "[aros] out/aros/$(basename "$ELF")  ($(stat -c%s "$OUT/$(basename "$ELF")") bytes)"
