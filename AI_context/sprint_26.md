// AI_context/sprint_26.md

# Sprint 26 — VBL callback chain traced to 0xfe891c; LOF spin identified as next blocker

## Description

This sprint pushed the investigation boundary past disk completion and past the
display-block setup stage documented in sprint 25. The immediate focus was
answering: _after_ the display structure is installed by the 0xfe8768..0xfe88ff
range, does the machine reach a bitplane-payload producer?

The answer is no — but this sprint identified exactly why, and exactly where.

## Central discovery: the 0x1892 self-install callback chain

The KS1.3 display-setup code uses a self-install pattern to split its work
across multiple VBL frames. Each step in the chain:

1. writes its own PC to chip RAM address `0x1892` as its very first instruction
2. does some subset of the display-structure initialisation
3. either writes the next callback's address to `0x1892` or re-installs itself
   to be called again next VBL

The VBL dispatcher at `0xfc1826` reads the longword at `0x1890` and dispatches
through `0x1892`. So the chain is driven by successive VBL interrupts.

### The complete 14-step chain

| Step | PC at 0x1892 | Role (from BOOT-DISPLAY-SETUP log) |
|------|-------------|--------------------------------------|
| 1    | `0xfe8772`  | Entry — installs self, starts struct alloc |
| 2    | `0xfe87f2`  | Clears/inits sub-block A |
| 3    | `0xfe87fa`  | Clears/inits sub-block B |
| 4    | `0xfe8810`  | Sets small struct fields |
| 5    | `0xfe8828`  | Sets display-struct fields |
| 6    | `0xfe883a`  | Wires Copper/display context |
| 7    | `0xfe8882`  | Activates display context |
| 8    | `0xfe8888`  | Programs basic video registers |
| 9    | `0xfe888e`  | UI/layout init |
| 10   | `0xfe889c`  | Layout pass 2 |
| 11   | `0xfe88ac`  | Layout pass 3 / Copper list commit |
| 12   | `0xfe88d2`  | Bitplane pointer commit |
| 13   | `0xfe88e0`  | Final struct wiring |
| 14   | `0xfe891c`  | **Terminal node — self-installs and does not advance** |

Step 14 (`0xfe891c`) self-installs and then apparently waits for an external
condition before it will write the next callback address. After 300 emulated
frames the chain never advances past step 14.

### Why the chain was hard to observe before

The VBL ISR at `0xfc1826` writes a word (`0x002c`) to `0x1892` as part of
its frame-counter bookkeeping _before_ it dispatches through `0x1892`. This
overwrites any previous state at that address.

The callback then immediately self-installs by writing its own full longword
PC to `0x1892` — overwriting the VBL's word. So the net state visible to the
next read is always the self-install, not the VBL's counter word.

This made the chain invisible to simple read-probe instrumentation. The
1892-WATCH write tracker (see below) was the tool that finally revealed the
full sequence.

## Instrumentation added this sprint

### 1. 1892-WATCH — write tracker for 0x1892..0x1895

Added inside `harness_write()` in the chip RAM path:

```c
uint32_t wend = addr + (uint32_t)size - 1u;
if (wend >= 0x1892u && addr <= 0x1895u) {
    uint32_t b92 = bellatrix_chip_read32(mem, 0x1892u);
    printf("[1892-WATCH] pc=%08x write addr=%06x size=%d val=%08x → [1892]=%08x\n",
           (unsigned)pc, (unsigned)addr, size, (unsigned)value, (unsigned)b92);
}
```

This emits every write that touches any byte in `0x1892..0x1895`, including
partial overlaps. It was the key tool for identifying which instruction was
responsible for each state of `0x1892`.

### 2. BOOT-DISPLAY-SETUP probe range extended to 0xfe8960

Both `harness_update_boot_display_block()` and `harness_probe_display_setup()`
had their upper PC limit raised from `0xfe88ff` to `0xfe8960` so that steps
10–14 of the chain appear in output.

### 3. VBL-DISP-NODE probe (less useful, retained for history)

Added a probe at PC `0xfc182a` that dumps chip RAM around A0 when A0 is in
range `0x1880..0x1900`. This fired less reliably than the 1892-WATCH and has
lower priority.

## Display structure fully initialized by step 10

The BOOT-DISPLAY-SETUP output at `0xfe8910..0xfe8918` showed a fully
populated display structure:

```
A5=0x0018b6
A5+08 = display struct  (small)
A5+0c = sub-block       (small)
A5+10 = sub-struct
A5+14 = sub-struct
A5+18 = sub-struct
A5+1c = display struct  (valid pointer)
A5+20 = associated area (valid pointer)
A5+24 = 0x0000a572      ← BPL1 payload buffer (0x1f40 bytes)
A5+28 = 0x0000c4b2      ← BPL2 payload buffer (0x1f40 bytes)
```

The bitplane base addresses `0xa572` and `0xc4b2` observed in earlier sprints
are confirmed to be the dynamically allocated large buffers at `A5+24` and
`A5+28`. They are correctly wired into the display structure, but their content
remains zero.

## Key observed state at the end of step 14

After `0xfe891c` self-installs and the next VBL fires:

```
dmacon  = 02d0   (DMA on, bitplane DMA enabled)
bplcon0 = 2302   (2 bitplanes, color mode active)
bpl1    = 0a892  (hardware register, set by copper list mid-frame)
bpl2    = 0c7d2  (hardware register, set by copper list mid-frame)
cop1    = 02368
cop2    = 10450
p0      = 00000000
p1      = 00000000
```

Note: `bpl1=0x0a892` vs `A5+24=0xa572`. These differ by `0x320`. This is
consistent with the Copper list programming a sub-row bitplane offset to
center the image — the copper list is doing geometry work correctly, but the
underlying buffer at `0xa572` is still zero.

`BPL-DIAG-FETCH` entries show `w0=0000 w1=0000 w2=0000` at every bitplane
fetch position — confirming no pixel data has been written to the payload area.

## Root cause hypothesis: LOF polling loop

After step 14 installs itself, execution falls into a loop at `0xfc5a6c`:

```
0xfc5a6c: btst #$6, $dff002.l   ; test LOF bit of VHPOSR
0xfc5a72: bne  $fc5a6c           ; loop while LOF=1 (long frame)
```

This is a GFX-BUILD synchronisation barrier that waits for a **short frame**
(LOF=0) before committing the final display list and writing the bitmap payload.

In the harness this loop spins for all 300 frames observed, which means either:

- `VHPOSR` bit 6 (LOF) is always 1 (Agnus never reports a short frame), **or**
- `VHPOSR` bit 6 is always 0 but the `btst` / branch logic inverts the sense,
  and execution is stuck because the short-frame condition is never false

The most likely explanation is an Agnus beam emulation issue: the LOF bit
should toggle every frame (in non-interlace mode it stays 0 permanently; in
interlace mode it alternates). KS1.3 waits for LOF=0 specifically.

If Agnus always reports LOF=1, this loop never exits, the bitmap producer
never runs, and `0xa572` / `0xc4b2` remain zero forever.

## Recommended next steps

### 1. Fix VHPOSR LOF bit in Agnus

Read `src/chipset/agnus/agnus.c` (or wherever `VHPOSR` / `0xDFF006` is
implemented) and verify that:

- `VHPOSR` bit 6 is the LOF field
- In non-interlace mode, LOF should be **0** (PAL/NTSC short-frame always)
- In interlace mode, LOF toggles each frame

If LOF is hardcoded to 1 or miscalculated, set it to 0 and re-run. The GFX-BUILD
loop at `0xfc5a6c` should then exit on the first frame and the bitmap producer
should run.

### 2. Consider AROS ROM as a parallel debug path

The user suggested using the AROS ROM (`src/disks/aros.adf` / AROS ROM in
`external/`) as an alternative to KS1.3. Advantages:

- AROS source is in `external/` — the equivalent of the KS1.3 code at
  `0xfe891c` / `0xfc5a6c` can be read directly
- If AROS also hangs at a LOF poll, it confirms the LOF bug
- If AROS produces a screen without fixing LOF, it implies a different code
  path that avoids the synchronisation barrier

This is a good parallel investigation once the LOF hypothesis is confirmed or
refuted.

### 3. Focused log filter for next session

```bash
./run.sh harness | grep -E '1892-WATCH|BOOT-DISPLAY-SETUP|BPL-DIAG|VHPOSR|LOF'
```

## Files touched in this sprint

- `tools/harness/musashi_backend.c`
  - `harness_update_boot_display_block()`: upper PC limit raised to `0xfe8960`
  - `harness_probe_display_setup()`: upper PC limit raised to `0xfe8960`
  - `harness_probe_vbl_dispatch_node()`: new function (probe at `0xfc182a`)
  - `harness_write()`: 1892-WATCH tracker added in chip RAM write path
  - `harness_instr_hook()`: call to `harness_probe_vbl_dispatch_node()` added

## Validation

- `cmake --build out/harness --target harness` — build passed
- Harness run with `FRAMES=300` produced the 14-step callback chain in output
- 1892-WATCH correctly interleaved with BOOT-DISPLAY-SETUP entries

## Current best model

The display-setup callback chain is complete and working correctly. The bitplane
infrastructure is fully wired: structure, copper list, hardware registers. The
only missing piece is the bitmap payload.

The payload producer is gated behind a LOF synchronization barrier at
`0xfc5a6c`. The barrier spins because Agnus reports the wrong LOF state.

**Fixing VHPOSR bit 6 (LOF) in Agnus is the highest-confidence next action.**
