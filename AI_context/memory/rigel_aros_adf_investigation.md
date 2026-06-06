// AI_context/memory/rigel_aros_adf_investigation.md

# Rigel AROS ADF Investigation

Date: 2026-06-06

## Scope

Investigate `src/disks/bootdisk-amiga-m68k.adf` and `src/disks/megademoA.adf` with `src/roms/aros.rom` through the Rigel chipset backend. The user reports initial artifacts followed by a grey screen. Do not investigate the ISO boot path in this pass.

## Findings

- `aros.rom + bootdisk-amiga-m68k.adf` reaches AROS residents and screen setup but remains grey. This ADF is an AROS emergency boot floppy and expects an AROS Live CD for the full workflow, so it is not the best isolated ADF-only video test.
- `KS13.rom + megademoA.adf` produces a non-grey multicolor frame, so the ADF and baseline disk/video path are not globally broken.
- Before the current disk fix, `aros.rom + megademoA.adf` failed in the boot stub:
  - Stub at `0x978f4` issued `CMD_READ` for `offset=0x0d3c00`, `length=0x8400`, `data=0x070000`.
  - `DoIO` returned `io_Error=0x1b` and `io_Actual=0`.
  - The stub jumped to `0x70020` anyway, executing unloaded memory and triggering illegal instruction.
- The root cause for that first failure was Rigel disk DMA ignoring `ADKCON.WORDSYNC` for the DMA source position. AROS sets `ADKCON=0x9500`; DMA should start at the detected `DSKSYNC` word, but Rigel copied from the beginning of the generated track gap.
- A diagnostic MFM decoder was added to `external/rigel/src/chipset/paula/disk.c`. The diagnostic initially missed the AROS-style `0x55555555` mask in `getmfmlong`; this was fixed in the diagnostic helper.
- After changing Rigel disk DMA to set `dma_src_offset=sync_offset` when `ADKCON.WORDSYNC` is enabled, the same AROS megademo read succeeds:
  - Tracks `154..159` are read.
  - `DoIO` returns `io_Error=0`, `io_Actual=0x8400`.
  - The boot stub reaches `jmp $70020` with the payload loaded.

## Current Status

- The first blocking AROS disk read failure is fixed locally in `external/rigel/src/chipset/paula/disk.c`.
- `aros.rom + megademoA.adf` still does not show the expected demo screen:
  - With the default 68000, execution later reaches `0x12022`, calls `0x12a88`, and hits an illegal opcode (`0x1f8b`). This is after the fixed disk read.
  - With `--cpu 68020`, that illegal instruction disappears, which suggests this later issue is CPU-model dependent.
  - Even with `--cpu 68020`, frame dumps around 2200 remain effectively grey/black: `#aaaaaa` plus black pixels, with `BPLCON0=0201` and DMA enabled.
- The remaining grey screen is therefore not the original failed final ADF read. It is a later video/bitplane/timing or payload execution issue.

## Notes On User-Supplied Hypotheses

- The WORDSYNC concern is relevant and was the cause of the first AROS megademo failure. AROS can skip junk before sync after the first failed decode attempt in principle, but the fixed-length DMA buffer lost enough useful stream window that the last sector/read path failed in practice.
- `disk_changed` clearing via `floppy_step()` still looks plausible and was not identified as the current blocker.
- OFS handler availability may be relevant for `bootdisk-amiga-m68k.adf`, but it is not the immediate cause for the `megademoA.adf` loader failure.
- DSKBLK/INTENA propagation appears good enough for the successful final `0x8400` read after the WORDSYNC fix; no interrupt-chain blocker was proven in this pass.

## Validation Run

- Rebuilt harness:
  - `rtk cmake --build out/harness-rigel --target harness -j2`
- Focused Rigel tests after cleanup:
  - `rtk cmake --build out/harness-rigel --target test_paula test_floppy test_mfm_decode harness -j2`
  - `rtk ctest --test-dir out/harness-rigel/rigel-build -R 'test_(paula|floppy|mfm_decode)' --output-on-failure`
  - `test_paula` now asserts that disk DMA with `ADKCON.WORDSYNC` starts at `0x4489`, not at the `0xAAAA` gap.
- AROS megademo validation after WORDSYNC fix:
  - `aros.rom + megademoA.adf`: final loader read returns `err=0`, `actual=0x8400`.
  - `aros.rom + megademoA.adf --cpu 68020`: no later illegal-instruction crash observed in the checked window, but video remains grey/black.
- KS1.3 regression smoke:
  - `KS13.rom + megademoA.adf` still completes the checked headless run without software failure.

## KS1.3 / DPaintIV Comparison

- `src/disks/wb13.adf` is not a plain Workbench disk. `unadf` identifies it as OFS volume `DPaintIV`, filled at about 95.7%, with `dpaint`, `Preferences`, `Install DPaint`, `c/`, `devs/`, `libs/`, `s/`, etc.
- Rigel after the WORDSYNC fix:
  - `KS13.rom + megademoA.adf` still produces a multicolor frame by frame 1200.
  - `KS13.rom + wb13.adf` reaches only two early track-0 disk DMA reads in the observed window and then remains visually white/blank in the Rigel PPM dump.
  - The observed KS1.3 track-0 reads use `DSKLEN=0x9cbe` and do not set `ADKCON.WORDSYNC`, so `start_src=0` is expected in that path.
- Legacy comparison:
  - A separate legacy harness was configured and built with `BELLATRIX_USE_RIGEL_CHIPSET=OFF`.
  - `KS13.rom + megademoA.adf` reaches active copper/bitplane state, as expected.
  - `KS13.rom + wb13.adf` also advances past track 0 in legacy. It seeks through higher cylinders, reaches around cylinder 40+ in the checked run, installs copper lists, and displays active bitplanes.
- This means the current `KS13.rom + wb13.adf` failure is not explained by the ADF being named oddly or by a generic KS1.3 boot limitation. It is a real Rigel-vs-legacy behavioral divergence after the initial bootblock path.

## 2026-06-06 Follow-up: KS1.3 / DPaintIV Rigel Floppy Trace

- Added focused Rigel floppy tracing gated by `BELLATRIX_RIGEL_FLOPPY_TRACE=1`, plus internal drive traces gated by `RIGEL_FLOPPY_TRACE=1`.
- Found and fixed a real CIA floppy-line divergence in `external/rigel/src/core/rigel_cia_api.c`:
  - Rigel lowered CIA-A `/DSKRDY` when any drive motor was spinning even if no drive was selected.
  - Legacy and `rigel_sync_floppy_cia_lines()` keep `/RDY` high when no drive is selected.
  - After removing that deselected-drive ready path, the early KS1.3 `/RDY` poll at `PC=0xFE5A78` reads `PRA=0xfc`, matching legacy.
- After the `/RDY` fix, Rigel still fails to progress to the DPaintIV root block:
  - Rigel performs two track-0 DMAs and then remains around `PC=0x00fc0f94`.
  - Legacy performs the same early track-0 reads, then seeks to cylinder 40 and reads root block track 80 (`adf_offset=450560`).
- The track-0 DMA payload itself matches at the useful sync window:
  - At the ACK after the real track-0 DMA, both Rigel and legacy show `sync=44894489:552aaaa5` at the expected buffer offset.
  - This makes MFM encoding and chip RAM DMA writes unlikely to be the immediate cause of the KS1.3/DPaintIV divergence.
- The next proven divergence is `DMACON` state at the second disk completion:
  - Legacy second DSKBLK handler sees `DMACONR=0x22d0`.
  - Rigel second DSKBLK handler sees `DMACONR=0x2200`, then the existing legacy-priority restore sets only `DSKEN`, yielding `0x2210`.
  - The missing bits are `COPEN|BLTEN` (`0x00c0`), not disk data.
- Added diagnostic `RIGEL_DMA_TRACE=1` and `RIGEL_COPPER_DMACON_TRACE=1`.
  - This proved the `0x02d0 -> 0x0200` transition is a Copper write:
    - `[RIGEL-COPPER-DMACON] value=00ff cop_pc=000a4e h=28 v=13 frame=199`
    - `[RIGEL-DMA] write-dmacon raw=00ff old=02d0 new=0200`
  - CPU writes to `$dff096` before this point match legacy; the destructive write does not pass through `machine_custom_write()`.
- Tried two narrow Copper hypotheses:
  - Treating `WAIT $ffff,$fffe` as stopped until VBL did not affect this failure.
  - Honoring BFD/blitter-busy waits in the Rigel Copper domain also did not affect this specific `cop_pc=0x000a4e` write.
  - Both are plausible correctness improvements, but they are not sufficient for the KS1.3/DPaintIV blocker as observed.

## 2026-06-06 Follow-up: KS1.3 / DPaintIV Copper Halt Fix

- Added a focused Copper fetch trace with `RIGEL_COPPER_TRACE=1` and `RIGEL_COPPER_TRACE_FRAME_FROM=198`.
- The destructive `MOVE #$00ff,DMACON` path was:
  - Frame 199 VBL reload starts at `COP1LC=0x02368`.
  - The list waits at `0x023b0` with `WAIT $0c01,$fffe`.
  - It then executes `COPJMP2` at `0x023b4`, jumping to `COP2LC=0x00676`.
  - Memory at `0x00676` is not a valid Copper list for this context. It begins with a low-register MOVE (`ir1=0x0000`), then Rigel continued linearly through chip RAM until `0x000a4e`, where the random words decoded as `MOVE #$00ff,DMACON`.
- The old/legacy Copper model halts on illegal low-register MOVE when `COPCON.CDANG` does not allow it:
  - `!CDANG && addr < 0x80` halts.
  - `CDANG && addr < 0x40` halts.
- Rigel had the guard but only returned from `copper_exec_move()`, so the service advanced PC and set `fetch_pending` again. That turned an illegal MOVE into a skipped instruction instead of a Copper halt.
- Local fix in `external/rigel/src/chipset/agnus/copper/copper_exec.c` / `copper_service.c`:
  - Illegal low-register MOVE clears `waiting` and `fetch_pending`, then sets `stopped_until_vbl`.
  - The Copper service returns immediately after such a stop instead of advancing PC.
- Result:
  - The spurious `RIGEL-COPPER-DMACON value=00ff cop_pc=000a4e` is gone in the checked path.
  - `KS13.rom + wb13.adf` now advances past the old blocker and reads root block track 80:
    - `cyl=40 side=0`
    - `track=80`
    - `adf_offset=450560`
  - The run continues into real filesystem activity with many later reads across the disk. It still ends at `PC=0x00fc0f94` at 900 frames, so this is progress past the floppy/root-block blocker, not a complete visual boot verdict.
- Validation:
  - `rtk cmake --build out/harness-rigel --target harness test_paula test_floppy test_mfm_decode -j2`
  - `rtk ctest --test-dir out/harness-rigel/rigel-build -R 'test_(paula|floppy|mfm_decode)' --output-on-failure`
  - `aros.rom + megademoA.adf` with 5000 frames still performs WORDSYNC disk reads with `start_src=1660` and reaches late tracks up to `cyl=79 side=1`, so the Copper halt fix did not regress the already-fixed AROS floppy read path.

## Updated Next Steps For KS1.3 / DPaintIV

1. Keep `KS13.rom + wb13.adf` as the main regression, but move the success condition forward:
   - Old blocker is cleared when Rigel reads track 80 (`adf_offset=450560`).
   - Next success condition should be visual/user-level progress: DPaintIV/Workbench-like display state or a stable shell/app screen instead of a blank/grey frame.
2. Audit which current Copper changes are production fixes versus diagnostics:
   - Production: illegal MOVE must halt; `COPJMP1/2` must not get an unconditional post-MOVE `PC += 4`; end-of-list stop; BFD/blitter wait if retained.
   - Diagnostic-only: `RIGEL_COPPER_TRACE`, `RIGEL_COPPER_DMACON_TRACE`, broad copper write event ranges, and DMA trace should either stay env-gated or be removed before final cleanup.
3. Continue after the root-block read:
   - Trace `DoIO` request completion fields for later filesystem reads.
   - Track any failed reads (`io_Error != 0`) or suspicious `io_Actual`.
   - Compare display register state after DPaintIV/Workbench starts setting `BPLCON0`, `COP1LC`, `COP2LC`, bitplane pointers, and colors.
4. Do not mask the symptom by forcibly restoring `COPEN|BLTEN|DSKEN` after disk DMA. The proven bug was illegal Copper execution, and the correct class of fix is to stop invalid Copper streams.

## Next Steps

1. Keep the WORDSYNC behavioral fix in Rigel and the focused regression in `test_paula`.
2. For `KS13.rom + wb13.adf`, compare Rigel vs legacy at the first point where legacy starts seeking beyond track 0 but Rigel does not:
   - CIAB PRB drive select/motor/side/step/direction writes.
   - CIAA/CIAB timer state used by trackdisk polling and motor settle delays.
   - Paula disk IRQ/DSKSYNC/DSKBLK timing around the second bootblock read.
   - Trackdisk request fields in memory after each `DoIO`/interrupt completion.
3. Add a decoded floppy-control trace for Rigel, gated behind an env var, so PRB writes print selected drive, motor, side, step edge, direction, cylinder, `/TRK0`, `/DSKCHG`, `/WPRO`, and `/RDY`.
4. Continue AROS megademo video investigation with `--cpu 68020` after the KS1.3 floppy regression is isolated:
   - Trace writes to BPL pointers, BPL mods, `DDFSTRT/DDFSTOP`, `DIWSTRT/DIWSTOP`, and color registers after the payload reaches `0x12000`.
   - Dump and inspect the bitplane memory pointed to by BPL1 around frames 500, 1200, and 2200.
   - Compare against the known-good `KS13.rom + megademoA.adf` register and bitplane state.
5. Revisit `bootdisk-amiga-m68k.adf` only after the ADF-only megademo path is visually correct, because the bootdisk expects CD media for the complete AROS workflow.
