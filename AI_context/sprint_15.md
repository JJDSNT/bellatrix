# Sprint 15 — Paula disk DMA, floppy, VPOSR chip ID, SERDATR TBE

## Date
2026-04-25

## Goal
Complete Paula disk DMA + floppy drive wiring; fix VPOSR chip ID for KS3.1
hardware detection; fix SERDATR TBE toggling that was trapping AROS in a
serial poll loop; repair stale emu68 build patch.

## Key changes delivered

### New: PaulaDisk (src/chipset/paula/paula_disk.c/h)
Full DMA-read stub: DSKPTH/L, DSKLEN, DSKSYNC, ADKCON, DSKBYTR registers;
chip-RAM DMA burst on DMAEN rising edge; fake-DMA cycle budget (46000 cycles
per track); INDEX pulse forwarded to CIA-B TOD; DSKBYTR.WORDSYNC set on sync
word match during DMA.

### New: FloppyDrive (src/chipset/floppy/floppy_drive.c/h)
ADF image load (open, track cache); cylinder/side step; DSKCHG/TRACK0/READY
signals; `encode_adf_track_to_mfm` with correct clock-bit insertion.

### Paula wiring (paula.c/h)
- UART TX callback → `[SERIAL]` printf with line-buffer
- SERDAT/SERPER/SERDATR read/write delegated to UARTState
- POTGOR stub (always 0xFF00)
- PaulaDisk attached and stepped from paula_step()

### Machine wiring (machine.c/h, bellatrix.c)
- INDEX pulse from beam VSYNC into `paula_disk_step()`
- `paula_disk_step` forwarded from `machine_advance`

### Fix: SERDATR TBE always-1 (uart.c)
When `tx_instant=true`, `uart_write_serdat()` emits byte and fires TBE IRQ
immediately **without** touching `tx_buffer_valid` or `tx_shift_busy`. This
means `uart_read_serdatr()` always sees TBE=1 and TSRE=1 during boot.

**Why this mattered**: AROS polls SERDATR at PC=0xFE85FA in a tight loop
waiting for TBE=1. The old code entered the buffer-state machine even in
instant mode, causing tx_shift_busy=true for 10 cycles → TBE=0 → AROS looped
~20 times per byte transmitted. After the fix AROS-LOOP count drops to zero
and execution reaches PC=0xFE98xx within frame 41.

### Fix: VPOSR chip ID (agnus.c)
`agnus_get_beam()` now returns:
```c
lof = s->beam.lof ? 0x8000 : 0;
vpos8 = (s->beam.vpos >> 8) & 1;
*vposr_out = lof | (0x20 << 8) | vpos8;   // 0x20 = ECS Fat Agnus 8372A PAL
```
Previously returned only `vpos[8]` (bits [14:8] = 0). KS3.1 reads VPOSR for
chipset identification; leaving those bits zero is treated as invalid hardware
on some initialisation paths.

### Fix: Emu68 build patch 0001 regenerated (patches/0001-...)
The patch file was stale — it did not include paula_disk.c and floppy_drive.c
which were added in sprint 13. The emu68/CMakeLists.txt was already patched
on-disk but marked `assume-unchanged`, so `git apply --reverse --check` failed
and setup.sh exited with an error.

Fix procedure:
1. `git update-index --no-assume-unchanged CMakeLists.txt`
2. `git diff HEAD -- CMakeLists.txt > patches/0001-...patch` (new patch)
3. `git update-index --assume-unchanged CMakeLists.txt` (restore)

Also fixed in `paula_disk.c` for AArch64 `-Werror`:
- `write_be16`: marked `__attribute__((unused))`
- Loop variable in `encode_adf_track_to_mfm`: `int sec` → `unsigned int sec`

## AROS boot progress

| Metric | Before | After |
|--------|--------|-------|
| AROS-LOOP count | 1–20+ per tx | 0 |
| AROS PC at frame 41 | stuck ~0xF8011x | 0xFE980e |
| SERDATR TBE | oscillates 0/1 | always 1 |

AROS is now spinning in the 0xFE98xx range (deep in early init). Next blocker
to investigate.

## KS3.1 status (before VPOSR fix)
KS3.1 was stuck at 0xF8363A (LED blink loop) after 2000 frames:
- DMACON pattern: clear → DMAEN → DSKEN → BLTEN → 2 tiny blits → reset
- Suspected blitter test failure; may also be affected by VPOSR=0 chip ID

VPOSR chip ID fix committed; KS3.1 re-test pending.

## Build status
- Harness (Musashi, Linux): green
- Emu68 (AArch64 bellatrix): green
- setup.sh: all 3 patches "already applied"

## Commits this sprint
- `c0b5c44` chipset: paula disk DMA + floppy drive + machine wiring
- `14fc0f3` harness: floppy/paula_disk build, AROS-LOOP debug, paula_attach_memory
- `f71785d` fix: VPOSR chip ID, SERDATR TBE instant, build errors

## Next steps
1. Run KS3.1 with new VPOSR (chip ID 0x20) and check if LED blink disappears
2. Investigate AROS stall at 0xFE98xx — identify the new blocking register/signal
3. VPOSW (0xDFF02A) / VHPOSW (0xDFF02C) write handlers (beam position write-back)
4. Blitter line-mode: implement 8-octant pixel writes (KS3.1 blitter test may need it)
