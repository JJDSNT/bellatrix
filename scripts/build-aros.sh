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
        --yes)     ASSUME_YES=1 ;;
        -h|--help) sed -n '3,23p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "usage: $0 [clean|distclean] [full] [--status] [--yes]" >&2; exit 2 ;;
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

# What the toolchain is made of, and nothing else: the two version files and
# the crosstools sources, all inside the AROS pin, plus any patch of ours that
# reaches them. It deliberately does not move when the port sources or the rest
# of the patch series change -- the most expensive thing to build is the thing
# that changes least, and a digest that moved every day would be worthless.
toolchain_digest() {
    {
        git -C "$SRC" rev-parse HEAD:config/gcc_def HEAD:config/binutils_def \
                                HEAD:tools/crosstools 2>/dev/null
        grep -l -E 'tools/crosstools|config/(gcc|binutils)_def' \
            "$ROOT"/patches/aros/[0-9]*.patch 2>/dev/null | sort |
            xargs -r sha256sum
    } | sha256sum | cut -c1-16
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

# --- status ------------------------------------------------------------------

if [ "$MODE" = status ]; then
    state="$(toolchain_state)"
    echo "target:      $TARGET"
    echo "build dir:   $BUILD"
    echo "configured:  $([ -f "$BUILD/mmake.config" ] && echo yes || echo "no — the next build reconfigures")"
    echo "ccache:      $(if grep -q 'ccache' "$BUILD/config/make.cfg" 2>/dev/null; then echo "in use"
                         elif command -v ccache >/dev/null; then echo "available, not configured into this tree"
                         else echo "not installed"; fi)"
    echo "toolchain:   $state ($(toolchain_digest))"
    [ "$state" = ready ] && echo "             $(host_tools_dir)/crosstools"
    echo "kernel ELF:  $([ -f "$BUILD/$ELF" ] && stat -c '%y' "$BUILD/$ELF" | cut -d. -f1 || echo "not built")"
    echo "dist tree:   $([ -d "$BUILD/bin/$TARGET/AROS/C" ] && echo "present ($(du -sh "$BUILD/bin/$TARGET/AROS" 2>/dev/null | cut -f1))" || echo "absent — make-sdcard.sh needs 'full'")"
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
            echo "a plain build would compile binutils and gcc first: hours, ~650 MB." ;;
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
    echo "[aros] wiping $BUILD, toolchain included"
    rm -rf "$BUILD"
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

for tool in gcc g++ make flex bison python3 gperf; do
    command -v "$tool" >/dev/null || { echo "ERROR: $tool not found" >&2; exit 1; }
done

# The expensive path says its price before charging it.
#
# `make` looks identical whether it compiles a handful of objects or builds gcc
# from source first, and the two differ by hours. An interactive caller is
# asked; a caller with no terminal -- CI, or an agent driving the shell -- is
# refused, because the failure mode there is discovering the cost afterwards.
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

# Record what the toolchain was built from, so a later `clean` can tell whether
# preserving it is sound. Written after the build because that is when it is
# true.
if stamp="$(toolchain_stamp)"; then
    toolchain_digest > "$stamp"
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
