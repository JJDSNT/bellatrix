# Rigel Gap Analysis After Submodule Update

Last updated: 2026-05-27

Compared sources:

- `migracao.md`
- `temp/rigel_changes_20260527_060543/rigel_local_changes.patch`
- `temp/rigel_changes_20260527_060543/files/`
- `external/rigel` at `7cb7d33c1060b27939694f241161569623149731`
- `external/rigel/AI_context/current_state.md`
- `external/rigel/docs/pending.md`
- `external/rigel/AI_context/ks31_support.md`
- `external/rigel/AI_context/ecs_support.md`

## Summary

The new Rigel state absorbed most of the previous visual/chipset gaps listed in
`migracao.md` and in the local `temp` patch. The remaining high-impact work is
no longer mainly "port legacy chipset into Rigel"; it is now split between:

1. harness integration required to boot real ROMs under Musashi;
2. full ECS/advanced video behaviour;
3. still-incomplete library features such as indexed output and save-state;
4. validation of subtle timing/arbitration details.

## Resolved By The Rigel Update

These items were listed as missing in `migracao.md`, the old
`docs/rigel_gap_analysis.md`, or the backup patch, but are now present in
`external/rigel`.

| Area | Current status |
|------|----------------|
| Copper VBL reload | Implemented. `current_state.md` and commit `3d17052` record auto-reload from `COP1LC` at `vpos=0,hpos=1`; `e319c34` adds the domain hook. |
| VPOSR/VHPOSR live reads | Implemented. `agnus_read.c` returns LOF, chip ID, VPOS high bits, and VHPOSR lores hpos. |
| LOF/LOL beam fields | Implemented by commit `3bb045e`. |
| BEAMCON0 PAL/NTSC | Implemented minimally. Current Rigel accepts PAL/NTSC via `BEAMCON0`; full programmable ECS timing remains open. |
| DDFSTRT/DDFSTOP scheduler wiring | Implemented. Writes update the slot scheduler and bitplane fetch range. |
| BPL1MOD/BPL2MOD | Implemented and applied at the end of each active non-VBL display line. |
| DIWSTRT/DIWSTOP cross-domain wiring | Implemented. Denise owns the registers and propagates to Agnus raster/scheduler. |
| OCS DIWSTOP vstop extension rule | Implemented by commit `c6609a8`. |
| DIWHIGH latch/extended ECS window | Basic ECS `DIWHIGH` support exists. Full ECS display semantics remain open. |
| Refresh DMA stub | No longer empty; `refresh_dma_step` is called at refresh slots. Fine arbitration validation remains open. |
| Blitter LINE mode stub | No longer a stub. LINE mode is active and dispatched per slot; line-state cleanup landed in `603336c`. |
| DMACONR BBUSY/BZERO | Implemented in `agnus_read.c` and covered by blitter DMA tests. |
| Copper MOVE/WAIT/SKIP and wait masks | Implemented; MOVE/SKIP/WAIT use the timing-aware copper path. |
| Denise priority/HAM/sprite tests | Added. Current state records tests for priority, HAM6/HAM8, dual playfield, sprites, and a sprite priority fix. |
| Dead TODO/stub files from old report | Several were removed: old pixel pipeline, scanline, Agnus DMA/beam forwarding stubs, and bitplanes display-window stub. |

## Still Open In Rigel

### 1. Full ECS support

Rigel has an ECS feature gate, Agnus/Denise IDs, minimal `BEAMCON0`,
`DIWHIGH`, readable `BPLCON3`, and a 1 MiB ECS chip-RAM window. It still lacks:

- programmable beam timing beyond `BEAMCON0` bit 5 (`HTOTAL`, `VTOTAL`,
  sync/blanking/hcenter, LOF/interlace interactions, external sync);
- SuperHires/Productivity modes;
- 2 MiB ECS Agnus variants;
- real `BPLCON3` display semantics such as sprite resolution and bank/compare
  effects.

Impact: medium for OCS/KS1.3, higher for KS3.1/ECS software and later
Workbench modes.

### 2. Harness gaps for real KS3.1 boot

`external/rigel/AI_context/ks31_support.md` says the chipset side is broadly
ready for KS3.1, but the Musashi harness is not ROM-boot ready. Critical gaps:

- CIA read/write mapping in Musashi memory callbacks;
- ROM mirror at `$FC0000`;
- chip RAM overlay controlled by CIA-A PRA bit 0;
- `harness_load_rom_file()`;
- CIA-B keyboard ACK via `SPMODE` still needs verification.

Secondary harness work:

- bus stall/chip RAM arbitration;
- optional slow RAM at `$C00000`.

Impact: critical for booting real Kickstart through the harness, but not a core
Rigel chipset regression.

### 3. Indexed 8-bit pixel output

`RIGEL_PIXEL_INDEXED_8BIT` remains unimplemented. Current output supports
`RGBA8888` and `RGB565`. Indexed output needs Denise to retain post-priority,
pre-palette chunky indices.

Impact: API/host feature gap, not a boot blocker.

### 4. Snapshot/save-state is still incomplete

`rigel_snapshot_t` captures only `cycles`, `intreq`, and `intena`. Copper,
blitter, audio, disk, beam, CIA, Denise output, and other state are not captured.

Impact: save-state/debugging limitation.

### 5. Chip bus mask by configured revision

`src/chipset/agnus/bus/chip_bus.c` still has a TODO to apply the address mask
based on chip revision. ECS has a 1 MiB visible window, but bus masking needs
continued validation across configured memory sizes and chip revisions.

Impact: visible only with software probing mirroring/address wrap behaviour.

### 6. Timing validation and arbitration fidelity

The slot scheduler is active, DDF and refresh are wired, and bus ownership is
reported from the scheduler. What remains is validation against hardware-level
captures:

- exact slot positions;
- refresh accounting side effects;
- CPU stall timing under DMA contention;
- blitter-nasty edge cases.

Impact: low to medium for boot, higher for cycle-sensitive demos/games.

### 7. DF1..DF3 disk selection path

Public floppy APIs expose `DF0..DF3`, but `current_state.md` states only `DF0`
is wired into Paula disk DMA. The other drives are exposed to the host but not
selected by chipset control paths yet.

Impact: non-boot DF0 use cases and multi-drive software.

### 8. Debug/optimization TODOs

Remaining lower-priority TODOs include:

- debug overlay rendering;
- detailed per-sprite debug dumps;
- SIMD-friendly planar output replacement;
- precise Denise video timing cleanup.

Impact: mostly tooling/performance/correctness polish.

## Temp Patch Relevance

The old local patch does not apply cleanly to the updated Rigel because upstream
rewrote or implemented the same areas. Its main themes are now mostly covered:

| Temp patch theme | Current relevance |
|------------------|-------------------|
| Video standard config and BEAMCON0 | Covered upstream, but full programmable ECS beam remains open. |
| LOF/LOL and VPOSR/VHPOSR | Covered upstream. |
| BPL1MOD/BPL2MOD | Covered upstream. |
| Blitter LINE mode | Covered upstream, with newer line-state structure. |
| Copper VBL reload | Covered upstream. |
| DDF absolute positioning / Denise line output | Mostly covered by scheduler + compositor changes; still validate edge timing. |
| DIWSTOP wrap/extension | OCS extension and ECS `DIWHIGH` exist; full ECS/video-mode semantics remain open. |
| Dirty-line/frame metadata tweaks | Partly covered by frame double buffering and dirty tracking; indexed/pre-palette output remains open. |

The only part worth revisiting manually is not direct reapplication; it is using
the old patch as test inspiration for edge cases around DDF/DIW positioning,
blitter line crossings, dirty-line exactness, and VPOSR chip ID/LOF behaviour.

## Updated Priority

Recommended order from the current state:

1. Fix Musashi harness ROM boot gaps: CIA mapping, `$FC0000` mirror, overlay,
   ROM-file loading.
2. Verify CIA-B keyboard ACK (`SPMODE`) and DSKBYTR/ADKCON disk polling against
   Kickstart behaviour.
3. Add focused regression tests derived from the old temp patch for DDF/DIW,
   blitter line edge cases, dirty-line exactness, and VPOSR/VHPOSR.
4. Validate scheduler slot positions, refresh, and CPU stall behaviour under
   DMA contention.
5. Expand ECS: programmable beam timing, SuperHires/Productivity, `BPLCON3`,
   and 2 MiB Agnus variants.
6. Implement indexed 8-bit output and expand snapshot/save-state when public API
   stability matters.

## KS1.3 Harness Notes

Observed with the deadline-limited harness quantum added on 2026-05-27:

- Four recent runs (`deadline_quantum*.log` and
  `deadline_quantum_nocopy*.log`) all reached the critical CPU write
  `DMACON 02d0->03d0` around PC `$00fe8622/$00fe862c`.
- This is a material improvement over the earlier fixed-quantum runs, where
  some executions stayed at `DMACON=02d0` after the Workbench display window was
  established and therefore never produced bitplane fetch/composition activity.
- The first visible bitplane fetches are now consistent across normal and
  no-copy output: frame ~623/625, `v=44`, `h=56`, `DMACON=03d0`, `depth=2`,
  `DDF=56-208`, `DIW=0581/40c1`, and `BPL1/BPL2` around
  `$012ba2/$014ae2`.
- `BPLCON0` alternates between `$0302` and `$2302` during the Workbench raster
  sequence. Frame-level summaries may sample `$0302` and report `depth=0`, but
  the active fetch/composition lines see `$2302` and `depth=2`.
- Remaining variation between runs is mostly how long the log captured
  post-enable composition activity, not a divergent pre-display chipset state.

The next useful correctness step is CPU/chipset arbitration: the harness now
uses Rigel deadlines to choose smaller CPU quanta, but it still does not model
actual CPU stalls on chip RAM/custom access during DMA ownership windows.
