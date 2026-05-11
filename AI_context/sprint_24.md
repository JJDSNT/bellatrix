// AI_context/sprint_24.md

# Sprint 24 — Harness video-path deep dive: DMA relaxation, late-video latch, and missing bitmap producer

## Description

This sprint continued the KS13 harness investigation after the earlier memory
and boot-timing work had already shown that:

- Kickstart reaches the video path
- Copper runs
- bitplane pointers are programmed
- the missing boot screen is not explained by `2 MB` Chip RAM timing alone

The focus here was to answer a more specific question:

- why does the harness still fail to show the boot screen even after the ROM
  clearly arms a late video setup?

The work was split into two phases:

1. verify whether the line snapshot / DMA gating logic was suppressing valid
   video state too aggressively
2. if video fetch starts working, determine whether the fetched bitmap data is
   actually non-zero

## Main conclusion

Two different things were true at the same time:

1. the old bitplane-DMA gating model was indeed too strict for this KS13 boot
   path
2. fixing that was not enough, because the bitplane buffers being fetched are
   still zero

So the current top-level conclusion is:

- the harness already builds and arms the late video structures correctly
- the Copper list is being constructed and executed
- `BPL1PT/BPL2PT` are being programmed every frame
- but there is still no evidence that the bitmap payload behind those pointers
  is ever populated

That moves the investigation upstream:

- the missing screen is no longer best explained by Denise timing, line latch
  ordering, or pure `BPLEN` gating
- it is now best explained by a missing bitmap producer, or by some missing
  emulation behavior that should lead to those buffers being filled

## Detailed findings

### 1. KS13 really reaches the late video path

Repeatedly observed in the harness:

- `COPPER-JMP2 ... new_pc=10450`
- `BPLCON0` alternates around `0x2302 / 0x0302`
- `BPL1PT = 0x0a572`
- `BPL2PT = 0x0c4b2`
- `DMACON = 0x02d0`
- late-frame CPU state sits around:
  - `PC=0x00fc5a78`
  - `PC=0x00fc5a6c`

Those PCs were later confirmed by disassembly to be only a wait loop on
`DMACONR`, not the producer of the image itself:

- `00fc5a60: btst #$6,$dff002`
- `00fc5a68: bne $fc5a6c`
- `00fc5a6a: rts`
- `00fc5a70: btst #$6,$dff002`
- `00fc5a78: bne $fc5a6c`
- `00fc5a7a: rts`

So this late-state loop is a waiter/arbiter, not the renderer.

### 2. The original latch behavior was suppressing valid line state

At the start of this sprint, the late visible line already had:

- `raw_np=2`
- `bplcon0=2302`
- `bpl1=0a572`
- `bpl2=0c4b2`

But the line still collapsed to background-only because:

- `DMACON=02d0`
- `DMAEN=1`
- `BPLEN=0`
- `latched_np=0`

That showed the old model was too rigid:

- `bitplanes_begin_line()` treated `BPLEN=0` as an unconditional reason to
  zero the latched plane count

### 3. A controlled relaxation was added to the bitplane path

The experiment introduced a shared `bitplanes_dma_allowed(...)` decision used
by:

- line latch
- Agnus DMA query
- DMA scheduler request filtering

The relaxed condition allows bitplane fetch to continue when:

- DMA master is enabled
- `raw_nplanes > 0`
- the high plane-count bits in `BPLCON0` indicate active bitplanes
- a non-zero bitplane pointer is already present

Touched files:

- `src/chipset/agnus/bitplanes.h`
- `src/chipset/agnus/bitplanes.c`
- `src/chipset/agnus/agnus.c`
- `src/chipset/agnus/dma.h`
- `src/chipset/agnus/dma.c`

### 4. The scheduler had a second independent suppression point

An important mid-sprint discovery was that relaxing only the latch was not
enough.

Reason:

- `agnus_dma_query_requests_cb()` could now request bitplane DMA
- but `dma_filter_requests()` in `dma.c` still rejected `REQ_BITPLANE*`
  strictly on the old `BPLEN` rule

After both layers were aligned, the fetch path really started executing.

### 5. The fetch path then proved the next problem

Once the DMA scheduler was relaxed consistently, the harness showed:

- `v=56`
- `raw_np=2`
- `latched_np=2`
- `dma_ok=1`
- `dmacon=02d0`
- `bplen=0`

And actual fetch logs appeared:

- `BPL-DIAG-FETCH`
- `BPL-DIAG-DONE`

This was the key confirmation that:

- the remaining failure was no longer simply “Agnus never fetched the line”

### 6. But all fetched words were still zero

Even after fetch was really happening, the late visible lines still produced:

- `w0=0000`
- `w1=0000`
- `first0=0000`
- `last0=0000`
- `first1=0000`
- `last1=0000`

This was consistent across:

- `v=44..63`
- both bitplanes
- repeated frames

Typical pointer progression looked like:

- start near `0x0a572 / 0x0c4b2`
- then advance by line fetches to:
  - `0x0a752 / 0x0c692`
  - `0x0a77a / 0x0c6ba`
  - and further

But the words fetched from all of those addresses remained zero.

### 7. Early boot really did clear those regions

The harness write watch already showed the early memory clear loop zeroing the
eventual bitplane ranges, for example around:

- `0x0000a572..`
- `0x0000c4b2..`

That by itself was not surprising.

What mattered was the next observation:

- later, no non-zero writes were found repopulating those bitmap payload
  regions

### 8. The ROM path around `0xfe881a/20/44` only builds metadata

Disassembly of KS13 around `0x00fe8810` showed:

- `move.l ($24,A5), ($c,A0)`
- `movea.l ($10,A5), A1`
- `jsr (-$c6,A6)`
- `movea.l ($14,A5), A0`
- `movea.l ($28,A5), A1`
- `move.l #$1f40, D0`
- `jsr (-$1d4,A6)`
- `move.l ($1c,A5), ($4,A0)`
- `move.l ($14,A5), ($c,A0)`

This matches the observed writes:

- `a552 = 0x0000a572`
- `a556 = 0x0000c4b2`
- `a542 = 0x0000a54a`

Interpretation:

- this code is wiring pointers into a control/data structure
- it is not directly drawing pixels into the bitplane buffers

### 9. The helper around `0xfcc5c0` is building Copper list entries

The call target from the `fccbxx` region was disassembled and showed writes of
control words and addresses into a structure:

- write a word at `(A0)`
- write a word at `2(A0)`
- write the high part / argument at `4(A0)`
- advance by 6 bytes
- repeat

That is characteristic of Copper list entry construction, not bitmap fill.

### 10. The `fca484..fca568` helper also manipulates list/control state

This region computes values based on structure fields and emits words such as:

- packed line/control values
- `0xfffe`

That again looks like Copper `WAIT` / control construction rather than bitmap
blitting.

The live memory dumps around `a4c0..a560` reinforced that reading:

- this area is a control/list structure
- not a pixel buffer

### 11. No blitter activity has been observed so far

Instrumentation in Agnus was broadened so blitter register writes would be
logged much more reliably.

Result:

- no `AGNUS-BLT-W` entries were found in the late boot/video phase

That is important because it means one of these is true:

1. the expected blitter-driven fill/copy path never starts in the harness
2. the screen producer is not the blitter and must be some CPU copy path not
   yet traced
3. an earlier subsystem divergence prevents the ROM from ever reaching the
   actual bitmap population step

## Relaxation performed in this sprint

This sprint included an intentional experimental relaxation of the DMA model.

What was relaxed:

- bitplane line latch gating
- Agnus DMA request exposure for bitplane channels
- DMA scheduler filtering of bitplane requests

Why it was justified:

- the KS13 late video path clearly had:
  - valid `BPLCON0`
  - valid bitplane pointers
  - visible-line timing
  - DMA master enabled
- but the old `BPLEN` rule alone was forcing the line to background-only

What the relaxation proved:

- there really was a model mismatch in the old gating behavior
- but that mismatch is not sufficient to explain the missing image

So the relaxation was diagnostically useful and narrowed the problem
substantially.

## Validation performed

During the sprint:

- `cmake --build out/harness`
- repeated headless harness runs with KS13

Representative run:

- `./out/harness/harness src/roms/KS13.rom --headless --cycles 90000000`

Representative outcome:

- harness completed around `90,000,218` cycles
- final late-loop PC still around `0x00fc5a78`
- repeated `COPPER-JMP2 -> 0x10450`
- repeated `BPL1PT/BPL2PT = a572/c4b2`
- repeated fetches from those buffers still zero

No focused unit/integration test suite was run for this sprint because the
work was concentrated on harness-only instrumentation and behavior tracing.

## Files touched during this investigation

- `src/chipset/agnus/bitplanes.h`
- `src/chipset/agnus/bitplanes.c`
- `src/chipset/agnus/agnus.c`
- `src/chipset/agnus/dma.h`
- `src/chipset/agnus/dma.c`
- `tools/harness/musashi_backend.c`

## Current best model

The current best explanation for the missing boot screen in the harness is:

- the ROM builds the display control structures
- the Copper list is valid enough to arm the bitplanes
- Agnus/Denise can be made to fetch those bitplanes consistently
- but the memory the pointers reference is never populated with image data

So the next investigation should focus on:

1. who is supposed to populate `0x0a572` and `0x0c4b2`
2. whether that producer is:
   - a CPU copy path
   - a library/memory helper reached through Exec
   - a blitter path that never starts
3. whether some earlier missing condition prevents the ROM from reaching the
   actual bitmap-population routine

## Next step

The next concrete step after this sprint is:

- instrument and trace non-zero writes in the payload ranges behind
  `0xa572` and `0xc4b2`
- then walk backward from those callsites to identify the missing producer

