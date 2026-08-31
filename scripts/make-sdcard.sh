#!/usr/bin/env bash
#
# Build a bootable SD card image for the AROS m68k-emu68 target.
#
#   ./scripts/make-sdcard.sh                      from the local AROS build
#   ./scripts/make-sdcard.sh --pi                 add the boot payload for real hardware
#   ./scripts/make-sdcard.sh --pack               ... and pack it as .tar.xz to hand out
#   ./scripts/make-sdcard.sh --dist DIR           from another distribution tree
#   ./scripts/make-sdcard.sh --size 512M          bigger card
#   ./scripts/make-sdcard.sh --out path/to.img    elsewhere (names the archive
#                                                 too, under --pack)
#
# Without --pi the image carries AROS alone, which is all QEMU needs: run.sh
# passes the kernel, the device tree and the m68k ELF on the command line. A
# Raspberry Pi has no command line, so --pi additionally writes the Broadcom
# firmware, Emu68, the AROS ELF and the two boot text files into the same
# partition, producing an image that boots a Pi 3 as written:
#
#   ./scripts/make-sdcard.sh --pi
#   sudo dd if=out/aros/bellatrix-pi3.img of=/dev/sdX bs=4M conv=fsync
#
# Layout: MBR, one bootable FAT32 (type 0c, LBA) partition starting at LBA 2048.
# partition.library's MBR handler reports type 0c as DOSType 0x46415402 ("FAT\2"),
# which is what fat-handler claims, so AROS ends up with SDCARD0P0:.
#
# QEMU's raspi3b wires -sd to the same Arasan controller that arch/m68k-emu68's
# soc/sdcard drives, so this image exercises the real driver rather than a
# stand-in.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/out/build/aros/bin/emu68-m68k/AROS"

# The default is this repository's own build, which requires
# `./scripts/build-aros.sh full` -- the lean build produces the ELF and the
# modules linked into it, and nothing else. Pointing --dist at a foreign
# distribution is supported and was the norm until 2026-08-07, but it means the
# card carries that tree's modules: every library, Zune class and C: command.
# A patch touching module code then changes nothing that boots, silently.
OUT=""
OUT_GIVEN=""
SIZE="256M"
PI=0
PACK=0

while [ $# -gt 0 ]; do
    case "$1" in
        --pi)   PI=1; shift ;;
        --pack) PI=1; PACK=1; shift ;;
        --dist) DIST="$2"; shift 2 ;;
        --out)  OUT="$2"; OUT_GIVEN=1; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        -h|--help) sed -n '2,28p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
done

# Two different artefacts, so two different names: nothing distinguishes them
# by content once written, and flashing the QEMU card to a Pi produces a board
# that does nothing at all.
if [ -z "$OUT" ]; then
    if [ "$PI" = 1 ]; then
        OUT="$ROOT/out/aros/bellatrix-pi3.img"
    else
        OUT="$ROOT/out/aros/sd.img"
    fi
fi

FIRMWARE="$ROOT/out/firmware"

for tool in sfdisk mformat mcopy truncate; do
    command -v "$tool" >/dev/null \
        || { echo "ERROR: $tool not found (sfdisk: util-linux, mformat/mcopy: mtools)" >&2; exit 1; }
done

[ -d "$DIST" ] || { echo "ERROR: distribution tree not found: $DIST" >&2; exit 1; }

# What does NOT go on the card. Everything else does.
#
# This is how AROS itself builds a medium, and it is worth stating because the
# obvious alternative is worse. arch/m68k-amiga/boot/iso/mmakefile.src and
# arch/i386-pc/boot/iso/mmakefile.src both point mkisofs at $(AROSDIR) -- the
# whole distribution tree -- and then subtract: the boot ISO is the development
# ISO minus Sources and minus the kernel ELF. Nobody upstream maintains a list
# of drawers that belong on a medium, because the distribution already is that
# list.
#
# This script used to keep one anyway, and it cost exactly what a list like
# that costs: Rexxc and WBStartup were absent for months, not by decision but
# because they were built after the list was written and nothing connects the
# two. A drawer the distribution grows now arrives by itself.
#
# Developer/ is the one deliberate subtraction, and unlike upstream's it is not
# only about size: ~292 MB of headers, link libraries and SDK material that
# nothing on the card reads, and a card carrying it stalls the boot between
# AROSMonDrvs and "preparing console" -- an open problem of its own.
EXCLUDE=(Developer)

# The kernel ELF is at the distribution root too, and it is not part of the
# volume: --pi writes it beside the firmware, where the Pi's bootloader can
# find it, and QEMU is handed it on the command line. Upstream removes its own
# equivalent from the boot ISO for the same reason.
ELF="aros-emu68-m68k.elf"
EXCLUDE+=("$ELF")

# What the card cannot boot without, checked so that running the lean build by
# mistake fails here rather than three minutes into a boot.
#
# Locale is on the list because S:Startup-Sequence does
# Assign "LOCALE:" "SYS:Locale": without it the boot console opens with
# "Can't find SYS:Locale" and every later LOCALE:-relative assign is built on
# sand. The rest are the drawers a boot reads before it reaches a desktop.
REQUIRED=(C S Libs Devs L Classes Fonts System Prefs Locale)

missing=()
for d in "${REQUIRED[@]}"; do
    [ -d "$DIST/$d" ] || missing+=("$d")
done
[ -f "$DIST/AROS.boot" ] || missing+=("AROS.boot")

# VC4 is not only the display HIDD.  Its 3D path crosses Mesa's GL frontend,
# Gallium and vc4gallium.hidd, so a distributable hardware pack must carry the
# complete matching stack and at least the diagnostic/demo programs used to
# exercise it on a Pi.  Failing here prevents a successful-looking archive
# made from a stale distribution tree that contains only the 2D driver.
REQUIRED_3D=(
    Libs/gl.library
    Libs/mesa3dgl20-0.library
    Libs/gallium.library
    Devs/Drivers/gallium.hidd
    Devs/Drivers/vc4gallium.hidd
    Extras/Demos/GL/glinfo
    Extras/Demos/GL/gears
    Extras/Demos/GL/gearbox
)
for f in "${REQUIRED_3D[@]}"; do
    [ -f "$DIST/$f" ] || missing+=("$f")
done

# The display driver proper, and the DEVS:Monitors program that loads it.
#
# Checked separately from the 3D stack because its absence does not look like
# a failure. The kickstart carries a framebuffer driver registered with
# DDRV_BootMode, so a card without these still boots to a desktop -- on the
# surface the firmware set up, with no HVS ownership and no hardware cursor.
# It looks like a slow machine rather than a missing driver, which is a much
# worse thing to ship than a build error.
REQUIRED_GFX=(
    Devs/Drivers/vcgfx.hidd
    Devs/Monitors/VideoCore
)
for f in "${REQUIRED_GFX[@]}"; do
    [ -f "$DIST/$f" ] || missing+=("$f")
done

if [ "${#missing[@]}" -ne 0 ]; then
    echo "ERROR: $DIST is missing: ${missing[*]}" >&2
    echo "       The lean 'kernel-link-<target>' build only produces the ELF." >&2
    echo "       A bootable card needs the full distribution tree." >&2
    exit 1
fi

# Resolved once, so the copy and the .info pass agree on what is going.
excluded() {
    local e
    for e in "${EXCLUDE[@]}"; do [ "$1" = "$e" ] && return 0; done
    return 1
}

ENTRIES=()
while IFS= read -r entry; do
    excluded "$entry" && continue
    # An .info without its drawer is an icon that opens onto nothing. The
    # distribution ships Demos.info and Developer.info whether or not the
    # drawers were built, so this is not hypothetical.
    case "$entry" in
        *.info)
            base="${entry%.info}"
            [ -d "$DIST/$base" ] || continue
            excluded "$base" && continue
            ;;
    esac
    ENTRIES+=("$entry")
done < <(cd "$DIST" && ls -A)

# The boot payload, for --pi only.
#
# bootcode.bin is the stage the Pi 3 loads out of the card by itself; start.elf
# and fixup.dat are the GPU firmware it then runs; the DTB is picked by the
# firmware from the board model, so every Pi 3 variant's tree is written and the
# board chooses. The kernel keeps the composition name recorded by build.sh;
# the firmware decompresses it by content, so config.txt may select either.
#
# The AROS ELF is taken from the distribution tree rather than out/aros so the
# kernel and the modules on the card always come from one build.
if [ "$PI" = 1 ]; then
    KERNEL_IMAGE="$(cat "$ROOT/out/images/Emu68.kernel-name" 2>/dev/null || echo Emu68.img)"
    BOOT_FILES=("$FIRMWARE/bootcode.bin" "$FIRMWARE/start.elf" "$FIRMWARE/fixup.dat"
                "$FIRMWARE/$KERNEL_IMAGE.gz" "$DIST/$ELF")
    for f in "$FIRMWARE"/bcm2710-*.dtb; do
        [ -e "$f" ] && BOOT_FILES+=("$f")
    done

    missing=()
    for f in "${BOOT_FILES[@]}"; do
        [ -f "$f" ] || missing+=("$(basename "$f")")
    done
    if [ "${#missing[@]}" -ne 0 ]; then
        echo "ERROR: boot payload incomplete: ${missing[*]}" >&2
        echo "       Run ./scripts/build.sh for the firmware and Emu68," >&2
        echo "       and ./scripts/build-aros.sh full for the AROS ELF." >&2
        exit 1
    fi
fi

mkdir -p "$(dirname "$OUT")"

if [ "$PACK" = 0 ]; then
    rm -f "$OUT"

    echo "[sd] $SIZE image at $OUT"
    truncate -s "$SIZE" "$OUT"

    # BELLATRIX_SFS=1 adds a second partition for the FAT-versus-SFS comparison
    # sdcard.md asks for.
    #
    # The Pi's firmware can only boot from FAT, so SYS: stays where it is and
    # the SFS volume is a work partition beside it: same card, same driver,
    # same everything except the filesystem, which is the only way the 2x2
    # matrix means anything.
    #
    # NOT FINISHED, and the reason is recorded here rather than in a commit
    # message nobody will find.
    #
    # Type 0x2f is what rom/partition/partition_types.c:33 maps to DOSType
    # SFS\0, which is why it was chosen. It is not enough: booted with such a
    # partition present, the system lists only SDCARD0P0 and no device node
    # appears for the second one at all.
    #
    # The supported path is an RDB nested inside the MBR.
    # rom/partition/partitionrdb.c:198 will only look for a RigidDiskBlock
    # inside an MBR partition of type 0x30 or 0x76, and the Amiga-side DOSType
    # then comes from the RDB's partition blocks rather than from the MBR type
    # byte. Writing that RDB -- RigidDiskBlock plus PartitionBlocks, with
    # checksums -- is what remains.
    if [ "${BELLATRIX_SFS:-0}" = 1 ]; then
        FATSECTORS=$(( 128 * 1024 * 1024 / 512 ))
        SECTORS2=$(( ($(stat -c%s "$OUT") / 512) - 2048 - FATSECTORS ))
        sfdisk -q "$OUT" >/dev/null <<EOF
label: dos
unit: sectors
start=2048, size=$FATSECTORS, type=c, bootable
start=$(( 2048 + FATSECTORS )), type=30
EOF
        echo "[sd] second partition: type 0x30, RDB container"

        # An RDB inside it, with one partition of DOSType SFS\0.
        #
        # rom/partition/partitionrdb.c:198 accepts a RigidDiskBlock only inside
        # an MBR partition of type 0x30 or 0x76, and the Amiga DOSType comes
        # from the RDB's partition blocks rather than the MBR type byte. Type
        # 0x2f maps to SFS\0 in partition_types.c and produces no device node
        # at all, which is what was tried first.
        #
        # rdbtool comes from external/amitools. Geometry has to be given
        # explicitly -- it cannot infer one from a bare file -- and heads=1,
        # sectors=32 matches what the SD device reports (sdcu_Heads = 1).
        RDBIMG="$OUT.rdb"
        RDBCYL=$(( (SECTORS2) / 32 ))
        rm -f "$RDBIMG"
        # The handler goes *inside* the RDB, in FSHD/LSEG.
        #
        # A partition whose DOSType is SFS\0 tells AROS what the volume is, not
        # how to serve it. The legacy tree's working SFS disk carried the
        # driver in the RDB for exactly this reason, and rdbtool's fsadd is how
        # it gets there. 24 RDB cylinders because the default reserves 32
        # blocks and the handler is 135 KB.
        SFSHANDLER="$DIST/L/sfs-handler"
        if PYTHONPATH="$ROOT/external/amitools" python3 \
                "$ROOT/external/amitools/bin/rdbtool" "$RDBIMG" \
                create chs=$RDBCYL,1,32 + init rdb_cyls=24 \
                + add size=100% fs=0x53465300 \
                + fsadd "$SFSHANDLER" fs=0x53465300 >/dev/null 2>&1; then
            dd if="$RDBIMG" of="$OUT" bs=512 seek=$(( 2048 + FATSECTORS )) \
                conv=notrunc status=none
            rm -f "$RDBIMG"
            echo "[sd] RDB written: one partition, DOSType SFS\\0"
        else
            echo "[sd] WARNING: rdbtool failed; the second partition has no RDB" >&2
        fi
    else
        sfdisk -q "$OUT" >/dev/null <<'EOF'
label: dos
unit: sectors
start=2048, type=c, bootable
EOF
    fi

    # @@1M is the mtools offset to the partition at LBA 2048.
    if [ "${BELLATRIX_SFS:-0}" = 1 ]; then
        SECTORS=$FATSECTORS
    else
        SECTORS=$(( ($(stat -c%s "$OUT") / 512) - 2048 ))
    fi
    mformat -i "$OUT@@1M" -F -v AROS -T "$SECTORS" ::
fi

echo "[sd] collecting the card contents"

# mtools enforces DOS device-name rules that FAT itself does not: it refuses to
# create a file called AUX, CON, PRN, NUL, COM1-9 or LPT1-9, anywhere in the
# tree. AROS's FAT handler has no such rule, and the full distribution ships
# Devs/DOSDrivers/AUX -- the AUX: mountlist. Copying the tree straight in then
# fails on that one file with no message at all and a bare exit 1, which reads
# like the card being too small or the tree being missing.
#
# Stage through a hard-link farm, which copies no data, and drop those names
# there: the build tree is never modified, and what was left out is named
# rather than silently missing.
STAGE="$(dirname "$OUT")/.sdstage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
#
# The .info files come along by being part of the tree, which matters more than
# it looks: Wanderer draws a drawer from its .info, and that file sits *beside*
# the drawer rather than inside it. A copy that took directories only would
# produce a boot volume whose window is empty -- every drawer present, none of
# them visible.
for entry in "${ENTRIES[@]}"; do
    cp -al "$DIST/$entry" "$STAGE/$entry"
done

# BELLATRIX_TRACE_STARTUP=1 makes the Startup-Sequence say where it is.
#
# A boot that stops somewhere in the sequence leaves no trace of which line it
# stopped on, and the candidates are not few: AddUSBClasses and AddBTClasses
# are asynchronous, Automount and Mount touch every DOSDriver, `Dir PIPE:`
# needs a handler, and the ENVARC: copy walks the whole tree. Guessing between
# them costs a card write per guess.
#
# So echo each line to DEBUG: before running it. The last line in the log is
# the one that did not return -- which is the whole question.
#
# Not on by default: it is noisy, and the serial line is the slowest thing on
# the machine.
if [ "${BELLATRIX_TRACE_STARTUP:-0}" = 1 ]; then
    seq="$STAGE/S/Startup-Sequence"
    if [ -f "$seq" ]; then
        python3 - "$seq" <<'TRACE'
import os, sys
p = sys.argv[1]
raw = open(p, "rb").read().decode("latin-1")
out = []
for line in raw.split("\n"):
    stripped = line.strip()
    # Leave structure alone: echoing inside an If/EndIf pair or before a label
    # changes what the shell parses, and a trace that alters the thing it
    # measures is worse than none.
    if (stripped and not stripped.startswith(";")
            and not stripped.upper().startswith(("IF ", "ELSE", "ENDIF", "LAB ",
                                                 "SKIP ", ".", "FAILAT"))):
        out.append('Echo >DEBUG: "[startup] %s"' % stripped.replace('"', "'")[:70])
    out.append(line)
# The stage is hard-linked from the distribution tree (cp -al), so an
# in-place rewrite edits the build tree itself and every later card
# inherits it. Write beside the file and move over it, which is what
# the boot-test insertion below already does.
open(p + ".new", "wb").write("\n".join(out).encode("latin-1"))
os.replace(p + ".new", p)
print("[sdcard] Startup-Sequence traced to DEBUG:")
TRACE
    fi
fi

# The classic display driver is on the card, and presentation is manual.
#
# amigavideo.hidd is inert on its own; what starts it is
# DEVS:Monitors/AmigaVideo, which AROSMonDrvs runs at boot. From that point
# AROS draws through Denise -- and nothing puts Denise on the panel by itself.
# `DeniseView SHOW` is the presenter and it is a command someone types.
#
# That is deliberate, and it is the decision: an automatic presenter is not
# wanted. The consequence has to be known rather than rediscovered, because it
# looks exactly like a hang -- the panel holds whatever the VideoCore last
# scanned out and the boot clock stops. It has been read as a crash three
# times. It is not one; the machine is running and drawing where nobody is
# looking.
#
# Without this driver there is no producer at all. Demo Reel 3 draws through
# graphics.library like any well-behaved application, so with vcgfx as the
# only display its bitplanes go to the VideoCore and Rigel's Denise is handed
# nothing -- which is what every census on hardware has said: non-bg=0,
# sum=00000000, for the whole of a boot. ISSUE-0068 reached the same
# conclusion from the disassembly before any of this was measured.
#
# A note on its cost, because the number in this comment used to be wrong.
# Leaving it off was justified here with "a boot that reaches Wanderer in
# under a minute took over two and a half, and the card dropped from 1173 to
# 178 KB/s". That was measured on a machine which was also spending its time
# in an interrupt storm -- dwc2emu68 never cleared the latched core
# interrupts, and SOF was unmasked from controller init. Both are fixed. What
# this driver actually costs has not been measured since, and the old figure
# should not be quoted as if it had.
#
# BELLATRIX_CHIPSET_DISPLAY=0 leaves it off, for a boot that wants the panel
# to keep showing the desktop. ISSUE-0068, ISSUE-0073.
if [ "${BELLATRIX_CHIPSET_DISPLAY:-1}" = 0 ]; then
    rm -f "$STAGE/Devs/Monitors/AmigaVideo"
    echo "[sdcard] classic chipset display driver left off the card"
else
    echo "[sdcard] classic chipset display driver ON -- the panel needs DeniseView SHOW"
fi

# BELLATRIX_DEMOREEL puts a drawer of extracted Demo Reel 3 files on the card.
#
# Not on every card, and not in the repository: the demo is commercial software
# from 1989 and is not ours to redistribute. tools/demoreel/install.sh extracts
# it from your own ADFs into out/, which is git-ignored, and this copies what is
# there if you point at it.
#
# It goes in one drawer on purpose. The demo's own ToRAM assigns DemoReel3: and
# DemoReelData: to the current directory when it finds Monument, so both disks
# merged and run from that drawer need no assigns from us. ISSUE-0068 phase 4.
if [ -n "${BELLATRIX_DEMOREEL:-}" ]; then
    if [ ! -d "$BELLATRIX_DEMOREEL" ]; then
        echo "ERROR: BELLATRIX_DEMOREEL=$BELLATRIX_DEMOREEL is not a directory" >&2
        echo "       Run tools/demoreel/install.sh first." >&2
        exit 1
    fi
    mkdir -p "$STAGE/DemoReel3"
    cp -al "$BELLATRIX_DEMOREEL/." "$STAGE/DemoReel3/" 2>/dev/null ||         cp -a "$BELLATRIX_DEMOREEL/." "$STAGE/DemoReel3/"
    echo "[sdcard] Demo Reel 3 on the card: $(find "$STAGE/DemoReel3" -type f | wc -l) files"
fi

# tests/ram-stress/ goes on every card, and nothing runs it.
#
# They are diagnostic scripts for ISSUE-0037, where the trigger happens four
# times per boot and the fault appears in roughly one boot in four -- so the
# expensive part of that investigation is waiting, not looking. Put on the card
# they turn one boot into as many iterations as you have patience for:
#
#     Execute "S:ram-stress-c"
#
# Copied here rather than by hand because a diagnostic that has to be
# re-injected after every build is one that quietly stops being there. That
# already happened once with C:xSysInfo.
if [ -d "$ROOT/tests/ram-stress" ]; then
    for s in "$ROOT/tests/ram-stress/ram-stress-"*; do
        [ -f "$s" ] && cp "$s" "$STAGE/S/$(basename "$s")"
    done
fi

# tests/gl/ for the same reason: probes that report through DEBUG:, so that
# what they found survives a machine that has stopped responding.
if [ -d "$ROOT/tests/gl" ]; then
    for s in "$ROOT/tests/gl/"*; do
        case "$s" in *.md) continue ;; esac
        [ -f "$s" ] && cp "$s" "$STAGE/S/$(basename "$s")"
    done
fi

# tests/media/ is for sample files a test needs to have something to work on.
#
# Audio bring-up needs a real file to play, and copying one onto the card by
# hand between builds is how a test stops being run. Anything dropped in that
# directory lands in the card's root; nothing is required to be there.
#
# Not committed to the repository: these are large and are somebody's audio.
# tests/media/ is in .gitignore for that reason.
if [ -d "$ROOT/tests/media" ]; then
    for m in "$ROOT/tests/media/"*; do
        [ -f "$m" ] && cp "$m" "$STAGE/$(basename "$m")"
    done
fi

# tests/sysinfo/ holds third-party measurement binaries -- currently xSysInfo,
# which sdcard.md sec.11 lists as one additional data point. It goes to C: so a
# boot test can call it by name; it takes a CLI template (DEBUG/S, BRIEF/S,
# FULL/S, DARK/S) and FULL writes its report to the shell rather than opening
# the GUI, which is what makes it scriptable at all.
if [ -d "$ROOT/tests/sysinfo" ]; then
    for b in "$ROOT/tests/sysinfo/"*; do
        case "$b" in *.md) continue ;; esac
        [ -f "$b" ] && cp "$b" "$STAGE/C/$(basename "$b")"
    done
fi

# BELLATRIX_BOOT_TEST runs one of them at boot.
#
# As early as the assigns allow, and that is a measurement decision rather
# than a tidiness one. Placed before Wanderer, a probe waits out the entire
# boot -- five minutes under QEMU -- to reach the few seconds it is there to
# observe, and every iteration of the investigation pays that. Placed after
# LIBS:, FONTS: and the rest are in place, which is all a program needs to be
# loaded and opened, it runs within a fraction of that.
#
# After the Mount, not before it, and that is not a detail.
#
# The anchor used to be `Assign "IMAGES:"`, which is line 30 of the sequence.
# DEVS:DOSDrivers is mounted on line 53. So a probe ran before DEBUG: existed
# as a device at all, and every redirection to it went nowhere -- for months,
# across seven scripts, silently. A run that printed nothing was read as a run
# that never got that far, which is the worst way for a diagnostic to fail.
#
# Moving the anchor after the Mount costs the handful of lines between them --
# a Path, a couple of assigns -- and buys a working DEBUG:. It is still long
# before Wanderer, which is the whole point of not using the LATE position.
#
# BELLATRIX_BOOT_TEST_LATE=1 restores the old position for anything that
# genuinely needs a finished system.
if [ -n "${BELLATRIX_BOOT_TEST:-}" ]; then
    if [ ! -f "$STAGE/S/$BELLATRIX_BOOT_TEST" ]; then
        echo "ERROR: no S:$BELLATRIX_BOOT_TEST to run at boot" >&2
        exit 1
    fi
    if [ "${BELLATRIX_BOOT_TEST_LATE:-0}" = 1 ]; then
        anchor='^If EXISTS "WANDERER:Wanderer"'
    else
        anchor='^Mount >NIL: "DEVS:DOSDrivers'
    fi
    awk -v script="$BELLATRIX_BOOT_TEST" -v anchor="$anchor" '
        $0 ~ anchor && !done { print; print "Execute \"S:" script "\""; done = 1; next }
        { print }
    ' "$STAGE/S/Startup-Sequence" > "$STAGE/S/Startup-Sequence.new" \
        && mv "$STAGE/S/Startup-Sequence.new" "$STAGE/S/Startup-Sequence"
    echo "[sd] boot test: S:$BELLATRIX_BOOT_TEST"
fi

if [ "$PI" = 1 ]; then
    # Keep the composition name chosen by build.sh. A Rigel image is Bellatrix;
    # a build without that integration remains Emu68.
    for f in "${BOOT_FILES[@]}"; do
        cp -al "$f" "$STAGE/$(basename "$f")"
    done

    # config.txt and cmdline.txt are written here rather than kept as files in
    # the repository: they name what this script just copied, and the two
    # drifting apart is a card that stops with no message worth reading.
    cat > "$STAGE/config.txt" <<'EOF'
# Bellatrix on a Raspberry Pi 3. Written by scripts/make-sdcard.sh --pi.

kernel=KERNEL_IMAGE_PLACEHOLDER
arm_64bit=1
initramfs aros-emu68-m68k.elf

disable_splash=1
avoid_warnings=1
# The VideoCore split has to hold the framebuffer the graphics driver asks
# for. vc4gfx programs the mode itself and wants two pages at the display's
# depth -- 1920x1080 at 32bpp is 8.3 MB a page -- and 32 MB is not enough
# once the firmware's own use is counted. A refused FBALLOC used to end as a
# black screen; it is now reported, but reporting it is not the same as
# having the memory. arch/aarch64-raspi sets 128 for the same reason (its
# comment is about the GL stack, which is the same pool).
gpu_mem=128

hdmi_group=2
hdmi_mode=82

# HDMI rather than DVI, which is what carries audio.
#
# hdmi_drive=1 is DVI: video only, no audio packets on the link at all. The
# firmware picks DVI when nothing says otherwise, so hdmiaudio.audio can
# initialise, program the MAI block and produce nothing audible -- the driver
# is fine and the wire is in the wrong mode. arch/arm-raspi sets this for the
# same reason.
hdmi_drive=2

# NOT setting dtoverlay=miniuart-bt here, deliberately.
#
# On a stock Pi 3 that overlay is what gives Bluetooth the PL011 and moves the
# console to the mini-UART, which is the arrangement the comment below assumes.
# But this image ships no overlays/ directory at all, and a dtoverlay line
# naming a file the firmware cannot find is ignored without a word -- which
# would read as "configured" while changing nothing.
#
# It may not be needed: btuart.resource reports "[BTUART] ready:
# PL011=0xf2201000" on hardware and the transport reaches the HCI layer, so
# something is already routing the PL011 to the radio. Whether that is Emu68's
# device tree or the firmware default has not been established. Settle that
# before adding the line, and ship overlays/ with it if it turns out to be
# needed.

# Serial console on GPIO 14/15. Bluetooth owns the PL011 on a Pi 3, so the
# console comes out of the mini-UART instead, and the mini-UART's baud rate
# follows the core clock -- pinning that clock is what keeps it readable.
enable_uart=1
core_freq=400
core_freq_min=400
EOF
    sed -i "s/KERNEL_IMAGE_PLACEHOLDER/$KERNEL_IMAGE.gz/" "$STAGE/config.txt"

    # One line, no trailing newline: the firmware hands this to Emu68 as the
    # boot arguments, and nocomposition is what puts AROS on the screen.
    #
    # BELLATRIX_CMDLINE_EXTRA appends to it, for arguments a diagnostic run
    # wants and a shipped card must not carry. The ones worth knowing about:
    #
    #   mungwall     walls every AllocMem and checks them on FreeMem, so a
    #                heap overrun is reported by whoever wrote past its
    #                allocation instead of by the next unrelated free
    #                (ISSUE-0037). Costs memory and speed; never default.
    #   sysdebug=..  AROS's runtime debug flags, e.g. sysdebug=InitCode.
    #
    #   BELLATRIX_CMDLINE_EXTRA=mungwall ./scripts/make-sdcard.sh --pack
    #
    #
    # BELLATRIX_CMDLINE replaces the whole line, for asking whether a default
    # is still earned. `nocomposition` has been on since emu68gfx was the only
    # display driver and the software compositor left the screen on the Emu68
    # logo; with fbgfx and vcgfx in its place that premise wants re-testing,
    # and a card that cannot be built without the flag cannot test it.
    #
    RIGEL_BOOTARG=""
    if [ "$(cat "$ROOT/out/images/Emu68.config-rigel" 2>/dev/null || echo 0)" = 1 ]; then
        RIGEL_BOOTARG=" bellatrix.rigel=1"
    fi
    printf '%s%s%s' \
        "${BELLATRIX_CMDLINE-nocomposition}" \
        "$RIGEL_BOOTARG" \
        "${BELLATRIX_CMDLINE_EXTRA:+ $BELLATRIX_CMDLINE_EXTRA}" \
        > "$STAGE/cmdline.txt"
fi

# What this card is, written for the same reason config.txt is: a card outlives
# the download it came from, and a card that cannot say what it is turns every
# later question into guesswork.
#
# The three digests are what decides whether an in-place update is enough. They
# are recorded, never enforced: a newer kernel or ELF over an older volume is a
# supported state -- see docs/Compat.md -- and this file is how somebody tells
# which combination they are looking at.
#
# BELLATRIX_VERSION is set by scripts/release.sh. Outside a release there is no
# tag to claim, and saying so is better than inventing one.
digest() { sha256sum | cut -c1-12; }

{
    echo "bellatrix   ${BELLATRIX_VERSION:-untagged}"
    echo "built       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit      $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "emu68       $(git -C "$ROOT/external/emu68" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "aros        $(git -C "$ROOT/external/aros" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "d-emu68     $({ git -C "$ROOT" rev-parse HEAD:patches/emu68
                          git -C "$ROOT/external/emu68" rev-parse HEAD; } 2>/dev/null | digest)"
    echo "d-elf       $({ git -C "$ROOT" rev-parse HEAD:aros HEAD:patches/aros
                          git -C "$ROOT/external/aros" rev-parse HEAD; } 2>/dev/null | digest)"
    echo "d-system    $({ git -C "$ROOT" rev-parse HEAD:patches/aros
                          git -C "$ROOT/external/aros" rev-parse HEAD; } 2>/dev/null | digest)"
} > "$STAGE/version.txt"

# --pack hands out the card's contents instead of a card: the recipient formats
# their own FAT32 partition, at whatever size their card happens to be, and
# unpacks this into it. No image is written -- the size of the medium is theirs
# to decide.
#
# tar carries the DOS device names that the staging step keeps aside for mtools,
# since nothing in this path goes through mtools.
if [ "$PACK" = 1 ]; then
    # --out names the archive when it is given, so a release can name its own
    # asset; otherwise the default sits beside the image it replaces.
    if [ -n "$OUT_GIVEN" ]; then
        ARCHIVE="$OUT"
    else
        ARCHIVE="$(dirname "$OUT")/bellatrix-pi3.tar.xz"
    fi
    mkdir -p "$(dirname "$ARCHIVE")"
    echo "[sd] packing $(basename "$ARCHIVE")"
    # One thread keeps xz -9 within the memory available on ordinary build
    # hosts.  -T0 may allocate several GiB and leave a truncated archive.
    tar -C "$STAGE" -cf - . | xz -T1 -9 > "$ARCHIVE"
    rm -rf "$STAGE"
    echo "[sd] $(basename "$ARCHIVE")  ($(stat -c%s "$ARCHIVE") bytes)"
    exit 0
fi

while IFS= read -r f; do
    [ -n "$f" ] || continue
    echo "[sd] skipping $f -- mtools will not write a DOS device name"
    rm -f "$STAGE/$f"
done < <(cd "$STAGE" && find . -type f | sed 's|^\./||' |
         grep -iE '(^|/)(AUX|CON|PRN|NUL|COM[1-9]|LPT[1-9])(\.[^/]*)?$' || true)

# The staging tree is the authority now, not the list that built it: --pi has
# added the boot payload to it and version.txt is written into it, so anything
# that reads DIST again here would write a different card than --pack packs.
while IFS= read -r -d '' entry; do
    if [ -d "$entry" ]; then
        mcopy -i "$OUT@@1M" -s -Q "$entry" ::
    else
        mcopy -i "$OUT@@1M" -Q "$entry" ::
    fi
done < <(find "$STAGE" -mindepth 1 -maxdepth 1 -print0)

rm -rf "$STAGE"

echo "[sd] $(basename "$OUT")  ($(stat -c%s "$OUT") bytes)"
mdir -i "$OUT@@1M" :: | tail -n +2

if [ "$PI" = 1 ]; then
    echo "[sd] write it with:  sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync"
fi
