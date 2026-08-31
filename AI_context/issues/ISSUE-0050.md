---
id: ISSUE-0050
title: "What our VideoCore driver lacks against its two references"
status: open
priority: low
type: research
owner: unassigned
created_at: 2026-08-23
updated_at: 2026-08-23
tags:
  - graphics
  - vcgfx
  - upstream
blockers:
related_files:
  - aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_hvs.c
  - aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_dma.c
  - aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_onbitmap.c
  - external/aros/arch/arm-native/soc/broadcom/2708/hidd/vcgfx
---

# Summary

Our `vcgfx` is our own code (2026-08-21), derived from AROS's arm-native
driver. There are two references for the same hardware and they are not the
same kind of thing:

| | what it is | where |
|---|---|---|
| AROS `arm-native` vcgfx | the HIDD ours was derived from | `external/aros/arch/arm-native/soc/broadcom/2708/hidd/vcgfx` |
| `VideoCore.card` | a Picasso96 RTG card driver, m68k under Emu68 | https://github.com/michalsc/VideoCore.card |

`VideoCore.card` is the closer match to this machine -- m68k guest, big-endian,
peripherals through Emu68's `0xf2xxxxxx` window -- even though it plugs into P96
rather than into a HIDD, so it cannot be adopted, only read. A checkout exists
at `~/bellatrix-legacy/external/VideoCore.card` (it left the tree at the
2026-08-03 reset), at `e49193a` (2026-02-03).

This issue records the comparison so it does not have to be redone. It is a
survey, not a work queue: the standing decision of 2026-08-17 applies, and only
the items marked *in scope* are things that make what already exists faster or
better.

# Against AROS arm-native

17 commits touch that directory. 14 are HVS5/BCM2711 -- Pi 4 -- and produce
`vcgfx_hvs5.c` + `vcgfx_hvs5.h` (~37 KB) which we do not carry. We detect the
chip and say so:

```
[VC4HVS] BCM2711 - HVS5 not supported yet, staying on the firmware path
```

That is a capability gap on a target that is not ours. Of the rest:

- **`7b81016a77` -- "copy 128 bits at a time where there is no stride".**
  Missing, applies to us directly, pure performance. Where source and
  destination are contiguous (`src_pitch == dst_pitch == width_bytes`) and
  16-byte congruent, it splits the copy into head / aligned middle / tail and
  runs the middle with `DMA_TI_SRC_WIDTH | DMA_TI_DEST_WIDTH`. Ours always
  takes the 2D band path (`vcgfx_dma.c:211`). **In scope.**
- `14ef672d23` -- refuse DMA through the bus alias above 1GB. **We have it**,
  including the control-block check.

Checked and *not* missing: cursor-plane authoring for HVS4 (`hvs_cursor_entry`,
`vcgfx_hvs.c:349`); the fields absent from our `vcgfx_hvs.h` are HVS5's.

Where we deliberately diverge, and are right to:

- **Register access is byte-swapped.** Upstream reads
  `*(volatile ULONG *)(VC4_HVS_BASE + offset)` raw, which on a big-endian m68k
  returns garbage -- this is the `head=0` that was once blamed on the firmware.
  `VideoCore.card` uses explicit `LE32`/`wr32le` accessors, i.e. the same
  conclusion reached independently.
- **The vsync probe is timed, not spin-counted.** Upstream later reached the
  same answer for HVS5 (`31af2ef079`).

# Against VideoCore.card

Ahead of us:

- **Authored scaling with real filter kernels.** `mitchell_netravali()`
  (`src/vc4.c:23`), `compute_scaling_kernel()` (:161),
  `compute_nearest_neighbour_kernel()` (:198), written into display-list
  memory. We do the opposite on purpose: the fb plane entry is *inherited
  verbatim from the firmware* and "the scaling words stay opaque"
  (`vcgfx_hvs.c:467`), with a guard against ever writing over the PPF kernel
  because doing so "corrupts scaled scanout and wedges the HVS" (:508). So we
  can present at whatever the firmware set up and cannot choose a scaling
  policy of our own.
- **`SetPanning()` (`src/vc4.c:414`, ~330 lines)** -- recomputes scale, offsets
  and sprite geometry and repoints the display list. This is the mature version
  of the scanout repoint written by hand for the boot-presentation hold, and is
  the best available reference for validating that path. **Worth reading before
  the Pi 3 test of ISSUE-0049.**
- **Pixel clock control** -- `ResolvePixelClock`, `GetPixelClock`, `SetClock`.
  We ask the mailbox for a resolution and take what arrives.
- **`vc6.c` (~38 KB)** -- VideoCore VI. Same Pi 4 gap as above.
- **A buddy allocator for display-list memory** (`src/buddyalloc.c`), shared
  between scaling kernels, unicam and the normal display. Ours uses fixed
  slots (`HVS_OWN_SLOTS`) sized to stay clear of the kernel.
- `unicam` (camera) and `devicetree.resource` -- not our problem.

Not ahead of us:

- **Blitter.** `BIF_BLITTER` is commented out of its capability word
  (`src/main.c:532`); we have a working `vc4_dma_copy`.
- **Vsync.** Its `GetVBeamPos()` (:1046) polls `SCALER_DISPSTAT1`. Our HVS4
  path arms the vblank interrupt.
- **Cursor.** Both author a cursor plane.

# Worth doing, in order

1. Port `7b81016a77`, the 128-bit linear DMA copy -- **behind a flag, and
   measured on a Pi 3 before it becomes the default.**

   Safe by construction and plausible on the silicon: `DMA_TI_SRC_WIDTH`
   (bit 9) and `DMA_TI_DEST_WIDTH` (bit 5) are BCM2835 DMA features, unchanged
   on the BCM2837, and they are a full-engine capability -- we already hold a
   full engine, because `DMACHF_TDMODE` is documented as "Needs a full engine"
   and the resource hands out lite channels first otherwise
   (`bcm2708_dma.h:20-24`). The objection this tree already recorded --
   "needs the row length a multiple of 16; what the engine does with the
   trailing partial write otherwise is unverified" (`vcgfx_dma.c:145`) --
   stops existing: the head/aligned-middle/tail split means the wide burst
   only ever covers 16-byte-aligned, 16-byte-multiple memory and there is no
   partial wide write at all.

   What we do *not* have is evidence for this board:

   - The commit is dated 2026-08-21, between HVS5/BCM2711 commits, so it was
     almost certainly developed and measured on a **Pi 4**.
   - Its own message records that widening a *strided* transfer
     **black-screened the machine**. The safe subset was found empirically.
     This engine has a way to go wrong in exactly this area.
   - We already have one DMA operation that hangs on real hardware: the 2D
     *fill*, which is why the page clear is a CPU `memset`
     (`vcgfx_onbitmap.c:435`).
   - Nothing in `AI_context` records the DMA *copy* path being exercised on a
     Pi 3 at all -- neither working nor failing.

   So the risk is not the alignment maths, it is that this driver's DMA has
   never been characterised on our board. Opt-in, then measure a full-window
   scroll both ways.
2. Read `SetPanning()` before testing ISSUE-0049's flip path on a Pi 3. It
   solves the same problem on the same hardware and has been run in anger.
3. Leave scaling alone until something needs it. Authoring kernels means taking
   over a part of the list we currently inherit precisely because touching it
   wedges the HVS, and nothing today asks for a scaling policy of our own.
