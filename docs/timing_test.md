# Cross-emulator E-clock timing probe

An independent, hardware-referenced cycle-timing oracle for the Bellatrix
chipset, imported from [LinuxJedi/Copperline](https://github.com/LinuxJedi/Copperline)
(`timing-test/`). It complements the QEMU boot oracle: QEMU checks *liveness*,
this checks *timing*.

## What it is

`timing-test` is a bootable Amiga diagnostic disk. Its boot block loads a small
program to `$30000` and hands over the whole machine — interrupts and DMA off,
no ROM calls — so measurements reflect pure hardware timing with no OS
perturbation. It runs 32 probes and reports each as a CIA-A **E-clock** tick
count over the serial port (and on screen):

slow/chip RAM read & write, `move`, shift, `mul`, `dbra`, frame length, DMA
contention, interrupt latency, blitter clear/fill/line, and copper-vs-CPU
phase. A few rows are raw `VHPOSR` beam positions rather than tick counts.

Because it is anchored to the E-clock, the same disk run under different
emulators (Copperline, vAmiga, FS-UAE) or on real hardware yields comparable
numbers — any divergence pinpoints a specific timing discrepancy. That is
exactly the differential we want against Rigel/Agnus.

## How it lives in the tree

- Source is the `external/copperline` submodule (GPL-3). We keep the boundary
  clean: **no GPL source is copied into the Bellatrix tree**; we only build and
  boot its output.
- The `timing-test.adf` is **not committed** (it is a GPL-3 build artifact).
  It is regenerated on the fly from the committed `boot.bin` / `test.bin` with
  `make_adf.py` — **no `vasm` required**. (`vasm` is only needed to reassemble
  the `.asm` sources, which we do not do.)
- `tests/integration/harness_timing_test.sh` builds the ADF, boots it under the
  Musashi harness with `KS13.rom`, captures the 32 serial rows, and diffs them
  against `tests/integration/timing_test_baseline.txt`.
- Registered as CTest `bellatrix_harness_timing_test` (skips with exit 77 when
  the submodule is not checked out).

## Running

```bash
# via ctest
cd build_harness_rigel && ctest -R bellatrix_harness_timing_test --output-on-failure

# directly
bash tests/integration/harness_timing_test.sh \
    build_harness_rigel/harness \
    src/roms/KS13.rom \
    external/copperline/timing-test \
    tests/integration/timing_test_baseline.txt

# after an intended timing change, re-lock the baseline:
TIMING_TEST_UPDATE=1 bash tests/integration/harness_timing_test.sh <same args>
```

## Scope

This is a **regression guard**, not yet a full cross-emulator accuracy check.
The committed baseline is *Bellatrix's own* numbers under a fixed config
(`KS13.rom`, harness default CPU/RAM). Absolute tick counts depend on CPU model,
ROM, and memory layout, so matching Copperline's or FS-UAE's reference values
requires a **config-matched run** — see `external/copperline/timing-test/compare.py`
(its reference is an FS-UAE A500+ / 68EC020 @7MHz / KS2.05 / 2M chip + 512K slow).

## First finding: the "all-zeros" rows were a CIA bug

The first run of this disk exposed a real Rigel defect on its very first use.
Every row measured via the CIA-A timer-A E-clock stopwatch (rows 0-8, 10-15, 18,
28-30) read `0`, while every row measured from the beam (VHPOSR / per-frame
counts) produced real numbers. Root cause: `cia_timers_write_reg` reloaded the
timer counter from the latch whenever CRA/CRB was written with `START=0` and
`RUNMODE=1` (one-shot). The test's `tread` stops the one-shot timer to read it
(`CRA=$08`), which tripped that reload and clobbered the count back to `$FFFF`,
so `elapsed = ~$FFFF = 0`. Real 8520 hardware reloads only on the LOAD strobe or
an underflow — not on stop. Fixed in `external/rigel/src/cia/cia_timers.c` with a
regression test (`external/rigel/tests/test_cia.c`, "one-shot stop-to-read"); all
32 rows now report data. This is exactly the class of blind spot the disk exists
to surface.

## Remaining divergence (expected)

With the CIA fix in, the CPU-bound rows (`move`, shift, `dbra`, `mul`, RAM
reads/writes) still differ from `compare.py`'s FS-UAE reference — because that
reference is a **68EC020 @7MHz** and the harness runs a 68040+FPU by default, so
instruction cycle counts legitimately differ. A few bus/timing-anchored rows
already line up closely (slow-RAM write, `mul`, frame length). Turning this into
a true accuracy check means a config-matched run (same CPU/ROM/RAM as a reference
emulator) and a row-by-row diff — the natural follow-up, now that the reference
clock itself is trustworthy.
