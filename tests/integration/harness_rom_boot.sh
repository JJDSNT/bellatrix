#!/usr/bin/env bash
# ROM boot display regression test.
#
# Boots a ROM headless (no media) and asserts the boot screen was actually
# programmed and rendered by the dump frame:
#   - the Rigel frame export left the 256x256 power-up default (a 256x256
#     export means the copper/display were never configured — exactly the
#     KS20 drive-ID regression signature, see ISSUE-0037);
#   - the exported frame is not a flat single-color raster.
#
# Usage: harness_rom_boot.sh <harness> <rom> <dump_frame>
# Exit codes: 0 pass, 77 skip (ROM not present), 1 fail.
set -euo pipefail

HARNESS="$1"
ROM="$2"
DUMP_FRAME="$3"

if [ ! -f "$ROM" ]; then
    echo "SKIP: ROM not present: $ROM"
    exit 77
fi

LOG="$(mktemp)"
PPM="$(mktemp --suffix=.ppm)"
trap 'rm -f "$LOG" "$PPM"' EXIT

BELLATRIX_RIGEL_TRACE=1 \
BELLATRIX_RIGEL_DUMP_FRAME="$DUMP_FRAME" \
BELLATRIX_RIGEL_DUMP_PPM="$PPM" \
"$HARNESS" "$ROM" --headless --frames $((DUMP_FRAME + 20)) >"$LOG" 2>&1

DUMP_LINE="$(grep -F "[RIGEL-DUMP] frame=${DUMP_FRAME} " "$LOG" || true)"
if [ -z "$DUMP_LINE" ]; then
    echo "FAIL: no [RIGEL-DUMP] line for frame ${DUMP_FRAME}"
    tail -20 "$LOG"
    exit 1
fi

SIZE="$(sed -n 's/.* size=\([0-9]*x[0-9]*\).*/\1/p' <<<"$DUMP_LINE")"
WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"
echo "boot frame ${DUMP_FRAME}: export ${SIZE}"

if [ "$SIZE" = "256x256" ]; then
    echo "FAIL: export still at 256x256 power-up default — display never programmed"
    exit 1
fi
if [ "$WIDTH" -lt 320 ] || [ "$HEIGHT" -lt 190 ]; then
    echo "FAIL: export ${SIZE} smaller than any known boot screen"
    exit 1
fi

python3 - "$PPM" <<'PYEOF'
import sys

path = sys.argv[1]
with open(path, 'rb') as f:
    data = f.read()

# P6 header: three tokens (magic, width, height) + maxval, comments allowed
tokens, i = [], 0
while len(tokens) < 4:
    while i < len(data) and data[i:i+1].isspace():
        i += 1
    if data[i:i+1] == b'#':
        while i < len(data) and data[i:i+1] != b'\n':
            i += 1
        continue
    j = i
    while j < len(data) and not data[j:j+1].isspace():
        j += 1
    tokens.append(data[i:j])
    i = j
i += 1
assert tokens[0] == b'P6', tokens
w, h = int(tokens[1]), int(tokens[2])

pixels = data[i:i + w * h * 3]
colors = {}
for p in range(0, len(pixels) - 2, 3):
    c = pixels[p:p+3]
    colors[c] = colors.get(c, 0) + 1

total = len(pixels) // 3
dominant = max(colors.values()) / total
print(f"colors={len(colors)} dominant={dominant:.4f}")
if len(colors) < 4:
    sys.exit(f"FAIL: only {len(colors)} distinct colors — flat/blank boot frame")
if dominant > 0.995:
    sys.exit(f"FAIL: one color covers {dominant:.2%} of the frame — blank boot screen")
PYEOF

echo "PASS"
