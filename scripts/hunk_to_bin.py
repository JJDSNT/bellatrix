#!/usr/bin/env python3
"""Extract raw code bytes from a single-hunk AmigaOS HUNK_CODE binary.

Reads a HUNK file produced by vasmm68k_mot -Fhunk and writes the raw
code section bytes to stdout (or an output file).

Usage: hunk_to_bin.py <input.o> [output.bin]
"""
import struct, sys

HUNK_UNIT   = 0x3E7
HUNK_NAME   = 0x3E8
HUNK_CODE   = 0x3E9
HUNK_DATA   = 0x3EA
HUNK_BSS    = 0x3EB
HUNK_END    = 0x3F2
HUNK_RELOC32 = 0x3EC
HUNK_SYMBOL  = 0x3F0
HUNK_HEADER  = 0x3F3


def extract(path):
    with open(path, 'rb') as f:
        data = f.read()

    pos = 0

    def read32():
        nonlocal pos
        val = struct.unpack_from('>I', data, pos)[0]
        pos += 4
        return val

    def skip_string():
        n = read32()
        nonlocal pos
        pos += n * 4

    htype = read32()

    if htype == HUNK_HEADER:
        # Loadable binary: skip header table
        skip_string()               # resident libs (usually 0)
        num_hunks = read32()
        read32(); read32()          # first/last hunk
        for _ in range(num_hunks):
            read32()                # hunk sizes
    elif htype == HUNK_UNIT:
        # Object file: skip unit name
        skip_string()
    else:
        sys.exit(f'{path}: not a HUNK file (first word: 0x{htype:x})')

    result = bytearray()
    while pos < len(data):
        htype = read32()
        if htype == HUNK_END:
            continue                # keep scanning for more sections
        if htype in (HUNK_CODE, HUNK_DATA):
            size_longs = read32()
            result.extend(data[pos:pos + size_longs * 4])
            pos += size_longs * 4
        elif htype == HUNK_BSS:
            read32()                # size only, no bytes
        elif htype == HUNK_NAME:
            skip_string()           # section name
        elif htype == HUNK_SYMBOL:
            # skip symbol table entries
            while True:
                n = read32()
                if n == 0: break
                pos += n * 4        # symbol name
                read32()            # symbol value
        elif htype == HUNK_RELOC32:
            # skip relocation info
            while True:
                n = read32()
                if n == 0: break
                read32()            # hunk number
                pos += n * 4        # offsets
        elif pos >= len(data):
            break
        else:
            break                   # unknown/end chunk

    return bytes(result)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(f'Usage: {sys.argv[0]} <input.o> [output.bin]')
    raw = extract(sys.argv[1])
    if len(sys.argv) >= 3:
        with open(sys.argv[2], 'wb') as f:
            f.write(raw)
    else:
        sys.stdout.buffer.write(raw)
