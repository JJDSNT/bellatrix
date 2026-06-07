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

## 2026-06-06 Follow-up: trackdisk.device Cleared; Grey Screen Explained as Boot-Retry Loop

User hypothesis to check: `trackdisk.device` (not `ata.device`) might be the AROS driver responsible for floppy boot detection.

- Confirmed via `RIGEL_FLOPPY_TRACE=1`/`RIGEL_DISK_TRACE=1` traces: trackdisk-relevant emulation works correctly in Rigel.
  - DF0 media correctly detected (`media=1 cyl=0 side=0`); DF1-DF3 `media=0` is expected (empty drives during `readunitid_internal` scan).
  - Cylinder seeking, `/TRK0`, `/DSKCHG`, `/WPROT`, `/DSKRDY` all transition correctly; recalibration succeeds.
  - 100+ successful disk DMA reads (`irq=0002`, no `io_Error`) across the run.
  - **Verdict: trackdisk.device / floppy emulation is NOT the blocker** — ruled out.

Tooling bug found and explained the "frozen frame counter" sub-investigation:
- `BELLATRIX_RIGEL_DUMP_FRAME`/`BELLATRIX_RIGEL_DUMP_PPM` only take effect when `BELLATRIX_RIGEL_TRACE=1` is ALSO set — `machine_rigel_maybe_dump_frame()` is called from inside a block gated by `if (!g_rtrace.enabled || !g_rigel) return;` (`src/machine/machine_rigel.c:385`). Earlier dump attempts at `dump_frame >= 5` silently never fired because that env var was missing — not an emulator bug, not a frozen counter.

With dumping fixed (`BELLATRIX_RIGEL_TRACE=1` + `BELLATRIX_RIGEL_DUMP_FRAME=N` + `BELLATRIX_RIGEL_DUMP_PPM=path`), the real picture:

- **`aros.rom` with NO ADF inserted**: by frame ~1300 the screen is genuinely rendering rich content — 640x256, ~10 distinct colors (black/white/cyan/grey text-like UI), consistent with the user's observation "sem adf a tela de no media do aros aparece por volta do frame 1300". Serial log shows the boot sequence is mid-`dosboot.resource` init at frame 1500 (`leave InitCode(0x04,...)` never reached in this run).
- **`aros.rom + bootdisk-amiga-m68k.adf`**: boot proceeds noticeably FURTHER — completes `InitCode(0x04, 0)`, loads `icon.library`, `shell.resource`, `shellcommands.resource`, `workbook.resource`, `con-handler`, `ram-handler`, `nonvolatile.library`, `nvdisk.library`, `setpatch.library` (none of which appear in the no-ADF run within the same frame budget).
- But the with-ADF screen freezes BYTE-IDENTICALLY from frame ~1450 through at least frame 4000 (640x256, flat `0xaaaaaa` background plus 8 black pixels forming the same diagonal corner-decoration pattern noted previously). Mode transitions: 256x256 (frames <~1420, with changing/blinking content — 616, 0, 1198 black px across consecutive samples) → 640x256 (from ~1430, settles to the frozen 8-pixel state by ~1450).
- The freeze period correlates with CONTINUOUS, SUCCESSFUL (`irq=0002`, no `io_Error`), CYCLIC disk re-reads of the same cylinder set (40-45, 80-91 — root-block / filesystem-metadata area), repeating indefinitely (111 disk events counted through frame 3000, still cycling at the end). Final PC for both no-ADF and with-ADF runs converges on the same idle-loop address `0x00fe849a`.

**Conclusion**: the with-ADF "grey screen" is the visible symptom of AROS being stuck in the `dosboot_init.c` retry loop (`for(;;) { dosboot_BootStrap(); if (!screen) NoBootMediaScreen(); wait 3s + animate }`), endlessly re-validating/re-reading the floppy's filesystem structures without ever completing a bootable handoff — exactly the behavior expected of an AROS *emergency/rescue* boot floppy that requires a companion Live CD to finish its workflow (as already noted in this file's "Findings" section). This is **not a Rigel/chipset video bug** — Denise/Agnus/Copper set up one static screen early (`BPLCON0 0000->0200` at `cyc=2858187`, `DMACON 0000->0240` at `cyc=70340797`, both identical between scenarios) and nothing thereafter changes the display because the active task is spinning in the DOS retry loop, not driving Intuition redraws.

**Net effect on the original "grey screen" question**: `bootdisk-amiga-m68k.adf` alone is confirmed to be the wrong test vehicle for judging Rigel's video pipeline (consistent with the file's existing guidance to prefer the megademo/`KS13` paths and revisit the bootdisk only once ISO/CD support is functional — which the user has confirmed is not yet the case: "o suporte iso ainda nao esta funcional").

## 2026-06-06 CORRECTION: User Rejected "Boot-Retry-Loop / Needs Live CD" Conclusion — This IS a Real Bug

The conclusion in the section directly above is **WRONG** and is superseded by this section. The user firmly corrected it in four consecutive messages:

- "na verdade o disco do aros deve bootar mesmo sem o iso." — the bootdisk SHOULD boot standalone, no companion Live CD required.
- "ele faz request pelo iso, mas isso deveria ja aparecer em uma tela intuition que no nosso caso nunca chega a aparecer" — yes, AROS requests the CD, but that request should itself surface as a visible Intuition system-requester screen (the standard "Please insert volume..." dialog) — and that screen never appears in our emulation. **That non-appearance is itself the bug.**
- "o aros tem um replacement de workbench chamado workbook, é ele que deveria aparecer ao bootar pelo floppy" — AROS's Workbench-replacement, "Workbook", is what should visually appear on this floppy boot path.
- "mas a realidade é que ao sair do path no media eu nao tenho nenhum retorno visual do aros (apenas artefatos)" — past the no-media check there is **zero** visual feedback from AROS — only the frozen artifact pattern.

So: the cyclic disk re-reads and `dosboot` retry behavior are real, but they are *not* an acceptable terminal state — AROS should be painting Intuition screens (a volume-request dialog, then Workbook) over that retry loop, and Rigel is silently failing to present any of it. **This is a genuine Rigel rendering/Copper bug.** Investigation continued below toward the actual root cause.

## 2026-06-06 Continued: Root Cause Localized — COP2LC Zeroed Every Frame by AROS Double-Buffer Selector Reading Uninitialized Fields

Resumed with `RIGEL_COP_REG_TRACE` (new env-gated trace added to `external/rigel/.../copper_regs.c`, logs `cop1lc`/`cop2lc`/Copper `program_counter` before+after each COPxLCH/L/COPJMPx write) and an extended `[RIGEL-MMIO-W]` trace in `machine_rigel.c` covering `$080`-`$08a` (COP1LCH/L, COP2LCH/L, COPJMP1/2), which crucially also logs the real M68K CPU `pc` via `bellatrix_debug_cpu_pc()` (not the Copper's own PC).

Sequence observed every frame from frame ~1111 onward (`BELLATRIX_RIGEL_TRACE=1 ./out/harness-rigel/harness src/roms/aros.rom --adf src/disks/bootdisk-amiga-m68k.adf --frames 1120`, grep `RIGEL-MMIO-W.*reg=08`):

```
[RIGEL-MMIO-W] reg=080 before=0000 write=0006 size=2 pc=00fcadd6   COP1LCH=0x0006
[RIGEL-MMIO-W] reg=082 before=0000 write=5358 size=2 pc=00fcadd6   COP1LCL=0x5358 → COP1LC=0x065358 (valid list)
[RIGEL-MMIO-W] reg=084 before=0000 write=0006 size=2 pc=00fcadde   COP2LCH=0x0006
[RIGEL-MMIO-W] reg=086 before=0000 write=53fc size=2 pc=00fcadde   COP2LCL=0x53fc → COP2LC=0x06553fc (valid list, ~0xa4 bytes after the first)
[RIGEL-MMIO-W] reg=084 before=0006 write=0000 size=2 pc=00fc9d5a   COP2LCH zeroed!
[RIGEL-MMIO-W] reg=086 before=53fc write=0000 size=2 pc=00fc9d5a   COP2LCL zeroed!
[RIGEL-MMIO-W] reg=084 before=0000 write=0000 size=2 pc=00fc9d5a   ... repeats every ~70824 cycles (= once per PAL frame)
[RIGEL-MMIO-W] reg=086 before=0000 write=0000 size=2 pc=00fc9d5a
```

`pc=00fc9d5a` reported by `bellatrix_debug_cpu_pc()` is the M68K's PC *after* fetching an instruction's extension words — the actual instruction is `move.l D0,$dff084.l` at `00fc9d54` (6 bytes, 0x54+6=0x5a). That single 32-bit move writes both COP2LCH and COP2LCL together — it IS the zeroing write.

Disassembled the surrounding ROM code with a small fixed-up `romdis` invocation (the stock `tools/harness/romdis.c` hardcodes `g_rom_base=0xF80000` with file-offset 0, which is wrong for this 1 MB ROM — the standard `0xF80000` window actually maps to the file's *second* half per `musashi_backend_load_rom`; had to feed it a half-ROM file to get correct addresses). Found the routine at `0x00fc95ca`:

```
00fc95ca: link    A5, #-$1c
00fc95ce: movem.l D2-D7/A2-A4/A6, -(A7)
00fc95d2: movea.l A1, A3
00fc95d4: move.l  ($e0,A1), (-$4,A5)
00fc95da: move.w  $dff004.l, (-$e,A5)      ; saves VPOSR (LOF / long-short field bit) to a local
...
00fc95f8: movea.l (-$4,A5), A2
00fc95fc: move.l  ($32,A2), D0             ; D0 = copper-list-A pointer field
00fc9600: move.l  ($36,A2), D1             ; D1 = copper-list-B pointer field
00fc9604: cmp.l   D0, D1
00fc9606: beq     $fc9d54                   ; if A==B, skip LOF check, just write D0
00fc960a: tst.w   (-$e,A5)                  ; else: branch on saved LOF bit
00fc960e: blt     $fc9612
00fc9610: move.l  D1, D0                    ; pick the "other" field (short-field list)
00fc9612: move.l  D0, $dff084.l             ; COP2LC = selected pointer
...
00fc9d54: move.l  D0, $dff084.l             ; (shared tail) COP2LC = D0
```

This is AROS's per-VBlank Copper-list double-buffer / long-short-field selector (graphics.library view-port machinery). **Both source fields (`+0x32`/`+0x36` of the structure pointed to by `(A1)+0xe0`) are zero**, so `D0==D1==0`, the routine takes the "equal, just use D0" fast path, and writes `COP2LC = 0` — every single frame, unconditionally.

The chain to the visible freeze: the primary Copper list at `COP1LC=0x065358` (decoded earlier — sets `BPLCON0=0x0201`, `COLOR00=0x0aaa` matching the grey background, zeroes all sprite/audio pointers, `DDFSTRT=DDFSTOP=0` i.e. **no bitplane fetch window**) ends with `WAIT $0c03,$fffe` (wait v=12,h=2) then `MOVE COPJMP2,#0000` at `0x6653f8` — a strobe that jumps the Copper to whatever `COP2LC` currently holds. Since `COP2LC` was just zeroed (at beam `v=3`, *before* the list reaches its `COPJMP2` strobe at `v=12,h=4`), the jump lands at chip address `0`, where the Copper fetches an illegal low-register `MOVE $000,#0000` and sets `stopped_until_vbl=true` — but the entire cycle (CPU re-zeroes COP2LC, list re-jumps to 0, halt) re-arms identically every frame. **No bitplane DMA is ever enabled** (confirmed earlier: `DMACON=0x02f0` has `BPLEN` clear), which is why the screen is a flat, unchanging grey.

## 2026-06-06 Continued Further: `(A2)+0x32`/`+0x36` Identified as `GfxBase->LOFlist`/`SHFlist` — Real Bug Is "No Screen Ever Gets Attached"

Pulled the thread to its end by identifying the AROS source. `A2` = `GfxBase` (`struct GfxBase *`); `+0x32`/`+0x36` are the compiler-generated offsets of `LOFlist`/`SHFlist` (`struct cprlist *`, declared in `<graphics/graphics_intern.h>` extension fields of `GfxBase`). The routine at ROM `0x00fc95ca` is **`gfx_vblank()`** — the AmigaVideo HIDD's per-VBlank interrupt handler, source at `external/aros/arch/m68k-amiga/hidd/amigavideo/amigavideo_chipset.c:1606-1636`:

```c
static AROS_INTH1(gfx_vblank, struct amigavideo_staticdata*, csd)
{
    struct GfxBase *GfxBase = (APTR)csd->cs_GfxBase;
    volatile struct Custom *custom = (struct Custom*)0xdff000;
    BOOL lof = (custom->vposr & 0x8000) != 0;
    ...
    /* is any displayed screen interlaced? */
    if (GfxBase->LOFlist != GfxBase->SHFlist) {
        custom->cop2lc = (ULONG)(lof ? GfxBase->LOFlist : GfxBase->SHFlist);
    } else {
        custom->cop2lc = (ULONG)GfxBase->LOFlist;     /* <-- our path: both are NULL, writes 0 */
        if (!lof)
            custom->vposw = custom->vposr | 0x8000;
    }
    ...
```

This is a 1:1 match for the disassembly (`cmp.l D0,D1` / `beq` / `move.l D0,$dff084.l`). **`GfxBase->LOFlist`/`SHFlist` are the source of the zero** — confirmed by grepping the entire captured MMIO trace: across the whole run, `COP2LCH/L` are written exactly **once** with a valid value (`pc=00fcadde`, `cop2lc=0x06553fc`, during `initcustom()`/chipset init) and **16 times** with zero by `gfx_vblank` (once per frame, every frame, forever after).

Traced where `LOFlist`/`SHFlist` are normally assigned, in the same file:
- `initcustom()` (`amigavideo_chipset.c:1660+`, called once from `startup.c:34` at HIDD-driver init): builds `csd->copper1` (→ matches our decoded list at `0x065358`) and a tiny "wait-forever" tail buffer whose address becomes `csd->copper2_backup = c` (→ matches `0x06553fc`); then does `custom->cop1lc = csd->copper1; custom->cop2lc = csd->copper2_backup;` directly — **this is the single correct write we see at `pc=00fcadd6/00fcadde`**. It does *not* touch `GfxBase->LOFlist`/`SHFlist`.
- `resetmode()` (`amigavideo_chipset.c:197`): sets `GfxBase->LOFlist = GfxBase->SHFlist = csd->copper2_backup;` — but it is **only called from `amigavideo_compositorclass.c:428` and `amigavideo_bitmapclass.c:222`** (compositor/bitmap class method handlers — i.e., when Intuition actually creates/composites a screen bitmap).
- `gfx_vblank_attachbm()` (lines ~1291-1297, ~1405-1420): sets `GfxBase->LOFlist = bm->bmcl->CopLStart` / `SHFlist = bm->bmcl->CopSStart` to the **real screen's** compiled Copper-list addresses — also only reachable through the bitmap-attach/compositing path triggered by Intuition opening a screen.

## 2026-06-07 Correction After Slow RAM: ADF No Longer Has Zero COP2LC, But Still Renders a Blank Grey Screen

After the shared slow RAM implementation, the old "COP2LC is zeroed every frame" conclusion is no longer the active failure mode. The slow RAM mapping changes AROS memory placement enough that `GfxBase` and the vblank list fields now live in slow RAM and must be probed through CPU memory, not only chip RAM.

Current Rigel harness results with `aros.rom`:

- No ADF:
  - `GfxBase->LOFlist=0x0002babc`, `GfxBase->SHFlist=0x0002c294`.
  - The selected `SHF` copper list at `0x02c294` programs a visible 4-plane display:
    - `BPLCON0=c205`
    - `BPL1=0x003b08`, `BPL2=0x00db08`, `BPL3=0x017b08`, `BPL4=0x021b08`
    - `BPLxMOD=0x0050`, `DIW=2c81/2cc1`, `DDF=003c/00d0`
  - The captured frame shows the expected AROS no-media visual.
- `aros.adf` and `bootdisk-amiga-m68k.adf`:
  - `GfxBase->LOFlist=0x0000dabc`, `GfxBase->SHFlist=0x0000dabc`.
  - The selected copper list at `0x00dabc` is valid but intentionally much simpler:
    - `BPLCON0=a201` (2 planes)
    - `BPL1=0x003ab8`, `BPL2=0x008ab8`
    - `BPLxMOD=0`, `DIW=2c81/2cc1`, `DDF=003c/00d0`
    - Colors are mostly grey/black (`COLOR00=0x0aaa`, `COLOR01=0x0000`, later color registers are zero).
  - Frame dumps around 1500-1600 show a flat grey screen with only tiny black artifacts.
  - Active bitplane regions receive writes, but the later writes from AROS code around `PC=0x00ffddd2` clear the relevant window to zero. Rigel then correctly fetches zero bitplane words, so the visible result is grey.
- Disk activity is not absent:
  - With ADFs, repeated DSKBLK interrupt enable/ack traffic is present (`INTENA/INTREQ` bit `0x0002`), unlike the no-ADF path.
  - This means the current "no visual return" is not explained by a total floppy/IRQ absence.

Current interpretation:

- The slow RAM implementation fixed the earlier failure where AROS could not keep the vblank display-list fields populated.
- The remaining ADF visual issue has moved forward: AROS now selects a valid 2-plane grey screen and clears its bitplane buffers, while continuing disk/boot activity.
- This is still not the desired AROS behavior: a standalone `aros.adf`/`bootdisk-amiga-m68k.adf` should eventually show Workbook or an Intuition requester. The next investigation should compare why the AROS bitmap/compositor path chooses or clears this minimal screen instead of attaching a populated WorkBook/requester bitmap.

Useful probes used in this pass:

- `AROS_GFXBASE_TRACE=1`: captures `GfxBase`, `LOFlist`, and `SHFlist` from the ROM vblank routine at `pc=0xfc95fc/0xfc9600`.
- `AROS_COPLIST_DUMP=1`: decodes the selected AROS copper lists.
- `BELLATRIX_CHIP_WRITE_WATCH=0x003ab8:0x003c00` / `0x008ab8:0x008c00`: confirms writes to the ADF bitplane buffers and later zeroing.
- `BELLATRIX_RIGEL_TRACE=1 BELLATRIX_RIGEL_DUMP_FRAME=1599`: captures the grey ADF frame and visible no-ADF frame.

**Conclusion of this thread**: `GfxBase->LOFlist`/`SHFlist` are never anything but their zeroed-at-allocation initial value (`AllocMem`/`MakeLibrary` clears `GfxBase`) because **neither `resetmode()` nor `gfx_vblank_attachbm()` is ever invoked** — i.e., the AmigaVideo compositor/bitmap class methods that run when Intuition creates/attaches a screen bitmap *never fire* during this entire run (1120+ frames). `gfx_vblank` faithfully runs every frame and "selects" between two NULL pointers, strobing `COPJMP2` toward chip address 0 and halting the Copper — a textbook-correct response to the actual problem, which is upstream.

**This closes the Copper/Rigel-rendering angle**: there is no Copper bug, no missing-DMA bug, no list-decoding bug — Rigel is faithfully executing exactly what AROS told it to. **The real defect is that AROS never creates/opens/attaches any screen bitmap** — matching precisely what the user described ("no Workbook, no Intuition requester, only artifacts"). The investigation must now move from Rigel/graphics internals to **why Intuition (or whatever subsystem opens the boot-time screen / volume-insert requester) never reaches the point of creating a screen bitmap** — almost certainly a stalled task/missing-signal problem (an IRQ, a semaphore release, a DMA-completion flag, or similar Rigel-side condition AROS is blocked waiting on) somewhere in the boot chain *before* Intuition's first `OpenScreen()`/requester call. Likely next steps: trace Exec task switches / `Wait()`/`Signal()` activity around the point where the boot sequence should hand off to Intuition, or search `external/aros/rom/intuition/` for the screen-opening / system-requester code path and instrument its entry points to see whether it's ever reached at all.

**Diagnostic instrumentation note**: a small standalone `romdis` build was created at `/tmp/romdis` (compiled from `tools/harness/romdis.c` + `external/musashi/m68kdasm.c`) together with a half-ROM extract `/tmp/aros_std_half.rom` — needed because `tools/harness/romdis.c` hardcodes `g_rom_base=0xF80000`/file-offset 0, which is *wrong* for this 1 MB ROM (the `0xF80000` window actually maps to the file's second half per `musashi_backend_load_rom`). If ROM disassembly becomes a recurring need, `romdis.c` should be fixed to replicate the real ext/std split logic from `musashi_backend.c:174-184`.

**Diagnostic instrumentation added this session (still present, env-gated, harmless if unused — candidates for cleanup or permanent env-gated retention)**:
- `external/rigel/src/chipset/agnus/copper/copper_regs.c`: `RIGEL_COP_REG_TRACE` env-gated `[RIGEL-COP-REG]` trace on every COPCON/COP1LCx/COP2LCx/COPJMPx write (logs cop1lc/cop2lc/program_counter before+after, beam h/v/frame).
- `external/rigel/src/chipset/agnus/copper/copper_service.c`: temporarily widened `rigel_copper_trace_pc` filter from `< 0x03000` to `< 0x80000` (needed to see the fetch trace for the list at `0x065358`; should probably be reverted or made configurable via env var before any commit).
- `src/machine/machine_rigel.c` (`machine_custom_write`, `[RIGEL-MMIO-W]` trace condition): extended the register filter to include `0x080,0x082,0x084,0x086,0x088,0x08a` (COP1LCH/L, COP2LCH/L, COPJMP1/2) — this is what surfaced the `pc=00fcadd6/00fcadde/00fc9d5a` CPU-side evidence above and is generally useful for any future Copper-pointer investigation.

## 2026-06-06 SUPERSEDES PRIOR SECTION: Screen DOES Attach (~frame 1427); Real Bug Is Bitplane DMA Fetch Returning Zero Words From Chip RAM

**The "no screen ever gets attached" conclusion above (section starting "Continued Further...") is WRONG and is superseded by this section.** Built new harness-side instrumentation (`AROS_GFXBASE_TRACE`, `aros_gfxbase_lof_check` in `tools/harness/musashi_backend.c`, watching writes to `GfxBase->LOFlist`/`SHFlist` once `GfxBase` is captured from `A2`) — confirmed `GfxBase = 0x0256e0` (the *other* alternating value the trace captured was a stale-PC artifact: Musashi reports `PC` as already-advanced during EA-data reads, so the read at `pc=fc95fc` actually belongs to the *preceding* `movea.l (-4,A5),A2`, capturing `A2`'s pre-load garbage).

**A screen DOES attach, at frame ≈1427** (cyc≈101055045-101066634):
- `GfxBase->LOFlist`/`SHFlist` (`+0x32`/`+0x36`) get written with a real, valid Copper-list address `0x000D9EFC` (via `gfx_vblank_attachbm`, at `pc≈0xfce52e/0xfce54e`).
- `DMACON` gets `BPLEN` (bit 8) set for the first time at the same frame (`pc=00fce556`, `0x02f0→0x03f0`) and stays set through 1995+ frames.
- `COPJMP2` is strobed (`pc=0653f8`, reg `$08A`), jumping the Copper PC from the boot list (`COP1LC=0x065358`) to `COP2LC=0x000D9EFC`.

**Decoded the full list at `0x000D9EFC`** (by widening `rigel_copper_trace_pc`'s filter from `< 0x80000` to `< 0x200000` — the old `0x80000` cutoff was *silently hiding all fetches from this list*, since `0x0d9efc & 0x1ffffe = 0xd9efc > 0x80000`). It is a completely valid, real Workbench-style screen program:
- Palette: `COLOR00=$0AAA` (grey bg), `COLOR01=$0000`, `COLOR02=$0FFF`, `COLOR03=$068B` (blue), plus more entries up to `COLOR15`.
- `DIWSTRT=$2C81`, `DIWSTOP=$2CC1` (covers the full visible 640×256 area — matches our observed frame dimensions).
- `BPLCON0=$A201` → HIRES, 2 bitplanes (depth=2).
- `DDFSTRT=$003C`, `DDFSTOP=$00D0`, `BPL1MOD=BPL2MOD=0`.
- `BPL1PT = 0x000C2B38`, `BPL2PT = 0x000C7B38`.
- Ends by writing **`COP1LC = 0x000D9EFC`** (its own address — the standard "rewrite COP1LC" trick) then `WAIT $FFFF,$FFFE` forever. This is why it self-sustains via the normal VBL auto-reload from frame 1429 onward without any further `COPJMP2` — confirmed: frame 1429 starts directly at `pc=0d9efc`.

**So the Copper/AROS chain is entirely correct and Rigel executes it faithfully.** The screen attaches, the palette and bitplane geometry are all valid, DMA is enabled. And yet frame dumps at 1450/1600/1900 remain flat grey (`#aaaaaa`, ≤8 stray black pixels that vanish by 1900) — i.e., **palette index 0 everywhere**.

**Root cause finally pinned down one layer deeper, inside Rigel itself — NOT an AROS bug:**
- Added a one-shot diagnostic (`AROS_BPL_DUMP`/`AROS_BPL_DUMP_AFTER` env vars, `aros_bpl_dump_check` in `tools/harness/musashi_backend.c`) that hexdumps chip RAM directly at `BPL1PT=0x000C2B38`/`BPL2PT=0x000C7B38` once the screen has been attached for N frames. Result: **both buffers are filled, uniformly across a full 20480-byte (80 B/line × 256 lines) hires bitplane page, with byte `0x84` repeating** — which is *exactly* `tools/harness/main.c:739`'s startup fill pattern (`memset(s_chip_ram, 0x84, sizeof(s_chip_ram))`, only the first 256 KiB then overwritten with `0xFF`). **Nothing has ever written to these addresses** — the buffer is pristine/untouched since emulator boot. (This by itself would be a legitimate further question — "why did AROS never draw into its own bitmap" — except that...)
- The existing `compose` event trace in `external/rigel/src/chipset/denise/render/compositor.c:compose_line()` shows, for every scanline of the active screen: `depth=2`, `plane_word_count=40`, **`nonzero=0x00000000`**, `plane_words[0][0]=0`, `plane_words[1][0]=0`. I.e. Denise's compositor receives **all-zero** bitplane words — consistent with the flat-grey output (palette index 0 = `COLOR00` everywhere).
- The existing `bpl_fetch` event trace in `external/rigel/src/chipset/agnus/timing/slot_scheduler.c` (case `AGNUS_SLOT_BITPLANE`) shows the fetch slot *is* running, with `depth=2`, plausible `addr` values in the right region (e.g. `addr=0x000c457a`, near `BPL1PT`), — **but `agnus->fetch.data[plane]` (the actual word `mem.read16()` returned) is `0`.**

**This means `bitplane_fetch_step()` (`external/rigel/src/chipset/agnus/bitplanes/bitplane_fetch.c`) — specifically its `mem.read16(mem.opaque, ptrs->bplpt[plane])` call / the `rigel_chip_ram_if_t` wiring for bitplane DMA — returns `0` even when the addressed chip-RAM location genuinely contains non-zero data** (independently confirmed via direct buffer dump through the same `bellatrix_chip_read8`/`harness_chip_read` path the CPU and Copper use). This is a **Rigel bitplane-DMA chip-RAM-read bug**, isolated to the `AGNUS_SLOT_BITPLANE` fetch path — not a Copper bug, not an AROS bug, not a "no screen attaches" bug. The long mystery of "COP2LC stuck at zero / grey screen" is fully explained as a downstream symptom of AROS correctly waiting for/building a screen that, once built, simply never receives renderable bitplane data from chip RAM due to this fetch-path defect.

**Concrete next steps** (in `external/rigel`, NOT `external/aros`):
1. Instrument/inspect `ptrs->bplpt[plane]` immediately before vs. after the `mem.read16()` call inside `bitplane_fetch_step` — verify whether the address actually used for the read matches the logged `addr` (the `bpl_fetch` trace logs `agnus->bplpt.bplpt[plane]` *after* `bitplane_fetch_step` already ran and called `bplpt_advance`, so the logged `addr` could be off-by-one-fetch from the address truly used).
2. Check `rigel_context_chip_ram(ctx)` / the `rigel_chip_ram_if_t mem` struct passed into `bitplane_fetch_step` — verify `mem.read16`/`mem.opaque` are correctly wired to the host's chip-RAM buffer in the harness/machine integration for this specific call site (vs., say, a stale/zeroed/different backing store than the one `custom_regs`/Copper/CPU paths use).
3. Cross-check `bplpt_set_hi`/`bplpt_set_lo` (`external/rigel/src/chipset/agnus/bitplanes/bitplane_pointers.c`) — confirm `BPL1PTH/L`/`BPL2PTH/L` MOVE writes from the Copper list (`pc=0d9f7c..0d9f88`, values `$000C/$2B38` and `$000C/$7B38`) actually land in `agnus->bplpt.bplpt[0]`/`[1]` with the expected values, and aren't being reset/zeroed by something between the Copper write and the first fetch slot (e.g. a per-line "reload from BPLxPT" step that reads stale/zeroed shadow registers instead of the live ones).

**New diagnostic instrumentation added this session** (env-gated, harmless if unused, in `tools/harness/musashi_backend.c`):
- `AROS_GFXBASE_TRACE=1` → `aros_gfxbase_lof_check`: captures real `GfxBase` (filtering the dual-value PC-stale artifact) and logs every write to `GfxBase->LOFlist`/`SHFlist`.
- `AROS_BPL_DUMP=1` (+ optional `AROS_BPL_DUMP_AFTER=<n>`, default 60) → `aros_bpl_dump_check`: one-shot hexdump + non-zero-byte census of the `BPL1PT`/`BPL2PT` chip-RAM buffers, N `gfx_vblank` invocations after the screen attaches.
- `external/rigel/src/chipset/agnus/copper/copper_service.c`: `rigel_copper_trace_pc` filter widened from `< 0x80000` to `< 0x200000` — **important**: the old narrower filter silently hid all Copper activity at `COP2LC=0x0d9efc` and would mislead any future investigation into thinking the Copper "never jumps" there. Recommend keeping the wider bound (or making it env-configurable) permanently.

## 2026-06-07 Checkpoint: Harness Slow RAM Gives First Visual Progress With `aros.rom + megademoA.adf`

Current local, uncommitted state after the harness-side slow RAM change:

- User-observed result: `megademoA.adf` now shows visual output when booted with `aros.rom`. This is the first useful visual return for that AROS-ROM + ADF path.
- Still failing visually: `aros.adf` and `bootdisk-amiga-m68k.adf` still provide no useful visual return in the same broad test context.
- Functional change responsible for the progress is now in the shared memory map: a 1.5 MB slow RAM window at `0xC00000-0xD7FFFF`, enabled by default in the harness and disabled with `BELLATRIX_SLOW_RAM=0`.
- The slow RAM check must happen before `bellatrix_bridge_normalize_addr()`, because the current normalizer maps `0xCxFyyy` to the custom-chip mirror `0xDFFyyy`; that would corrupt/alias valid slow RAM addresses such as `0xC1FA68`.
- Cleanup applied after review: slow RAM read/write now lives in `src/machine/memory/slow_ram.c`, goes through the shared `BellatrixMemory` map, and has end-of-window bounds checks.
- Review caveat: Emu68 now has the source in its CMake list and its bus normalization is protected when slow RAM is configured, but no `0xC00000` backing was enabled for the real target because the existing Emu68 MMU path maps that range read-only/fault-driven. The validated path is the harness/shared-memory route.
- Build verification: `rtk cmake --build out/harness` passes.
- Test verification: `rtk ctest --test-dir out/harness --output-on-failure` is not green in the current tree. Passing tests: `bellatrix_unit_memory`, `bellatrix_unit_uart`, `bellatrix_unit_cia`. Failing tests: `bellatrix_integration_overlay`, `bellatrix_harness_smoke`, `bellatrix_harness_boot_adf`, `bellatrix_harness_no_autoconfig`. The visible failure excerpt includes `FAIL line=522 aud0 current sample decoded from auddat expected=32512 actual=0`, which does not look caused by the slow RAM change but should be treated as residual risk for this checkpoint.

Recommended next split before committing:

1. Commit only the functional slow RAM support plus a small bounds/trace cleanup.
2. Keep AROS/Rigel diagnostics either in a separate diagnostic commit or behind clean env-gated hooks.
3. Defer `aros.adf`/`bootdisk-amiga-m68k.adf` visual debugging until the `megademoA.adf` visual path is preserved as a known-good regression target.
