# emu68sd — the second SD driver

`emu68sd_*.c/h` are Emu68's own `brcm-sdhc.device`
(https://github.com/michalsc/brcm-sdhc.device), copied here to become a
**second, distinct driver** for the same controller — not a modification of the
one we already have.

There are two, on purpose. `sdcard.md`'s 2×2 matrix compares a driver against a
filesystem, and that comparison means nothing if the two drivers are one driver
with a switch inside it. `SDCARD_BACKEND` in `mmakefile.src` already selects
between backends; this becomes a third value beside `sdhost` and the Arasan one.

## Why this one is worth having

It drives the same silicon on the same machine and boots a Pi every day, and it
does it **in PIO, with no DMA and no block cache at all**. Ours requires DMA —
`sdcard_sdhost_init.c` fails init without a channel — so the two are not
variations on a theme, they are different answers.

Its transfer loop reads the FIFO level **once per burst** and then moves the
whole burst without asking again, where a per-word loop pays an MMIO read,
JITted, for every four bytes:

```c
words = (rd32(sc, HC_DEBUG) >> 4) & 0x1f;   /* FIFO level, once */
if (words < burst) continue;
while (words--) *buff++ = rd32be(sc, HC_DATAPORT);
```

## The detail that will bite whoever finishes this

`rd32` byte-swaps and `rd32be` does not, and which is used where is not
arbitrary:

- **control registers** are swapped (`rd32`/`wr32`);
- **the data port is raw** (`rd32be`/`wr32be`).

A byte stream read as a 32-bit word on a big-endian CPU and stored back raw
keeps its byte order. Swapping it would reverse every group of four. Our
`sdhost_read()` swaps unconditionally, so it must not be used for `SDDATA`.

The registers themselves map one-to-one with ours:

| brcm-sdhc | ours | |
|---|---|---|
| `HC_DEBUG` 0x34 | `SDEDM` 0x34 | FIFO level at bits 4..8 |
| `HC_DATAPORT` 0x40 | `SDDATA` 0x40 | |
| `HC_FIFO_SIZE` 16, `HC_FIFO_BURST` 8 | — | |

## What remains

These files are not in the build. They are Emu68's driver in Emu68's shape: its
own `struct SDCardBase` with function pointers, its own command layer, its own
unit task. AROS's `sdcard.device` wants eleven functions instead
(`sdcard_sdhost_intern.h`): `SoftReset`, `SetClock`, `SetPowerLevel`,
`SetBusWidth`, `SendCmd`, `WaitCmd`, `FinishCmd`, `FinishData`, `BusIRQ`,
`GetClockDiv`, `BusInit`/`BusPostIRQInit`.

Porting the logic into that shape is the work. Copying the files was not it.
