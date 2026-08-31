#!/usr/bin/env bash
#
# Demo Reel 3 on Rigel, with a real Kickstart, in a window.
#
#   tools/demoreel/oracle.sh [extra harness options...]
#
# This is the reference the Bellatrix runs are compared against: the same
# demo on the same chipset, with an operating system that is known to run it.
# Having no Kickstart is a property of the machine we are building; here it is
# an oracle, the same way a test uses an expected value it does not ship.
#
# In the window:
#
#   F12          take or release the mouse (the first click in the window
#                also takes it)
#   Esc          release it
#
# The demo is started the way it was meant to be: double-click DemoReel3.
# Headless runs cannot do that without --mouse, and --mouse does not yet
# manage the double-click -- see AI_context/consolidated/demoreel_oracle.md.
#
# Useful additions:
#
#   --audio-out out/oracle/run.wav    write Paula's mix; it reports peak/rms
#                                     at the end, so "silent" is a fact
#   --screenshot-every 500 --screenshot-dir out/oracle
#   --trace                           Rigel's structured events on stderr
#   --cycle-exact                     the honest cost model
#
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

KICKSTART="${KICKSTART:-$HOME/bellatrix-legacy/src/roms/KS13.rom}"
DISKS="${DISKS:-$HOME/newtek_demoreel_3}"
D1="${D1:-$DISKS/Demo Reel 3 (1989)(Newtek)(Disk 1 of 2)/Demo Reel 3 (1989)(Newtek)(Disk 1 of 2).adf}"
D2="${D2:-$DISKS/Demo Reel 3 (1989)(Newtek)(Disk 2 of 2)/Demo Reel 3 (1989)(Newtek)(Disk 2 of 2).adf}"
HARNESS="${HARNESS:-$ROOT/out/rigel-harness/rigel-harness}"

for f in "$KICKSTART" "$D1" "$D2" "$HARNESS"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

# 512 KB chip plus 512 KB slow is a 1 MB A500, and the demo needs it: with
# chip alone the Workbench answers "Volume RAM is full" while ToRAM copies,
# and DoWeHaveMem exists to print "This Demo Requires a 1 Meg Machine".
exec "$HARNESS" "$KICKSTART" \
    --adf "$D1" --df1 "$D2" \
    --cpu 68000 --chip 512 --slow 512 --scale 2 \
    "$@"
