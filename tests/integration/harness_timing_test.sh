#!/usr/bin/env bash
#
# Cross-emulator E-clock timing probe (Copperline "timing-test").
#
# Boots LinuxJedi/Copperline's bootable timing-test disk under the Bellatrix
# harness. The disk takes over the machine (IRQ/DMA off, no ROM calls) and
# streams 32 rows of CIA-A E-clock measurements over the serial port
# (slow/chip RAM read/write, shift, mul, dbra, blitter, copper-vs-CPU phase,
# frame length, ...). We capture those rows and diff them against a committed
# Bellatrix baseline so a chipset/CPU-timing regression shows up as a changed
# row.
#
# The ADF is intentionally not committed (it is a GPL-3 asset built from the
# upstream submodule). This script regenerates it from the committed
# boot.bin/test.bin with make_adf.py -- no vasm needed.
#
# Regression guard only. It is NOT a cross-emulator accuracy check: the
# absolute tick counts depend on CPU model / ROM / RAM layout, so matching
# Copperline's or FS-UAE's reference numbers requires a config-matched run
# (see external/copperline/timing-test/compare.py). That comparison is a
# follow-up; here we lock in *our own* numbers so they cannot silently drift.
#
# Usage:
#   harness_timing_test.sh <harness> <rom> <timing-test-dir> <baseline>
#
# Set TIMING_TEST_UPDATE=1 to (re)write the baseline instead of comparing.
#
# Exit 77 (CTest SKIP) when the upstream submodule / prebuilt bins are absent.
set -euo pipefail

HARNESS="$1"
ROM="$2"
TT_DIR="$3"
BASELINE="$4"

CYCLES="${TIMING_TEST_CYCLES:-60000000}"

for f in "$HARNESS" "$ROM"; do
    if [ ! -e "$f" ]; then
        echo "SKIP: missing $f"
        exit 77
    fi
done

BOOT_BIN="$TT_DIR/boot.bin"
TEST_BIN="$TT_DIR/test.bin"
MAKE_ADF="$TT_DIR/make_adf.py"
if [ ! -f "$BOOT_BIN" ] || [ ! -f "$TEST_BIN" ] || [ ! -f "$MAKE_ADF" ]; then
    echo "SKIP: Copperline timing-test submodule not checked out ($TT_DIR)"
    echo "      run: git submodule update --init external/copperline"
    exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found (needed to build timing-test.adf)"
    exit 77
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ADF="$WORK/timing-test.adf"
LOG="$WORK/run.log"
GOT="$WORK/got.txt"

python3 "$MAKE_ADF" "$BOOT_BIN" "$TEST_BIN" "$ADF" >/dev/null

"$HARNESS" "$ROM" --adf "$ADF" --headless --cycles "$CYCLES" >"$LOG" 2>&1

# The test emits its rows as SERDAT words; the harness surfaces them as
# "[SERIAL] XXXXXXXX". Take the first 32 (one full pass of the probe table).
grep -oE '\[SERIAL\] [0-9A-Fa-f]{8}' "$LOG" | awk '{print toupper($2)}' | head -32 >"$GOT"

n="$(wc -l <"$GOT")"
if [ "$n" -lt 32 ]; then
    echo "FAIL: only $n/32 timing rows captured in $CYCLES cycles"
    echo "--- harness tail ---"
    tail -20 "$LOG"
    exit 1
fi

if [ "${TIMING_TEST_UPDATE:-0}" = "1" ]; then
    cp "$GOT" "$BASELINE"
    echo "baseline updated: $BASELINE"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "FAIL: baseline missing ($BASELINE); create it with TIMING_TEST_UPDATE=1"
    exit 1
fi

if ! diff -u "$BASELINE" "$GOT" >"$WORK/diff.txt"; then
    echo "FAIL: timing rows diverged from baseline (row = 1-based line):"
    cat "$WORK/diff.txt"
    exit 1
fi

echo "OK: 32 timing rows match baseline"
