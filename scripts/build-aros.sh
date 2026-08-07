#!/usr/bin/env bash
#
# Build the AROS m68k image for the Emu68 target. Runs setup.sh first if the
# submodules are not prepared, so a fresh clone needs nothing else.
#
#   ./scripts/build-aros.sh            incremental, ELF only
#   ./scripts/build-aros.sh clean      wipe the build directory first
#   ./scripts/build-aros.sh full       the whole distribution, not just the ELF
#
# Output: out/aros/aros-emu68-m68k.elf, and with `full` a complete
#         distribution tree under out/build/aros/bin/<target>/AROS/
#
# The first build also builds an m68k-aros cross toolchain (binutils and gcc)
# from source, which takes considerably longer than the AROS build itself.

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

# `full` builds the distribution, which is what the SD card is made from.
#
# This matters more than it looks. The lean target produces the kernel ELF and
# the modules that link into it; every *other* module -- libraries, Zune
# classes, the commands in C: -- comes from whatever tree make-sdcard.sh is
# pointed at. Until 2026-08-07 that was always the reference distribution, so a
# patch touching module code changed nothing that booted, silently. See
# AI_context/issues/ISSUE-0007.md.
#
# It drags in contrib and fetches external sources, so it is not the default.
for arg in "$@"; do
    if [ "$arg" = "full" ]; then
        METATARGET="AROS-$TARGET"
    fi
done

if [ "${1:-}" = "clean" ]; then
    # Keep the downloaded toolchain tarballs — they are ~110 MB and re-fetching
    # them is the slowest part of starting over.
    if [ -d "$BUILD/bin/Sources" ]; then
        echo "[aros] wiping build, keeping downloaded sources"
        tmp="$(mktemp -d)"
        mv "$BUILD/bin/Sources" "$tmp/"
        rm -rf "$BUILD"
        mkdir -p "$BUILD/bin"
        mv "$tmp/Sources" "$BUILD/bin/"
        rmdir "$tmp"
    else
        echo "[aros] wiping $BUILD"
        rm -rf "$BUILD"
    fi
elif [ -n "${1:-}" ] && [ "${1:-}" != "full" ]; then
    echo "usage: $0 [clean] [full]" >&2
    exit 2
fi

for tool in gcc g++ make flex bison python3 gperf; do
    command -v "$tool" >/dev/null || { echo "ERROR: $tool not found" >&2; exit 1; }
done

"$ROOT/scripts/setup.sh" --verify >/dev/null 2>&1 || "$ROOT/scripts/setup.sh"

mkdir -p "$BUILD"
cd "$BUILD"

# configure is only re-run when there is nothing to build with. It regenerates
# the whole bin/<target>/gen tree, so running it needlessly is not free.
if [ ! -f "$BUILD/mmake.config" ]; then
    echo "[aros] configuring for $TARGET"
    "$SRC/configure" --target="$TARGET"
else
    echo "[aros] already configured"
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

mkdir -p "$OUT"
cp "$BUILD/$ELF" "$OUT/"
echo "[aros] out/aros/$(basename "$ELF")  ($(stat -c%s "$OUT/$(basename "$ELF")") bytes)"
