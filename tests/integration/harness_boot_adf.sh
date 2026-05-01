#!/usr/bin/env bash
set -euo pipefail

HARNESS="$1"
ROM="$2"
ADF="$3"
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

"$HARNESS" "$ROM" --adf "$ADF" --headless --cycles 2000000 >"$LOG" 2>&1

grep -F "[HARNESS] Reset vectors: ISP=0x11144ef9  PC=0x00f800d2" "$LOG" >/dev/null
grep -F "[AUTOCONFIG] disabled; config window=00e80000 returns empty" "$LOG" >/dev/null
grep -F "[FLOPPY] DF0 ADF inserted size=901120" "$LOG" >/dev/null
grep -F "[VBL-ENTER] frame=1 hpos=0 vpos=0" "$LOG" >/dev/null
grep -F "[VBL-ENTER] frame=14 hpos=0 vpos=0 dmacon=0x0000 intena=0x0000 intreq=0x0020 pending=0x0000" "$LOG" >/dev/null
grep -F "[COPPER] vbl_reload skipped - COPEN off (dmacon=0000)" "$LOG" >/dev/null
grep -E "\\[HARNESS\\] Done\\.  cycles=[0-9]+  frames=15  PC=0x00f800e2" "$LOG" >/dev/null
