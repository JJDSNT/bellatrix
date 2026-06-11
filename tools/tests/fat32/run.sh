#!/bin/bash
# Host-side regression test for src/storage/fat (LFN + unicode + open/read).
# Builds a FAT32 disk image with mkfs.fat/mtools, compiles the reader
# natively and checks listing/open semantics against it.
#
# Prerequisites: gcc, dosfstools (mkfs.fat), mtools (mcopy/mmd/mlabel), python3

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../../src"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"

# --- FAT32 filesystem (63 MB) with LFN/unicode content -----------------------
dd if=/dev/zero of=part.img bs=1M count=63 status=none
mkfs.fat -F 32 part.img >/dev/null

echo "conteudo ADF teste 12345" > f1
printf 'ISOISOISO' > f2
echo "outro arquivo" > f3

mcopy -i part.img f1 "::Turrican II - The Final Fight (1991) Edição Açãoçé.adf"
mcopy -i part.img f2 "::Some Really Long CD Image Name With Many Many Words In It.iso"
mcopy -i part.img f3 "::README.txt"
mcopy -i part.img f1 "::GAME.ADF"
mmd   -i part.img "::Subdir With Long Name"
mlabel -i part.img ::TESTVOL

# --- MBR with one 0x0C partition at LBA 2048 ---------------------------------
dd if=/dev/zero of=disk.img bs=512 count=2048 status=none
cat part.img >> disk.img
python3 - <<'EOF'
import struct
with open('disk.img', 'r+b') as f:
    f.seek(446)
    f.write(struct.pack('<B3sB3sII', 0x00, b'\x00\x02\x00', 0x0C,
                        b'\xfe\xff\xff', 2048, 63 * 2048))
    f.seek(510)
    f.write(b'\x55\xaa')
EOF

# --- build & run --------------------------------------------------------------
gcc -Wall -Wextra -I"$SRC" -I"$SRC/storage/fat" -o test_fat32 \
    "$HERE/test_fat32.c" \
    "$SRC/storage/fat/fat32.c" \
    "$SRC/storage/fat/fat32_lfn.c" \
    "$SRC/storage/fat/fat32_unicode.c"

./test_fat32 disk.img
