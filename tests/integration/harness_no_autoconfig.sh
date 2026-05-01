#!/usr/bin/env bash
set -euo pipefail

HARNESS="$1"
ROM="$2"
ADF="$3"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

"$HARNESS" "$ROM" --adf "$ADF" --headless --cycles 5000 >"$LOG" 2>&1

grep -F "[AUTOCONFIG] disabled; config window=00e80000 returns empty" "$LOG" >/dev/null

if grep -F "[AUTOCONFIG] board assigned" "$LOG" >/dev/null; then
    echo "unexpected AutoConfig assignment"
    cat "$LOG"
    exit 1
fi

if grep -F "[AUTOCONFIG] shutup" "$LOG" >/dev/null; then
    echo "unexpected AutoConfig shutup"
    cat "$LOG"
    exit 1
fi
