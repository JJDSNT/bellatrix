#!/usr/bin/env bash
#
# Build, verify and publish a Bellatrix release.
#
#   ./scripts/release.sh v0.1.0                 build, verify, publish
#   ./scripts/release.sh v0.1.0 --dry-run       everything except publishing
#   ./scripts/release.sh v0.1.0 --skip-build    use what is already built
#   ./scripts/release.sh v0.1.0 --no-publish    produce the assets, publish nothing
#   ./scripts/release.sh --check DIR            verify assets somebody else built
#
# Three assets come out of it, in out/release:
#
#   bellatrix-<tag>-pi3.tar.xz   the whole card, for a first installation
#   Bellatrix.img.gz             the aarch64 kernel, to update in place
#   aros-emu68-m68k.elf          the m68k system, to update in place
#   bellatrix-<tag>-qemu.tar.xz  the same system, runnable without a Pi
#
# The two loose files keep the exact names config.txt declares, because updating
# a card has to be a copy and never a rename. Mixing versions is supported, not
# gated: docs/Compat.md asks the resident system to boot volumes it was never
# built alongside, so this script records what a card is and refuses nothing.
#
# It runs the same way on a workstation and on a runner. The only difference is
# who calls it: locally it does the lot, and in CI the workflow can split it
# with --skip-build (the build happened in an earlier job) or --no-publish (the
# workflow uploads the assets itself). Nothing here reads GITHUB_* or assumes a
# checkout layout other than this repository's.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/out/build/aros"
DIST="$BUILD/bin/emu68-m68k/AROS"
FIRMWARE="$ROOT/out/firmware"
RELEASE="$ROOT/out/release"

TAG=""
DRY_RUN=0
SKIP_BUILD=0
NO_PUBLISH=0
DRAFT=0
NOTES=""
CHECK_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)    DRY_RUN=1; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --no-publish) NO_PUBLISH=1; shift ;;
        --draft)      DRAFT=1; shift ;;
        --notes)      NOTES="$2"; shift 2 ;;
        --check)      CHECK_DIR="$2"; shift 2 ;;
        -h|--help)    sed -n '3,26p' "$0" | sed 's/^# \?//'; exit 0 ;;
        -*)           echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
        *)            TAG="$1"; shift ;;
    esac
done

say()  { echo "[release] $*"; }
die()  { echo "[release] ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------- verification
#
# The part that makes a release trustworthy, and the reason it is a function
# rather than a step: a workflow that builds elsewhere can still call
# `release.sh --check` on what it downloaded, and get the same answers.
#
# Everything here has cost something already. The archive's paths must be
# relative, because `tar -xJf … -C /media/…` is how a card gets filled and an
# absolute path would write somewhere else entirely. Devs/DOSDrivers/AUX has to
# survive because mtools refuses that name and the staging step drops it for the
# image -- it went missing from the archive once. And the names on the kernel=
# and initramfs lines have to exist, because a card whose config.txt names a
# file that is not there answers with seven blinks of the green LED and nothing
# on the serial line.
verify_archive() {
    local archive="$1" listing missing=()
    [ -f "$archive" ] || die "no archive at $archive"

    listing="$(tar -tJf "$archive")" || die "$archive is not readable as tar.xz"

    if grep -qE '^/|(^|/)\.\./' <<<"$listing"; then
        die "$archive contains absolute or parent-relative paths"
    fi

    # The kernel and the m68k system are deliberately absent from this list:
    # they are checked below against the names config.txt gives them, which is
    # the only authority on what the card actually boots.
    local required=(
        ./config.txt ./cmdline.txt ./version.txt
        ./bootcode.bin ./start.elf ./fixup.dat
        ./AROS.boot ./Devs/DOSDrivers/AUX ./S/Startup-Sequence
    )
    local entry
    for entry in "${required[@]}"; do
        grep -qxF "$entry" <<<"$listing" || missing+=("${entry#./}")
    done
    grep -qE '^\./bcm2710-.*\.dtb$' <<<"$listing" || missing+=("bcm2710-*.dtb")

    local dir
    for dir in C S Libs Devs L Classes Fonts System Prefs Storage Utilities Tools Locale; do
        grep -qE "^\./$dir/" <<<"$listing" || missing+=("$dir/")
    done

    [ "${#missing[@]}" -eq 0 ] || die "$archive is missing: ${missing[*]}"

    # config.txt names the two loose assets. If it ever names something else,
    # in-place updates break silently -- the card keeps booting the old file
    # while the person believes they replaced it.
    local cfg kernel initramfs
    cfg="$(tar -xJOf "$archive" ./config.txt)"
    kernel="$(sed -n 's/^kernel=\(.*\)$/\1/p' <<<"$cfg" | tr -d '\r')"
    initramfs="$(sed -n 's/^initramfs \([^ ]*\).*$/\1/p' <<<"$cfg" | tr -d '\r')"

    [ -n "$kernel" ]    || die "config.txt in $archive has no kernel= line"
    [ -n "$initramfs" ] || die "config.txt in $archive has no initramfs line"
    grep -qxF "./$kernel" <<<"$listing" \
        || die "config.txt names kernel=$kernel, which is not in the archive"
    grep -qxF "./$initramfs" <<<"$listing" \
        || die "config.txt names initramfs $initramfs, which is not in the archive"

    echo "$kernel $initramfs"
}

# The loose assets are the files config.txt asked for, under those exact names.
verify_assets() {
    local dir="$1" archive names kernel initramfs
    archive="$(ls "$dir"/bellatrix-*-pi3.tar.xz 2>/dev/null | head -1)" \
        || die "no bellatrix-*-pi3.tar.xz in $dir"
    [ -n "$archive" ] || die "no bellatrix-*-pi3.tar.xz in $dir"

    names="$(verify_archive "$archive")"
    kernel="${names%% *}"
    initramfs="${names##* }"

    [ -f "$dir/$kernel" ]    || die "$kernel is not beside the archive in $dir"
    [ -f "$dir/$initramfs" ] || die "$initramfs is not beside the archive in $dir"

    # The loose file and the copy inside the archive have to be the same build.
    # They are produced from one tree, so a mismatch means the assets were
    # assembled from two, which is the failure this check exists to catch.
    local f
    for f in "$kernel" "$initramfs"; do
        cmp -s <(tar -xJOf "$archive" "./$f") "$dir/$f" \
            || die "$f differs from the copy inside the archive"
    done

    # The QEMU bundle stands or falls on the four files its launcher names.
    local qemu
    qemu="$(ls "$dir"/bellatrix-*-qemu.tar.xz 2>/dev/null | head -1 || true)"
    if [ -n "$qemu" ]; then
        local qlisting qmissing=() qlauncher qkernel qinitrd
        qlisting="$(tar -tJf "$qemu")" || die "$qemu is not readable as tar.xz"
        for f in ./sd.img ./bcm2710-rpi-3-b.dtb ./run.sh ./run.bat ./README.txt; do
            grep -qxF "$f" <<<"$qlisting" || qmissing+=("${f#./}")
        done
        [ "${#qmissing[@]}" -eq 0 ] || die "$(basename "$qemu") is missing: ${qmissing[*]}"

        # run.sh is the bundle's authority on what it boots, the way config.txt
        # is the card's. Reading the names back means neither this check nor the
        # bundle has to know them in advance.
        qlauncher="$(tar -xJOf "$qemu" ./run.sh)"
        qkernel="$(sed -n 's|.*-kernel "$here/\([^"]*\)".*|\1|p' <<<"$qlauncher")"
        qinitrd="$(sed -n 's|.*-initrd "$here/\([^"]*\)".*|\1|p' <<<"$qlauncher")"
        [ -n "$qkernel" ] && [ -n "$qinitrd" ] \
            || die "run.sh in the QEMU bundle names no kernel or no initrd"
        grep -qxF "./$qkernel" <<<"$qlisting" \
            || die "run.sh names -kernel $qkernel, which is not in the bundle"
        grep -qxF "./$qinitrd" <<<"$qlisting" \
            || die "run.sh names -initrd $qinitrd, which is not in the bundle"

        # -kernel wants a raw image: the gzip the card carries would not boot.
        tar -xJOf "$qemu" "./$qkernel" | head -c2 | grep -q $'\x1f\x8b' \
            && die "$qkernel in the QEMU bundle is gzipped; QEMU will not unpack it"
        say "verified: $(basename "$qemu")"
    fi

    ( cd "$dir" && sha256sum -c --quiet ./*.sha256 ) \
        || die "a checksum in $dir does not match"

    say "verified: $(basename "$archive"), $kernel, $initramfs"
}

if [ -n "$CHECK_DIR" ]; then
    verify_assets "$CHECK_DIR"
    exit 0
fi

# -------------------------------------------------------------------- preflight
#
# Nothing is built before this. Failing after an hour of compiling because of an
# uncommitted file is the worst available outcome, and every check here costs
# less than a second.
[ -n "$TAG" ] || die "no tag given (try --help)"

case "$TAG" in
    v[0-9]*|[0-9]*) ;;
    *) die "tag '$TAG' does not look like a version (v1.2.3 or 1.2.3)" ;;
esac

say "preflight for $TAG"

[ -z "$(git -C "$ROOT" status --porcelain)" ] \
    || die "the working tree has uncommitted changes; a release must name a commit"

"$ROOT/scripts/setup.sh" --verify >/dev/null 2>&1 \
    || die "submodules are not as expected; run ./scripts/setup.sh --verify"

if [ "$NO_PUBLISH" = 0 ] && [ "$DRY_RUN" = 0 ]; then
    command -v gh >/dev/null || die "gh is not installed, and publishing needs it"
    gh auth status >/dev/null 2>&1 || die "gh is not authenticated"
fi

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD)"
say "at $COMMIT, submodules verified"

# ------------------------------------------------------------------------ build
if [ "$SKIP_BUILD" = 0 ]; then
    say "building Emu68"
    "$ROOT/scripts/build.sh"

    # The card is made from the distribution tree, which only the full build
    # produces. The lean target would leave every library, Zune class and C:
    # command at whatever the tree happened to hold.
    say "building AROS (full)"
    "$ROOT/scripts/build-aros.sh" full
else
    say "skipping the build, using what is there"
fi

[ -d "$DIST/C" ] || die "no distribution tree at $DIST; run build-aros.sh full"

# -------------------------------------------------------------------- assemble
rm -rf "$RELEASE"
mkdir -p "$RELEASE"

ARCHIVE="$RELEASE/bellatrix-$TAG-pi3.tar.xz"

# The card ships with the chipset built in and switched off.
#
# Those are different questions and a release is where the difference earns its
# keep. CONFIG_RIGEL=1 (build.sh's default) means the kernel on this card
# *carries* Rigel, so no one has to rebuild anything to get it. Leaving `rigel`
# off cmdline.txt means the card *boots* the chipset-less machine, which is the
# one that is fast and the one whose behaviour is settled.
#
# Turning it on is then editing one line in a text file with the card in a
# reader -- which is exactly the property the boot argument was introduced for,
# and it does not exist if a release decides for its user.
say "packing the card"
BELLATRIX_RIGEL=0 BELLATRIX_VERSION="$TAG" \
    "$ROOT/scripts/make-sdcard.sh" --pack --out "$ARCHIVE"

# The two increments are copied from the same trees the archive was built from,
# not extracted back out of it: if they ever disagree, verify_assets says so
# rather than the difference travelling to a card.
#
# Their published names come from the config.txt that make-sdcard.sh just wrote,
# so renaming a file on the card is a one-line change there and nothing here has
# to learn about it.
CFG="$(tar -xJOf "$ARCHIVE" ./config.txt)"
KERNEL_NAME="$(sed -n 's/^kernel=\(.*\)$/\1/p' <<<"$CFG" | tr -d '\r')"
ELF_NAME="$(sed -n 's/^initramfs \([^ ]*\).*$/\1/p' <<<"$CFG" | tr -d '\r')"
[ -n "$KERNEL_NAME" ] && [ -n "$ELF_NAME" ] \
    || die "config.txt in the archive names no kernel or no initramfs"

BUILT_KERNEL="$(cat "$ROOT/out/images/Emu68.kernel-name" 2>/dev/null || echo Emu68.img)"
cp "$FIRMWARE/$BUILT_KERNEL.gz" "$RELEASE/$KERNEL_NAME"
cp "$DIST/aros-emu68-m68k.elf" "$RELEASE/$ELF_NAME"

# The QEMU bundle.
#
# QEMU's raspi3b does not run the Pi's boot ROM, so it never reads config.txt:
# the kernel, the device tree and the m68k ELF are passed on the command line
# instead. That is why this cannot be the card archive with a different name --
# it needs the files loose *and* a disk image, where the card needs files only.
say "packing the QEMU bundle"
QEMU_STAGE="$RELEASE/.qemu"
rm -rf "$QEMU_STAGE"; mkdir -p "$QEMU_STAGE"

# QEMU never reads this card's cmdline.txt -- the bootargs come from -append
# below -- but the card in the bundle should still say what the card in the
# archive says, so nobody reading one learns something untrue about the other.
BELLATRIX_RIGEL=0 "$ROOT/scripts/make-sdcard.sh" --out "$QEMU_STAGE/sd.img" >/dev/null

# The bundle's kernel is the card's kernel without the gzip: uncompressed here
# because the Pi firmware unpacks a gzipped kernel by content and QEMU does not.
# Taking the name from KERNEL_NAME rather than writing it out means a rename in
# make-sdcard.sh reaches this bundle too -- the card and the bundle cannot end
# up calling the same file different things.
QEMU_KERNEL="${KERNEL_NAME%.gz}"
# The bundle boots the same machine the card boots: the kernel carries Rigel and
# the command line does not ask for it. Turning it on here is one edit, and the
# launchers below say which -- including the part that is not obvious, that the
# divisor is not optional under QEMU.
QEMU_BOOTARGS="nocomposition"
QEMU_RIGEL_BOOTARGS="nocomposition rigel bellatrix.chipdiv=8"

cp "$ROOT/out/images/$BUILT_KERNEL" "$QEMU_STAGE/$QEMU_KERNEL"
cp "$FIRMWARE/bcm2710-rpi-3-b.dtb"  "$QEMU_STAGE/"
cp "$DIST/$ELF_NAME"                "$QEMU_STAGE/"

# Uncompressed here, unlike on a card: the Pi firmware unpacks a gzipped kernel
# by content, and QEMU does not.
cat > "$QEMU_STAGE/run.sh" <<'LAUNCHER'
#!/bin/sh
# Boot Bellatrix under QEMU. Needs qemu-system-aarch64 (Debian/Ubuntu:
# qemu-system-arm). Ctrl-A X quits. Anything passed here reaches qemu.
#
# nocomposition is not optional yet: with the compositor enabled the boot
# finishes and the screen never changes.
#
# The classic chipset: change the -append line below to
#     -append "@RIGEL_BOOTARGS@"
# The divisor is not optional under QEMU; README.txt says why.
here=$(cd "$(dirname "$0")" && pwd)
exec qemu-system-aarch64 \
    -M raspi3b -accel tcg,tb-size=64 \
    -kernel "$here/@KERNEL@" \
    -dtb "$here/bcm2710-rpi-3-b.dtb" \
    -initrd "$here/@ELF@" \
    -drive "file=$here/sd.img,if=sd,format=raw" \
    -append "@BOOTARGS@" \
    -serial mon:stdio -display gtk -device usb-tablet -no-reboot "$@"
LAUNCHER
chmod +x "$QEMU_STAGE/run.sh"

# The same command for Windows. Two differences, both learned the hard way by
# everyone who ships qemu: -display is left out, because a Windows qemu build
# may have no gtk frontend and naming one it does not have is a hard error,
# while omitting it lets qemu open its own window; and %~dp0 already ends in a
# backslash, so the paths concatenate without one.
cat > "$QEMU_STAGE/run.bat" <<'WINLAUNCHER'
@echo off
rem Boot Bellatrix under QEMU on Windows. Needs qemu-system-aarch64 on PATH
rem (the QEMU for Windows installer puts it there). Anything passed to this
rem script reaches qemu.
setlocal
set HERE=%~dp0

where qemu-system-aarch64 >nul 2>&1
if errorlevel 1 (
    echo qemu-system-aarch64 was not found on PATH.
    echo Install QEMU for Windows from https://qemu.weilnetz.de/w64/ and
    echo make sure its folder is on PATH, then run this again.
    exit /b 1
)

rem nocomposition is not optional yet: with the compositor enabled the boot
rem finishes and the screen never changes.
rem
rem The classic chipset: change the -append line below to
rem     -append "@RIGEL_BOOTARGS@"
rem The divisor is not optional under QEMU; README.txt says why.
qemu-system-aarch64 ^
    -M raspi3b -accel tcg,tb-size=64 ^
    -kernel "%HERE%@KERNEL@" ^
    -dtb "%HERE%bcm2710-rpi-3-b.dtb" ^
    -initrd "%HERE%@ELF@" ^
    -drive "file=%HERE%sd.img,if=sd,format=raw" ^
    -append "@BOOTARGS@" ^
    -serial mon:stdio -device usb-tablet -no-reboot %*
WINLAUNCHER

cat > "$QEMU_STAGE/README.txt" <<'QREADME'
Bellatrix under QEMU
====================

    ./run.sh          on Linux and macOS
    run.bat           on Windows

That is all, if qemu-system-aarch64 is installed. Without a window:

    ./run.sh -display none

The files, and why each is passed:

    @KERNEL@               the aarch64 kernel, given to -kernel
    bcm2710-rpi-3-b.dtb    the device tree, given to -dtb
    @ELF@                  the m68k system, given to -initrd
    sd.img                 the system volume, attached as the SD card

QEMU's raspi3b does not run the Pi's boot ROM, so it never reads a
config.txt from the card: the first three are named on the command line
instead. On real hardware the same files are on the card and config.txt
names them.

A wired USB tablet is attached for the pointer. QEMU's relative usb-mouse
path reverses both axes here.

The classic chipset
-------------------

This kernel carries it and does not boot it, exactly as the SD card
release does. The chipset is a boot argument rather than a build option,
so the same files boot either machine.

To boot it, change the -append line in run.sh or run.bat to:

    -append "@RIGEL_BOOTARGS@"

The divisor is not optional under QEMU, and it is worth knowing why.
Chipset time is a function of real elapsed time. A Raspberry Pi 3 can
deliver the 3546895 colour clocks a second that needs; QEMU manages about
a fifth of them. It does not respond by running a slower chipset --
it pins the core the chipset runs on, throws away the colour clocks it
could not deliver, and makes every CPU access to the chipset queue behind
it, at which point the boot stalls in the graphics drivers rather than
reaching a desktop.

bellatrix.chipdiv=8 runs the chipset clock at an eighth of real time, so
the machine asks for what this host can actually give. Everything the
chipset does still costs exactly what it costs; what changes is how many
colour clocks a second of real time buys. On a Pi, leave it out.
QREADME

sed -i "s|@KERNEL@|$QEMU_KERNEL|g; s|@ELF@|$ELF_NAME|g; s|@BOOTARGS@|$QEMU_BOOTARGS|g; \
        s|@RIGEL_BOOTARGS@|$QEMU_RIGEL_BOOTARGS|g" \
    "$QEMU_STAGE/run.sh" "$QEMU_STAGE/run.bat" "$QEMU_STAGE/README.txt"

tar -C "$QEMU_STAGE" -cf - . | xz -T0 -9 > "$RELEASE/bellatrix-$TAG-qemu.tar.xz"
rm -rf "$QEMU_STAGE"

( cd "$RELEASE" && for f in *; do sha256sum "$f" > "$f.sha256"; done )

verify_assets "$RELEASE"

# ---------------------------------------------------------------- release notes
#
# The notes carry the three digests and one sentence that matters more than all
# of them: whether dropping the two loose files onto an existing card is enough.
# The system digest answers it -- libraries, Zune classes and the commands in C:
# are files on the card, and no new ELF brings them along.
VERSION_TXT="$(tar -xJOf "$ARCHIVE" ./version.txt)"
D_SYSTEM="$(awk '/^d-system/ {print $2}' <<<"$VERSION_TXT")"

PREV_TAG="$(git -C "$ROOT" tag --sort=-creatordate | grep -v "^$TAG$" | head -1 || true)"
UPDATE_NOTE="Install the archive; this is the first release with digests recorded."
if [ -n "$PREV_TAG" ] && command -v gh >/dev/null && gh release view "$PREV_TAG" >/dev/null 2>&1; then
    PREV_SYSTEM="$(gh release view "$PREV_TAG" --json body -q .body 2>/dev/null |
                   awk '/^d-system/ {print $2; exit}' || true)"
    if [ -n "$PREV_SYSTEM" ] && [ "$PREV_SYSTEM" = "$D_SYSTEM" ]; then
        UPDATE_NOTE="The system volume is unchanged since $PREV_TAG: copying \`$KERNEL_NAME\` and \`aros-emu68-m68k.elf\` onto a $PREV_TAG card is enough."
    elif [ -n "$PREV_SYSTEM" ]; then
        UPDATE_NOTE="The system volume changed since $PREV_TAG, so the two loose files are not enough on their own — unpack the archive over the card, or onto a fresh one."
    fi
fi

NOTES_FILE="$RELEASE/.notes.md"
if [ -n "$NOTES" ]; then
    cp "$NOTES" "$NOTES_FILE"
elif [ -f "$ROOT/.github/release-notes/$TAG.md" ]; then
    cp "$ROOT/.github/release-notes/$TAG.md" "$NOTES_FILE"
else
    : > "$NOTES_FILE"
fi

{
    echo
    echo "## Installing"
    echo
    echo 'Format a microSD card with one FAT32 partition and unpack the archive at its root:'
    echo
    echo '```'
    echo "tar -xJf bellatrix-$TAG-pi3.tar.xz -C /media/you/BOOT"
    echo '```'
    echo
    echo "## Updating a card you already have"
    echo
    echo "$UPDATE_NOTE"
    echo
    echo "Both files go at the root of the card, under the names they have here —"
    echo '`config.txt` names them, so renaming one is the same as not copying it.'
    echo
    echo "## Running it without a Pi"
    echo
    echo "\`bellatrix-'"$TAG"'-qemu.tar.xz\` is the same system, packaged to run under"
    echo "QEMU. Unpack it anywhere and:"
    echo
    echo '```'
    echo "./run.sh"
    echo '```'
    echo
    echo "or \`run.bat\` on Windows. It needs \`qemu-system-aarch64\` — Debian and Ubuntu"
    echo "call the package \`qemu-system-arm\`, Windows users want the installer from"
    echo "qemu.weilnetz.de. Ctrl-A X quits, and \`./run.sh -display none\` runs it on the"
    echo "serial line alone."
    echo
    echo "## What this is"
    echo
    echo '```'
    echo "$VERSION_TXT"
    echo '```'
} >> "$NOTES_FILE"

# --------------------------------------------------------------------- publish
if [ "$DRY_RUN" = 1 ] || [ "$NO_PUBLISH" = 1 ]; then
    say "assets in $RELEASE:"
    ( cd "$RELEASE" && ls -lh -- * | sed 's/^/    /' )
    [ "$DRY_RUN" = 1 ] && say "dry run: nothing was published"
    exit 0
fi

# The tag is created here when it is missing, and pushed only after saying so.
# Pushing a tag is an outward-facing act, not a side effect of a build.
if ! git -C "$ROOT" rev-parse "$TAG" >/dev/null 2>&1; then
    say "creating tag $TAG at $COMMIT"
    git -C "$ROOT" tag -a "$TAG" -m "Bellatrix $TAG"
fi

if ! git -C "$ROOT" ls-remote --exit-code --tags origin "$TAG" >/dev/null 2>&1; then
    if [ -t 0 ]; then
        read -r -p "[release] push tag $TAG to origin? [y/N] " reply
        case "$reply" in [yY]|[yY][eE][sS]) ;; *) die "not pushed; nothing published" ;; esac
    else
        say "pushing tag $TAG (non-interactive)"
    fi
    git -C "$ROOT" push origin "$TAG"
fi

FLAGS=()
[ "$DRAFT" = 1 ] && FLAGS+=(--draft)
case "$TAG" in *-rc*|*-beta*|*-test*) FLAGS+=(--prerelease --latest=false) ;; esac

if gh release view "$TAG" >/dev/null 2>&1; then
    say "release $TAG exists, replacing its assets"
    # bellatrix-*.tar.xz is both archives, the card and the QEMU bundle. Naming
    # the bundle again here would hand gh the same file twice.
    gh release upload "$TAG" "$RELEASE"/bellatrix-*.tar.xz "$RELEASE/$KERNEL_NAME" \
        "$RELEASE/$ELF_NAME" "$RELEASE"/*.sha256 --clobber
else
    say "creating release $TAG"
    gh release create "$TAG" \
        "$RELEASE"/bellatrix-*.tar.xz "$RELEASE/$KERNEL_NAME" \
        "$RELEASE/$ELF_NAME" "$RELEASE"/*.sha256 \
        --title "$TAG" --notes-file "$NOTES_FILE" "${FLAGS[@]}"
fi

say "published: $(gh release view "$TAG" --json url -q .url)"
