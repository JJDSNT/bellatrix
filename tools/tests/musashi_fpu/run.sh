#!/bin/bash
# Regression test for the FPU opmodes added to external/musashi/m68kfpu.c
# (ISSUE-0034). Generates a tiny M68K test ROM, assembles it via the
# project's docker vasm wrapper, boots it in the harness (--cpu 68040),
# and diffs the computed results (dumped from chip RAM) against expected
# values computed independently in Python.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cd "$ROOT"

echo "[fpu-test] generating fputest.S..."
python3 "$HERE/gen_fputest.py"

echo "[fpu-test] assembling (docker vasm)..."
TTY_ENABLED="" ./emu68/build-scripts/build-m68k-amigaos vasmm68k_mot \
    -Fbin -quiet -o "$HERE/fputest.rom" "$HERE/fputest.S"

echo "[fpu-test] running harness (--cpu 68040)..."
KICKSTART="$HERE/fputest.rom" HARNESS_CPU=68040 FRAMES=6 \
    HARNESS_SCREENSHOT_FRAMES=5 HARNESS_SCREENSHOT_DIR="$WORK" \
    HARNESS_CHIPDUMP=1000:60 \
    ./run.sh harness > "$WORK/run.log" 2>&1

if grep -q "F-LINE-TRAP" "$WORK/run.log"; then
    echo "[fpu-test] FAIL: an FPU opcode trapped (unimplemented) — see $WORK/run.log"
    grep "F-LINE-TRAP" "$WORK/run.log"
    exit 1
fi

DUMP="$WORK/chip_5_01000.bin"
if [ ! -f "$DUMP" ]; then
    echo "[fpu-test] FAIL: chip RAM dump not produced — see $WORK/run.log"
    exit 1
fi

echo "[fpu-test] verifying results..."
python3 - "$DUMP" "$HERE/expected.json" <<'EOF'
import json, struct, sys
dump_path, expected_path = sys.argv[1], sys.argv[2]
data = open(dump_path, "rb").read()
expected = json.load(open(expected_path))
names, exp = expected["names"], expected["expected"]
n = len(names)
vals = struct.unpack(">%di" % n, data[:n * 4])
ok = True
for name, e, v in zip(names, exp, vals):
    status = "OK" if v == e else "MISMATCH"
    if v != e:
        ok = False
    print(f"  {name:10s} expected={e:6d} got={v:6d}  {status}")
sys.exit(0 if ok else 1)
EOF

echo "[fpu-test] PASS — all opmodes correct"
