#!/usr/bin/env bash
set -euo pipefail

HARNESS="$1"
ROM="$2"
ADF="$3"

if [ ! -f "$ROM" ]; then
    echo "SKIP: AROS ROM not found: $ROM"
    exit 77
fi
if [ ! -f "$ADF" ]; then
    echo "SKIP: AROS ADF not found: $ADF"
    exit 77
fi

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

HARNESS_RTG=1 "$HARNESS" "$ROM" --adf "$ADF" --headless --frames 1600 >"$LOG" 2>&1

grep -F "[HARNESS] Fast RAM: 8MB Zorro II board configured" "$LOG" >/dev/null
grep -F "[RTG] board registered: Zorro III 8MB window, 8180 KB VRAM" "$LOG" >/dev/null
grep -F "[SERIAL] -> configured, 40000000 - 407fffff" "$LOG" >/dev/null
grep -F "[RTG] first ROM read at window offset 0x100 — DiagArea probed" "$LOG" >/dev/null
grep -F "[SERIAL] da_Config=90" "$LOG" >/dev/null
grep -F "[RTG-DBG] b0000004" "$LOG" >/dev/null
grep -F "[RTG] REG_ID probed (FindCard reached the board)" "$LOG" >/dev/null
grep -F "[RTG-DBG] cafd0003" "$LOG" >/dev/null
grep -F "[RTG] direct VRAM mapped: 40003000-407fffff" "$LOG" >/dev/null
grep -F "[RTG-PROBE] FillRect count=1 size=640x480 fmt=1 arg=ff action=observed" "$LOG" >/dev/null
grep -F "[RTG-ACCEL] FillRect count=1 fmt=1 640x480 handled=host" "$LOG" >/dev/null
grep -F "[RTG-PROBE] BlitTemplate count=1 size=128x8 fmt=1 arg=ff action=observed" "$LOG" >/dev/null
grep -F "[RTG-ACCEL] BlitTemplate count=1 fmt=1 128x8 handled=host" "$LOG" >/dev/null
grep -F "[RTG-PROBE] BlitRectNoMaskComplete count=1 size=16x16 fmt=1 arg=0c action=observed" "$LOG" >/dev/null
grep -F "[RTG-ACCEL] BlitCopy count=1 fmt=1 16x16 handled=host" "$LOG" >/dev/null
grep -F "[RTG] first write reg=14 value=00000280" "$LOG" >/dev/null
grep -F "[RTG] first write reg=18 value=000001e0" "$LOG" >/dev/null
grep -F "[RTG] first write reg=24 value=00000000" "$LOG" >/dev/null
grep -F "[RTG] enable=1 640x480 fmt=1 bpr=640" "$LOG" >/dev/null

if grep -F "addr=4000" "$LOG" | grep -F "[Z3-OPENBUS]" >/dev/null; then
    echo "configured RTG Z3 window was incorrectly treated as open bus"
    cat "$LOG"
    exit 1
fi

echo "AROS RTG chain passed: Z3 -> card -> FillRect/BlitCopy -> 640x480 scanout"
