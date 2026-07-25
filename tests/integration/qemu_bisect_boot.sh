#!/usr/bin/env bash
# tests/integration/qemu_bisect_boot.sh
#
# Automated boot oracle for `git bisect run` against the real QEMU/Emu68 JIT
# target (the Musashi harness's bellatrix_rom_boot tests do not reproduce the
# multicore stall — different CPU backend, no core topology).
#
# Pass criterion has TWO independent legs, both required:
#   1. chipset: frame export leaves the 256x256 power-up default, read from
#      the *unconditional* [RIGEL-FRAME] N=.. WxH.. trace line (NOT
#      [RIGEL-BPLCON0]/[RIGEL-FRAME-VIDEO] -- both hang off
#      machine_rigel_video_trace_enabled(), which hardcodes
#      g_rigel_video_trace=0 outside BELLATRIX_HARNESS builds, so on a real
#      QEMU/bare-metal build those lines never fire no matter what the guest
#      does and would report BAD unconditionally).
#   2. guest OS: ExecBase (M68K address 4) is non-null and its LibList has at
#      least one entry, read directly from emulated chip RAM via a small
#      liveness probe this script injects into machine_rigel_trace.c before
#      each build (see inject_execbase_probe below). A display resize alone
#      is not proof of a working OS -- a Guru Meditation also reprograms
#      BPLCON0 -- so leg 1 alone over-reports GOOD. Exec struct offsets are
#      fixed by the guest ROM (KS13/KS31/AROS...), not by Bellatrix source,
#      so this probe is valid unmodified across the whole bisect range.
#
# QEMU TCG is much slower than real hardware and there is no --frames cutoff
# outside the harness, so this polls the growing log instead of blocking on a
# single wall-clock timeout: it bails out EARLY (BAD) once the frame counter
# stops advancing for STALL_LIMIT seconds (the actual multicore-hang
# signature), but gives a live, still-256x256 boot up to HARD_CAP seconds
# before giving up -- known-good KS13 boots have taken "a few minutes" of
# wall clock historically, so a short fixed timeout would misclassify a slow
# but working commit as BAD.
#
# Each run resets submodules to the checked-out commit's pinned state and
# re-applies patches from scratch (setup.sh --reset) because patches/ and the
# emu68 gitlink move together across the bisect range -- an incremental
# `setup.sh` would apply a stale patch onto a submodule tree already patched
# by a *different* prior revision and corrupt the tree silently.
#
# Usage (env vars select the variant; see scripts/build.sh for the same
# names):
#   BELLATRIX_CPU_BACKEND=emu68|musashi   (default emu68)
#   BELLATRIX_MULTICORE_BUILD=0|1         (default 0)
#   ROM=<path>                            (default src/roms/KS13.rom)
#   FRAME_CAP=<frames>                    (default 2000 -- Jaime's own
#                                          calibration: no working boot has
#                                          ever needed more than this)
#   HARD_CAP=<seconds>                    (default 900, safety net only)
#   STALL_LIMIT=<seconds>                 (default 60)
#
# Exit codes (git-bisect-run convention):
#   0    display left the 256x256 power-up default        -> commit is GOOD
#   1    stalled, or still 256x256 past FRAME_CAP/HARD_CAP -> commit is BAD
#   125  build/setup failed or ROM missing                 -> SKIP this commit

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$ROOT/scripts/bellatrix-image.sh"
ROM="${ROM:-$ROOT/src/roms/KS13.rom}"
FRAME_CAP="${FRAME_CAP:-1500}"
HARD_CAP="${HARD_CAP:-900}"
STALL_LIMIT="${STALL_LIMIT:-60}"
export BELLATRIX_CPU_BACKEND="${BELLATRIX_CPU_BACKEND:-emu68}"
export BELLATRIX_MULTICORE_BUILD="${BELLATRIX_MULTICORE_BUILD:-0}"
export BELLATRIX_RIGEL_TRACE_BUILD=1

MUSASHI_CPU="${BELLATRIX_MUSASHI_CPU:-68040}"
IMAGE_NAME="$(bellatrix_image_name "$BELLATRIX_CPU_BACKEND" "$MUSASHI_CPU" "$BELLATRIX_MULTICORE_BUILD")"
INSTALL="$ROOT/out/firmware"
IMAGE="$ROOT/out/images/$IMAGE_NAME"
DTB="$INSTALL/bcm2710-rpi-3-b.dtb"

inject_execbase_probe() {
    local f="$ROOT/src/machine/machine_rigel_trace.c"
    local anchor="    /* Track noisy video-register changes only on explicit video/verbose trace. */"
    python3 - "$f" <<'PYEOF'
import sys

path = sys.argv[1]
anchor = '    /* Track noisy video-register changes only on explicit video/verbose trace. */'
probe = '''    {
        static uint32_t osdbg_live_last_libcount = 0xFFFFFFFFu;
        if (chip_ram_is_configured(&g_machine.memory)) {
            uint32_t eb = ((uint32_t)chip_ram_read16(&g_machine.memory, 4u) << 16) |
                          chip_ram_read16(&g_machine.memory, 6u);
            if (eb != 0u && eb < (uint32_t)g_machine.memory.chip_ram_size) {
                uint32_t liblist = eb + 378u;
                uint32_t node = ((uint32_t)chip_ram_read16(&g_machine.memory, liblist) << 16) |
                                 chip_ram_read16(&g_machine.memory, liblist + 2u);
                uint32_t osdbg_count = 0u;
                while (node != 0u && node < (uint32_t)g_machine.memory.chip_ram_size &&
                       osdbg_count < 64u) {
                    osdbg_count++;
                    node = ((uint32_t)chip_ram_read16(&g_machine.memory, node) << 16) |
                           chip_ram_read16(&g_machine.memory, node + 2u);
                }
                if (osdbg_count != osdbg_live_last_libcount) {
                    kprintf("[OSDBG-LIVE] ExecBase=%08x libs=%u frame=%llu\\n",
                            eb, (unsigned)osdbg_count,
                            (unsigned long long)g_rtrace.frame_count);
                    osdbg_live_last_libcount = osdbg_count;
                }
            }
        }
    }
''' + anchor

with open(path) as fh:
    text = fh.read()

if anchor not in text:
    sys.exit("ANCHOR_NOT_FOUND")

text = text.replace(anchor, probe, 1)
with open(path, 'w') as fh:
    fh.write(text)
PYEOF
}

echo "[BISECT-BOOT] $(git -C "$ROOT" rev-parse --short HEAD) backend=$BELLATRIX_CPU_BACKEND multicore=$BELLATRIX_MULTICORE_BUILD rom=$(basename "$ROM") hard_cap=${HARD_CAP}s stall_limit=${STALL_LIMIT}s"

[ -f "$ROM" ] || { echo "SKIP: ROM not present: $ROM"; exit 125; }

BUILD_LOG="$(mktemp)"
if ! "$ROOT/scripts/setup.sh" --reset >"$BUILD_LOG" 2>&1; then
    echo "SKIP: setup.sh --reset failed"
    tail -40 "$BUILD_LOG"
    rm -f "$BUILD_LOG"
    exit 125
fi
if ! inject_execbase_probe; then
    echo "SKIP: ExecBase liveness probe injection failed (anchor text not found in machine_rigel_trace.c at this commit)"
    exit 125
fi
if ! "$ROOT/scripts/build.sh" clean >>"$BUILD_LOG" 2>&1; then
    echo "SKIP: build failed"
    tail -40 "$BUILD_LOG"
    rm -f "$BUILD_LOG"
    exit 125
fi
rm -f "$BUILD_LOG"

[ -f "$IMAGE" ] || { echo "SKIP: image not produced: $IMAGE"; exit 125; }
[ -f "$DTB" ]   || { echo "SKIP: dtb not produced: $DTB"; exit 125; }

SERIAL_LOG="$(mktemp)"

qemu-system-aarch64 \
    -M raspi3b -kernel "$IMAGE" -dtb "$DTB" \
    -serial null -serial stdio -display none \
    -initrd "$ROM" \
    -append "console=ttyS0 enable_cache" \
    >"$SERIAL_LOG" 2>&1 &
QEMU_PID=$!
trap 'kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null; rm -f "$SERIAL_LOG"' EXIT

RESULT=""
ELAPSED=0
STALLED=0
LAST_FRAME=-1
POLL=5

while [ "$ELAPSED" -lt "$HARD_CAP" ]; do
    sleep "$POLL"
    ELAPSED=$((ELAPSED + POLL))

    LAST_SIZE_LINE="$(grep -o '\[RIGEL-FRAME\] N=[0-9]* [0-9]*x[0-9]*' "$SERIAL_LOG" | tail -1 || true)"
    LAST_OSDBG_LINE="$(grep -o '\[OSDBG-LIVE\] ExecBase=[0-9a-f]* libs=[0-9]*' "$SERIAL_LOG" | tail -1 || true)"
    LIBS="${LAST_OSDBG_LINE##*libs=}"
    [ -n "$LIBS" ] || LIBS=0

    if [ -n "$LAST_SIZE_LINE" ]; then
        SIZE="${LAST_SIZE_LINE##* }"
        CUR_FRAME="${LAST_SIZE_LINE#*N=}"
        CUR_FRAME="${CUR_FRAME%% *}"
        if [ "$SIZE" != "256x256" ] && [ "$LIBS" -ge 1 ]; then
            RESULT="GOOD"
            break
        fi
    else
        # [RIGEL-FRAME] may not be wired the same way at every commit in the
        # bisect range (its caller, not this file, differs across the
        # window) -- fall back to [CORE0-SUP]'s own frames= counter so a
        # live-but-differently-instrumented commit doesn't get misdeclared
        # BAD_STALLED just because this one signal is silent. GOOD still
        # strictly requires [RIGEL-FRAME] leaving 256x256 above; this only
        # feeds the stall/liveness check.
        SUP_FRAME="$(grep -o 'frames=[0-9]*' "$SERIAL_LOG" | tail -1 | grep -o '[0-9]*' || true)"
        CUR_FRAME="${SUP_FRAME:--1}"
    fi

    if [ "$CUR_FRAME" = "$LAST_FRAME" ]; then
        STALLED=$((STALLED + POLL))
        if [ "$STALLED" -ge "$STALL_LIMIT" ]; then
            RESULT="BAD_STALLED"
            break
        fi
    else
        STALLED=0
        LAST_FRAME="$CUR_FRAME"
    fi

    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        RESULT="BAD_EXITED"
        break
    fi

    if [ "$CUR_FRAME" != "-1" ] && [ "$CUR_FRAME" -ge "$FRAME_CAP" ]; then
        RESULT="BAD_FRAMECAP"
        break
    fi
done

[ -n "$RESULT" ] || RESULT="BAD_HARDCAP"
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

if [ "$RESULT" = "GOOD" ]; then
    echo "GOOD: display left 256x256 AND libs=${LIBS} at frame ~${LAST_FRAME} (${ELAPSED}s)"
    grep -m1 '\[OSDBG-LIVE\]' "$SERIAL_LOG" || true
    tail -3 "$SERIAL_LOG"
    exit 0
fi

echo "BAD ($RESULT): last frame ~${LAST_FRAME}, libs=${LIBS}, stalled=${STALLED}s, elapsed=${ELAPSED}s"
grep -m1 '\[OSDBG-LIVE\]' "$SERIAL_LOG" || true
tail -15 "$SERIAL_LOG"
exit 1
