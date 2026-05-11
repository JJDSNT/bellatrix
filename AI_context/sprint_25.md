// AI_context/sprint_25.md

# Sprint 25 — Harness disk-path closure and post-DSKBLK boot tracing

## Description

This sprint closed the remaining ambiguity around the KS13 harness disk path
and moved the investigation boundary forward again.

The immediate trigger was a user report that:

- the insert-disk screen still does not appear
- several recent changes already landed across sprints 23 and 24
- the disk path needed to be verified end-to-end before continuing to blame
  video/bootstrap behavior

The work therefore focused on:

1. proving whether DF0 + ADF insertion was really active in the harness path
2. proving whether Paula disk DMA was actually transferring data
3. proving whether the CPU really ACKed `DSKBLK`
4. checking whether any visible-screen payload starts to appear immediately
   after the disk IRQ is handled

## Main conclusion

The harness disk path is now confirmed to be working far enough that:

- DF0 media insertion is active
- Paula performs real disk DMA reads
- `DSKBLK` is raised
- the CPU acknowledges the disk interrupt

So the current missing insert-disk screen is no longer best explained by
“disk DMA never happened”.

The stronger working hypothesis now is:

- boot progresses past disk completion
- but the producer that should populate the eventual boot / insert-disk
  bitplane payload either never runs, runs too late, or writes somewhere other
  than the originally watched payload windows

## What changed

### 1. Launcher → `run.sh` ADF handoff bug fixed

The launcher was already exporting:

- `ADF=/path/to/disk.adf`

But `run.sh` was not reading that field back from the launcher output file.

Fix:

- `load_launcher_selection()` now imports `ADF` alongside
  `KICKSTART`, `DISPLAY_MODE`, `EMU_PROFILE`, and `BOOTARGS`

Impact:

- selecting an ADF in the launcher now actually passes `--adf ...` to the
  harness binary

### 2. Floppy / Paula logging was tightened around real read activity

The logging was adjusted so the useful disk-read milestones are:

- disk inserted / motor / step transitions in `floppy_drive.c`
- DMA start / prepared sync in `paula_disk.c`
- actual read activity only when `paula_disk_dma_service_grant()` copies the
  first word to Chip RAM
- completion when the transfer finishes and `DSKBLK` fires

That clarified an important detail:

- the repeated `13630`/`14716` byte transfers seen in logs are track-style DMA
  requests, not “whole disk reads”

### 3. `DSKBLK` ACK checkpoint added in the harness

The Musashi backend gained a focused checkpoint:

- `[BOOT-DSKBLK-ACK]`

It triggers only on real disk IRQ acknowledgements:

- `INTREQ raw=0002`
- `INTREQ raw=1002`

It captures:

- `pc`
- `D0/D1/D2`
- `A5/A6`
- `intena/intreq`
- `dmacon`
- `bplcon0`
- `bpl1/bpl2`
- `cop1/cop2`
- payload probes at `0x0a572` / `0x0c4b2`

This removes the earlier ambiguity where generic `INTREQ` traffic could look
like a disk completion path.

### 4. Short post-`DSKBLK` trace window added

The harness now opens a small post-ACK trace window:

- `[BOOT-AFTER-DSK]`

This emits only for a short range of interesting PCs after a real `DSKBLK`
acknowledgement, covering the known late boot/video path hotspots.

Purpose:

- determine whether the machine transitions from disk completion into visible
  display setup
- observe whether `BPLCON0`, bitplane pointers, and Copper state become valid
  immediately after disk completion

### 5. Boot payload watch was widened

The original payload watches were very narrow:

- around `0x0a572`
- around `0x0c4b2`

This sprint widened the search to:

- `0x0a000..0x0afff`
- `0x0c000..0x0cfff`

New non-zero write markers:

- `[BOOT-DISPLAY-BLOCK-W]`
- `[WATCH-BOOT-PAYLOAD-W]`

Purpose:

- determine whether the boot screen producer writes somewhere nearby even if it
  does not exactly target the previously observed bitplane base addresses

### 6. Dynamic display-block tracking replaced the fixed-buffer assumption

The earlier payload watch still assumed that writes near `0x0a572` /
`0x0c4b2` were already the final visible bitplane payload.

The next pass refined that using the structure topology already recovered in
earlier sprints:

- `($1c,A5)` = display structure
- `($20,A5)` = associated pointer / area
- `($24,A5)` = large buffer (`0x1f40`)
- `($28,A5)` = second large buffer (`0x1f40`)

The harness now tracks this block dynamically in the `0xfe8768..0xfe88ff`
setup path and emits:

- `[BOOT-DISPLAY-BLOCK]`

with:

- `A5`
- `A5+1c`
- `A5+20`
- `A5+24`
- `A5+28`

The harness also tracks non-zero writes into the dynamically discovered
`A5+24` / `A5+28` buffers and emits:

- `[BOOT-DISPLAY-BUFFER-W]`

This is stronger than the fixed-range watch because it follows the actual ROM
allocated buffers instead of assuming a fixed payload window in advance.

## Key observed result

Representative real ACKs now look like:

- `pc=00fea1de raw=1002`
- `pc=00fc4af0 raw=0002`

And at those points the snapshot still showed:

- `dmacon=02d0`
- `bplcon0=1302`
- `bpl1=00000`
- `bpl2=00000`
- `p0=00000000`
- `p1=00000000`

Also important:

- in the earlier fixed-window pass, no writes were observed in the original
  `0x0a572` / `0x0c4b2` payload windows at the moment the real `DSKBLK` ACKs
  were captured

### 7. New observation: a display-related writer exists, but not yet the final payload

After widening and then refining the instrumentation, the harness showed a new
useful event:

- repeated writes at `pc=0x00fe9aaa`
- addresses around `0x00a450..0x00a490`
- values like `0xaaaaaaaa`

Interpretation:

- there is real boot-time display-block memory activity before the `DSKBLK`
  ACKs
- but this should not yet be assumed to be the final bitplane payload
- the pattern looks more like initialization / setup / filler than a useful
  boot image

Interpretation:

- disk completion is real
- the CPU consumes the disk interrupt
- but the visible-screen payload is still absent at or immediately after that
  point

This keeps the investigation aligned with sprint 24’s “missing bitmap
producer” conclusion, but now with the disk path removed as the primary
suspect and with a more specific boundary between:

- intermediate display-block writes
- and the still-unconfirmed final writes into `($24,A5)` / `($28,A5)`

## Recommended filtered harness view

For focused interactive inspection, use:

```bash
./run.sh harness | grep -E 'BOOT-(DISPLAY-BLOCK|DISPLAY-BUFFER-W|DSKBLK-ACK|AFTER-DSK)|WATCH-BOOT-PAYLOAD-W'
```

This gives a compact view of:

- dynamic display-block discovery
- non-zero writes into the dynamically tracked large display buffers
- real disk IRQ acknowledgements
- the short post-disk boot trace
- non-zero writes into the widened boot payload windows

This is the preferred log slice for deciding whether the boot path is:

1. reaching post-disk video setup
2. writing visible payload
3. or still failing before the bitmap producer stage

## Validation

Validated locally with:

- `bash -n run.sh`
- `cmake --build out/harness --target harness`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_harness_boot_adf`

Result:

- harness build passed
- boot-with-ADF test passed

## Current best model

The current best explanation for the missing insert-disk screen in the harness
is now:

- disk I/O has advanced far enough
- `DSKBLK` is not the blocker
- the next missing piece is more likely in:
  - promotion from display-block setup into the actual large display buffers
  - bitmap payload production into `($24,A5)` / `($28,A5)`
  - later display-setup sequencing
  - or a remaining mismatch between intermediate display-block writes and the
    final visible bitplane source

## Files touched in this sprint

- `run.sh`
- `src/chipset/floppy/floppy_drive.c`
- `src/chipset/paula/paula_disk.c`
- `tools/harness/musashi_backend.c`
