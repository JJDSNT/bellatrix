#!/usr/bin/env bash
set -euo pipefail

HARNESS="$1"
ROM="$2"
ADF="$3"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

"$HARNESS" "$ROM" --adf "$ADF" --headless --cycles 2000000 >"$LOG" 2>&1

grep -F "[HARNESS] Reset vectors: ISP=0x11144ef9  PC=0x00f800d2" "$LOG" >/dev/null
grep -F "[MEMMAP] expansion backend: board_registry (harness)" "$LOG" >/dev/null
grep -F "[HARNESS] Fast RAM: 8MB Zorro II board configured" "$LOG" >/dev/null
if grep -F "[Z2] board '" "$LOG" >/dev/null; then
    echo "unexpected Zorro2 board activity"
    cat "$LOG"
    exit 1
fi
grep -F "[RIGEL] DF0 ADF inserted size=901120" "$LOG" >/dev/null
grep -F "[HARNESS] Chipset backend: rigel" "$LOG" >/dev/null
grep -E "\\[HARNESS\\] Done\\.  cycles=[0-9]+  frames=15  PC=0x00f8" "$LOG" >/dev/null
