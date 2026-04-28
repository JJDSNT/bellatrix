// AI_context/sprint_17.md

# Sprint 17 — Denise raster-time rendering fix

## Date
2026-04-28

## Goal
Fix the timing integration between Agnus/bitplanes and Denise: lines with
active bitplanes were being lost because rendering was deferred to a VBL
frame-batch loop that sampled `bplcon0` after the Copper had already disabled
bitplanes.

---

## Context

After sprint 16, AROS was booting and the Copper list was executing correctly
(confirmed by `[COPPER-MOVE]` logs showing BPL1PTH/BPL1PTL/BPL2PTL and
BPLCON0=0x2302 for 2 bitplanes). However no graphical output appeared.

Diagnosis (captured in `temp2.md` and `temp3.md`):

```
[BPL-LINE-BEGIN] ... nplanes=2   ← Agnus captures correct state
[DENISE-ENTRY]  ... ready=0 nplanes=0   ← Denise sees nplanes=0
```

Root cause: the VBL block ran a per-line loop that gave the Copper a budget of
128 steps per line. With a short Copper list the Copper could write
`BPLCON0=0x2302` (enable) and immediately `BPLCON0=0x0302` (disable) within
the same iteration before `bitplanes_begin_line` captured the state. So every
line latched `nplanes=0` and rendered only background.

Architectural mismatch confirmed: Bellatrix was modelling the Amiga as a
frame-based system (run-all-copper-then-render) but the Amiga is a raster-time
system (Copper, Agnus, and Denise are interleaved cycle by cycle).

---

## Commit delivered

| Hash | Summary |
|------|---------|
| `a7e13a2` | agnus/denise: move rendering from VBL frame-batch to raster-time bitplanes_step |

---

## Key changes

### `bitplanes.c` — `bitplanes_begin_line`

When `nplanes=0`, immediately sets `line_ready=1` and returns before
`bitplanes_snapshot_line_ptrs`. Leaves `active=1` so that subsequent
`agnus_step` calls at the same vpos do not re-enter `begin_line` and
generate duplicate renders.

Before this fix: nplanes=0 lines left `line_ready=0` indefinitely → the
background rendering path in Denise was never reached via the new per-step
path.

### `denise.h` / `denise.c` — `denise_render_line` signature

Old: `void denise_render_line(Denise *d, const BitplaneState *bp, int line_idx, int vheight)`

New: `void denise_render_line(Denise *d, const AgnusState *agnus, const BitplaneState *bp)`

`vstart`, `vstop`, `vheight`, and `line_idx` are now computed internally from
`agnus->diwstrt`, `agnus->diwstop`, and `bp->line_vpos`. Lines outside the
display window are silently discarded with an early return, making the function
safe to call from `agnus_step` at any beam position.

Diagnostic log updated to include `bp_line_vpos` and `agnus_v` as suggested
by the `temp3.md` analysis.

### `agnus.c` — `agnus_step`

Added after `copper_step`:

```c
bitplanes_step(&s->bitplanes, s);
if (bitplanes_line_ready(&s->bitplanes))
{
    if (s->denise)
        denise_render_line(s->denise, s, &s->bitplanes);
    bitplanes_clear_line_ready(&s->bitplanes);
}
```

Rendering is now triggered at raster time: as the beam advances naturally via
`beam_step`, the Copper fires its WAITs at correct positions, bitplanes_step
latches per-line state immediately after each Copper action, and Denise renders
as soon as the line is fetched.

### `agnus.c` — VBL block

The per-line rendering loop (128-step-per-line Copper, artificial
`beam.vpos` manipulation, `bitplanes_begin_frame`, `bitplanes_fetch_line`,
`denise_render_line`) was removed.

VBL block now only:
1. Logs VBL entry, vectors, and pre-reload state.
2. Fires `PAULA_INT_VERTB`.
3. Reloads Copper via `copper_vbl_reload`.
4. Calls `PAL_Video_Flip`.

---

## Build status

- Emu68 (AArch64 bellatrix): green

---

## Next steps

1. Flash and capture btrace to confirm `[BPL-LINE-BEGIN] nplanes=2` now has
   matching `[DENISE-ENTRY] bp_nplanes=2` log lines.
2. If pixels appear but are corrupt: check `bp->ddf_words` computation vs
   actual DDF register values in the Copper list.
3. If no pixels: verify `bitplanes_step` is being reached — add a one-time log
   when `line_ready` fires for a line with `nplanes>0`.
4. Wire `irq_line_level` into CIA-A ICR propagation (carried from sprint 16).
5. VPOSW / VHPOSW write handlers (carried from sprint 16).
