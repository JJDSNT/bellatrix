#!/usr/bin/env python3
"""Assemble lide.rom from bootldr nibbles + lide.device binary.

ROM layout (32 KB, matching bootrom/rom.ld):
  0x0000  LONG  'LIV2'  header magic
  0x0004  boot loader nibbles (max 0x7FC bytes), padded with 0xFF to 0x0800
  0x0800  reserved, filled 0xFF to 0x1000
  0x1000  lide.device binary, padded with 0xFF to 0x7FFC
  0x7FFC  LONG  'LIDE'  footer magic
"""
import struct, sys, os

ROM_SIZE        = 0x8000
HDR_MAGIC       = 0x4C495632  # 'LIV2'
FOOT_MAGIC      = 0x4C494445  # 'LIDE'
BOOTLDR_OFFSET  = 0x0004
BOOTLDR_LIMIT   = 0x0800
DEVICE_OFFSET   = 0x1000
FOOTER_OFFSET   = ROM_SIZE - 4


def main(lide_dir, out_path):
    nibbles_path = os.path.join(lide_dir, 'bootrom', 'obj', 'bootnibbles')
    device_path  = os.path.join(lide_dir, 'lide.device')

    with open(nibbles_path, 'rb') as f:
        nibbles = f.read()
    with open(device_path, 'rb') as f:
        device = f.read()

    max_nibbles = BOOTLDR_LIMIT - BOOTLDR_OFFSET
    if len(nibbles) > max_nibbles:
        sys.exit(f'bootnibbles too large: {len(nibbles)} > {max_nibbles}')

    max_device = FOOTER_OFFSET - DEVICE_OFFSET
    if len(device) > max_device:
        sys.exit(f'lide.device too large: {len(device)} > {max_device}')

    rom = bytearray(b'\xff' * ROM_SIZE)
    struct.pack_into('>I', rom, 0, HDR_MAGIC)
    rom[BOOTLDR_OFFSET:BOOTLDR_OFFSET + len(nibbles)] = nibbles
    rom[DEVICE_OFFSET:DEVICE_OFFSET + len(device)]    = device
    struct.pack_into('>I', rom, FOOTER_OFFSET, FOOT_MAGIC)

    with open(out_path, 'wb') as f:
        f.write(rom)
    print(f'lide.rom: {ROM_SIZE} bytes  '
          f'(device {len(device)} bytes, nibbles {len(nibbles)} bytes)')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(f'Usage: {sys.argv[0]} <lide_dir> <output_rom>')
    main(sys.argv[1], sys.argv[2])
