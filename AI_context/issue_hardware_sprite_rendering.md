// AI_context/issue_hardware_sprite_rendering.md

# Issue: Amiga hardware sprites (mouse pointer, player ship) never appear on screen

## Status: fixed enough to commit for WB1.3 pointer — broader sprite cases still need regression checks

## Update 2026-06-23: post-commit summary

Committed fixes:

- Rigel submodule commit `4847e26`:
  `denise: fix hardware sprite pointer rendering`
- Bellatrix repo commit `b69ed3c`:
  `machine: stabilize Amiga pointer rendering`

Final confirmed problem chain:

1. Sprite 0 pointer routing was incomplete. `SPR0PTH`-`SPR7PTL` writes were not
   owned by Agnus MMIO, so sprite DMA did not reliably receive the base pointer.
2. Sprite DMA treated idle lines before `VSTART` as real fetches. This advanced
   the current sprite pointer through chip RAM before the beam reached the
   sprite's visible line.
3. Sprite DMA did not preserve a separate base pointer for frame reload. Once
   the current pointer reached the terminator or walked through data, the next
   frame did not restart from `SPRxPT`.
4. The sprite-list terminator `0000/0000` was not treated as a terminator. The
   channel could continue scanning later chip RAM and encounter plausible
   sprite-looking data in the same frame.
5. Sprite pixels were clipped by the DIW/playfield horizontal window. This is
   wrong for Amiga hardware sprites: DIW constrains bitplanes, while sprites
   may be visible in border/overscan.
6. Host frame export initially exposed either too little or too much horizontal
   range: too little clipped the pointer; too much showed a large blank band at
   the left of Workbench. The committed viewport is monitor-like: keep a small
   border before the DIW instead of exporting the whole internal raster.
7. The two-Amiga-pointer symptom was not framebuffer residue and not the host
   cursor. Direct trace showed consecutive frames alternating old/new sprite
   positions. Root cause: host mouse deltas updated `JOY0DAT` immediately,
   potentially mid-frame, while AmigaOS/Workbench samples mouse movement on
   VBL and rewrites sprite data from that stable frame cadence. Latching host
   movement and applying it only once per frame made sprite `hstart` monotonic
   in the synthetic test instead of alternating.
8. A separate compositor bug made the first sprite line use a different
   horizontal scale when `BPLCON0.HIRES` changed around the line. Sprite overlay
   now uses a sprite-specific scale derived from active video geometry, not the
   instantaneous bitplane scale for that line.

Important validation:

- Focused tests passed after the final changes:
  `ctest --test-dir out/harness -R 'test_(denise|sprites|priority|ham|dualpf)' --output-on-failure`.
- Synthetic mouse movement before the final input latch produced alternating
  sprite positions, e.g. `hstart=124/new/124/new`.
- After `bellatrix_machine_mouse_motion()` was changed to accumulate deltas
  and `machine_mouse_frame_tick()` became the only place that applies them to
  `JOY0DAT`, the trace became monotonic:
  `hstart=124,125,127,128,130,131,133...`.
- After separating `sprite_hscale`, frame 826 showed the first and following
  sprite rows aligned at the same base X (`y=42 hstart=133 dst_px=266...`,
  `y=43 hstart=133 dst_px=266...`), instead of the first row being drawn at
  half-scale.

Remaining caveats:

- User-visible result is clearly improved but not perfect yet. There are still
  pointer control/viewport details to tune.
- Battle Squadron / broader hardware sprite cases still need a fresh regression
  pass with `RIGEL_SPRITE_DMA_TRACE=1`.
- The SDL harness now hides the host cursor by default and leaves relative
  mouse/grab/capture opt-in (`BELLATRIX_SDL_RELATIVE_MOUSE=1`). This is a UI
  debugging choice, not the root cause of the two Amiga pointers.

## Update 2026-06-23: WB1.3 pointer root cause

The Workbench 1.3 pointer problem was not only the DIW crop. The full chain
had three separate failures:

1. Sprite pixels were initially cropped by the DIW/playfield horizontal window.
2. Exporting the full internal `x=0..visible_x_stop` range made WB1.3 appear
   with a huge empty left band because its DIW is `vis=252..892/5..261`.
3. Most importantly, sprite DMA had only a current pointer. After sprite 0
   reached the terminator, the internal pointer kept walking through chip RAM
   and never returned to the `SPR0PTH/PTL` base on the next frame. The pointer
   therefore drew once early in boot and vanished by the stable Workbench frame.
4. After adding per-frame reload, sprite DMA still kept scanning chip RAM after
   the `0000/0000` sprite-list terminator inside the same frame. On live output
   this could look like two Amiga pointers because the channel could encounter
   another plausible control/data sequence later in the field.

Final fix:

- `sprite_dma_channel_t` now keeps `base_ptr` separately from current `ptr`.
- writes to `SPRxPTH/PTL` update `base_ptr` and reset `ptr` to that base;
- `sprite_dma_frame_start()` restores every sprite channel to `base_ptr` at
  VERTB and clears `armed/fetch_ctrl/vstart/vstop/w0`;
- `sprite_dma_slot()` treats a fetched control pair of `0000/0000` as a
  terminator: the channel becomes idle until the next VBL reload or explicit
  `SPRxPT` write, and the zero control pair is not delivered to Denise as an
  active full-frame sprite;
- frame export uses a monitor-like horizontal viewport: if the DIW start is
  large, export begins 32 pixels before `visible_x_start`, preserving a small
  border for sprites without exposing the whole blanking range.
- The SDL harness hides the host cursor by default but does not grab/capture
  the mouse by default. It installs a transparent SDL cursor because screenshots
  showed only one pointer while the live display showed two, which is consistent
  with the extra visible pointer being the host OS cursor (not captured in the
  framebuffer screenshot). Relative mouse mode, window grab, and capture are
  opt-in via `BELLATRIX_SDL_RELATIVE_MOUSE=1`; the default remains ungrabbed
  because grab/capture made pointer control worse on the test desktop. Set
  `BELLATRIX_SDL_SHOW_HOST_CURSOR=1` to restore the normal host cursor for
  comparison/debugging.

Validation after the final fix:

- `BELLATRIX_RIGEL_TRACE=1 BELLATRIX_RIGEL_DUMP_FRAME=865 ...` writes
  `/tmp/wb13_pointer_viewport_865.ppm` at `672x256`, not the earlier `892x256`.
- The same log reports `vis=252..892/5..261`, so the exported frame starts at
  `x=220` and keeps 32 pixels of left border.
- `RIGEL_SPRITE_DMA_TRACE=1` now shows repeated late-frame sprite 0 reloads,
  e.g. `vpos=0 ptr=000c84 ... vstart=42 vstop=58`, followed by
  `SPRITE0-WRITE` lines near `y=42..52`.
- After the terminator fix, the stable WB1.3 trace has no sprite 0 DMA scans at
  `vpos=59+` after frame 850; the channel stops at the terminator and resumes
  only at the next `vpos=0` reload.
- The PPM contains the hardware pointer pixels near the left edge of the
  exported Workbench image.
- Focused tests passed:
  `ctest --test-dir out/harness -R 'test_(denise|sprites|priority|ham|dualpf)'
  --output-on-failure`.

## Description

Original report: in the Musashi harness, clicking on a Workbench 1.3 icon
landed at the wrong position relative to where the visible mouse cursor
appeared to be. Investigation showed the visible "cursor" the user was
tracking was the *host* SDL cursor, not the Amiga's own hardware-sprite
pointer (sprite 0) — and that the Amiga's sprite pointer was not rendering
at all. The same symptom reproduces with Battle Squadron: enemy ships
(bitplane/blitter objects) render correctly, but the player's own ship
(believed to be hardware sprite 0, the same mechanism as the WB1.3 pointer)
never appears.

## Two confirmed bugs, both fixed

### 1. `SPR0PTH`-`SPR7PTL` never routed to Agnus

`external/rigel/src/chipset/agnus/mmio/agnus_mmio.c`,
`rigel_agnus_mmio_has_reg()` listed `BPL1PTH`-`BPL6PTL` (0x0E0-0x0F6) but
*not* `SPR0PTH`-`SPR7PTL` (0x120-0x13E). Writes to those addresses (from the
CPU or the copper, both go through the same `custom_regs_write16()` ->
`rigel_custom_domain_for_reg()` dispatch) fell through to
`RIGEL_DOMAIN_UNKNOWN` and were stored as a raw register value only —
`sprite_dma_set_ptr_hi/lo()` was never called, so every sprite's DMA fetch
pointer stayed at its reset value forever.

Fix: added the `AGNUS_SPR0PTH`-`AGNUS_SPR7PTL` range check, mirroring the
existing `BPL1PTH`-`BPL6PTL` one. Confirmed by trace: before the fix, every
sprite (0-7) read back identical garbage/zero control words every frame;
after the fix, sprite 0's pointer diverges from the others
(`ptr=0x0478` shared "blank" list for unused sprites vs `ptr=0x0c80`+ for
sprite 0 specifically) and real, varying `DATA`/`DATB` words get fetched.

### 2. Sprite DMA pointer advanced during the idle wait for VSTART

`external/rigel/src/chipset/agnus/dma/sprite_dma.c`, `sprite_dma_slot()`.
On real hardware, a sprite's DMA channel is idle (no fetch, pointer does not
move) on every line between the initial control-word fetch and `VSTART`.
The Rigel implementation fetched and advanced the pointer on *every* line
unconditionally, regardless of `armed`/`vstart`/`vstop` — so by the time the
beam actually reached `VSTART`, the pointer had already raced far past the
sprite's real image data, and `DATA`/`DATB` read unrelated chip RAM.

Fix: added an `idle = ch->armed && vpos < ch->vstart` gate; both the A-slot
and B-slot calls return early (no read, no pointer advance) while idle.
Confirmed by trace (`RIGEL_SPRITE_DMA_TRACE=1`, see below): sprite 0 now
fetches a coherent, incrementing sequence of non-zero `DATA`/`DATB` pairs
within `[vstart, vstop)`, and the compositor (`compositor.c`, sprite overlay
block) computes non-zero `pix` values (1/2/3) at multiple distinct screen
coordinates across a live Battle Squadron session — i.e. the pixel data
pipeline is now demonstrably correct.

## Third confirmed bug: output viewport used the DIW left edge

Workbench 1.3 (`KS13.rom + wb13.adf`) exposed a separate compositor/output
bug. Trace at the sprite write point showed the pointer pixels had
`dst_px=25..35`, while the decoded display-window left edge was
`x_start=129`; every pixel reported `cropped=1 drawn=0`.

The important conclusion is that sprites must not be clipped by the DIW
horizontal window. DIW clips bitplanes/playfields; hardware sprites can be
visible in the border. The old compositor and frame export path were using
`denise->video.visible_x_start/visible_x_stop` as both the playfield DIW and
the host-visible frame window, so anything in the left border disappeared.

Fix direction implemented:

- `external/rigel/src/chipset/denise/render/compositor.c`
  - initialize the full internal scanline to `COLOR00`, not only the DIW
    span, so border pixels have deterministic background state;
  - remove the sprite overlay's `x_start/x_stop` crop and only stop at the
    internal scanline buffer limit.
- `external/rigel/src/core/rigel_denise_api.c`
  - `rigel_get_frame()`/`rigel_get_scanline()` now expose a monitor-like
    horizontal viewport: for large DIW starts, the frame begins 32 pixels
    before `visible_x_start` and runs through the DIW right edge. This keeps
    the left-border sprite area without exporting the whole blanking range.
- `external/rigel/src/chipset/denise/output/framebuffer.c`
  - zero-copy host target copy uses the same viewport rule, matching the
    non-zero-copy frame API.
- `src/host/pal.h`, `src/host/posix/pal_posix.c`,
  `src/machine/machine_rigel.c`, `src/machine/machine_rigel_step.c`
  - the harness now avoids Rigel zero-copy presentation and supports dynamic
    SDL video resize. Zero-copy writes directly into the SDL framebuffer,
    which was normally 640 pixels wide; once Rigel exports the full WB1.3
    border-inclusive raster (`892x256`), direct copy necessarily clips the
    right side. The normal presenter path now calls `PAL_Video_Resize()` when
    `rigel_get_frame()` changes size, recreating the SDL texture/window so
    resolution changes are shown at native size instead of being cropped.

Validation:

- `rtk cmake --build out/harness -j2` passed.
- `BELLATRIX_RIGEL_TRACE=1 BELLATRIX_RIGEL_DUMP_FRAME=700
  BELLATRIX_RIGEL_DUMP_PPM=/tmp/wb13_viewport_fix.ppm
  ./out/harness/harness src/roms/KS13.rom --adf src/disks/wb13.adf
  --headless --frames 720` produced `/tmp/wb13_viewport_fix.ppm` at
  `892x256`; the image shows the WB1.3 window inside a preserved left border.
- `RIGEL_SPRITE_DMA_TRACE=1 ... --frames 460` now shows:
  `SPRITE0-WRITE y=251 dst_px=25 x_start=129 x_stop=449 cropped=0 ... drawn=1`
  and no `SPRITE0-WRITE.*cropped=1` hits in the checked run.
- After the presenter fix, another frame-700 dump still reports `892x256`,
  confirming the internal frame remains complete while the interactive harness
  should no longer clip it to the 640-wide SDL target.
- A later screenshot still showed the Workbench text itself clipped. That was
  caused by an over-aggressive compositor experiment that also clipped
  bitplane writes to `[x_start,x_stop)`. Removing that bitplane crop restores
  the full title/text while keeping sprite writes uncropped by DIW. Frame 865
  dump `/tmp/wb13_nobplcrop_865.png` shows complete `AmigaDOS`/Workbench text.
- Focused Rigel Denise tests passed:
  `ctest --test-dir out/harness -R 'test_(denise|sprites|priority|ham|dualpf)'
  --output-on-failure`.

## Still open: broader sprite/playfield priority regression

```c
if (pf_color[dst_px] == 0 || pair + pf_prio[dst_px] < 4u) {
    output->scanline_rgba[dst_px] = color;
    output->scanline_rgb565[dst_px] = rgb32_to_rgb565(color);
}
```

For the WB1.3 pointer, the write-side trace now confirms `drawn=1`, so the
old priority-suppression hypothesis is not the blocker for that case.
Battle Squadron/player-ship still needs a fresh run with the same write-side
trace to verify whether its remaining invisibility, if still present, is
priority, attach state, palette mapping, or another viewport assumption.

## Diagnostic trace added (safe on bare metal)

Two trace points exist, gated by `RIGEL_ENABLE_STDLIB_ENV` (compiles out
entirely without a real stdio/getenv backend — same pattern as
`copper_exec.c`) and by `RIGEL_SPRITE_DMA_TRACE=1`:

- `external/rigel/src/chipset/agnus/dma/sprite_dma.c`: `[SPRITE0-DMA]` —
  vpos, ptr, ctrl/data flag, w0/w1, armed, vstart, vstop, for sprite 0 only.
- `external/rigel/src/chipset/denise/render/compositor.c`: `[SPRITE0-PIXEL]`
  — y, amiga_x, scan_px, pix, for sprite 0 only, whenever `pix != 0`.
- `external/rigel/src/chipset/denise/render/compositor.c`: `[SPRITE0-WRITE]`
  — y, `dst_px`, DIW `x_start/x_stop`, crop state, PF priority state, and
  final `drawn` result at the write-side check.

Run with `RIGEL_SPRITE_DMA_TRACE=1` and grep for `SPRITE0-DMA` /
`SPRITE0-PIXEL` / `SPRITE0-WRITE`. These are intentionally left in place
(harmless, opt-in, zero-cost when the env var is unset) for the next session.

## Next steps

1. Re-test Battle Squadron/player-ship with `RIGEL_SPRITE_DMA_TRACE=1` and
   inspect `[SPRITE0-WRITE]` for `drawn=0` versus `drawn=1`.
2. If priority is not the cause, check `denise->sprites.attached_mask` /
   `denise_sprite_is_attached()` — an incorrectly-set attach bit on sprite 0
   or 1 could route sprite 0 through the "attached pair" path
   (`denise_sprite_attached_pixel()`) instead of the normal path, with
   different (possibly wrong) palette indexing.
3. Double check `DMACON` `SPREN` is actually set during the frames where the
   trace shows activity (the trace currently doesn't print DMACON state).
4. Decide whether Rigel needs first-class separate fields for display viewport
   versus DIW/playfield window. The current fix keeps `visible_x_start` as DIW
   metadata but derives a viewport in the frame export path; a later API
   cleanup should make this separation explicit.
5. See `issue_qemu_mouse_input.md` for the separate, not-yet-investigated
   QEMU mouse input regression noticed during this session.

## Files touched this session

- `external/rigel/src/chipset/agnus/mmio/agnus_mmio.c` (real fix, no trace)
- `external/rigel/src/chipset/agnus/dma/sprite_dma.c` (real fix + trace)
- `external/rigel/src/chipset/denise/render/compositor.c` (trace only;
  also has an unrelated reverted experiment — see below)
- `src/machine/machine_rigel.c`, `machine_rigel_step.c`,
  `machine_rigel_internal.h` (per-VBL mouse motion rate limiter, unrelated
  to sprite rendering but found during the same session — see
  `issue_qemu_mouse_input.md`)

## Dead end, for the record

A `+2*pipeline_lead` offset was briefly added to the sprite's `scan_px` in
`compositor.c`, hypothesizing that sprites needed the same shift-register
pre-load compensation bitplanes get for `DDFSTRT`. This was wrong and was
reverted: `HSTART` is already a display-beam position (like `DIWSTRT`), not
a fetch-trigger position (like `DDFSTRT`), so no compensation is needed.
The user confirmed empirically (clicking next to the WB1.3 icon) that this
change made alignment worse, not better.
