#!/usr/bin/env bash
set -euo pipefail

HARNESS="$1"
ROM="$2"
ADF="$3"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

"$HARNESS" "$ROM" --adf "$ADF" --headless --cycles 5000 >"$LOG" 2>&1

grep -F "[MEMMAP] expansion backend: harness zorro2 bus" "$LOG" >/dev/null
grep -F "[Z2] all boards configured" "$LOG" >/dev/null

if grep -F "[AUTOCONFIG]" "$LOG" >/dev/null; then
    echo "unexpected legacy AutoConfig activity"
    cat "$LOG"
    exit 1
fi

if grep -F "[Z2] board '" "$LOG" >/dev/null; then
    echo "unexpected Zorro2 board activity"
    cat "$LOG"
    exit 1
fi
