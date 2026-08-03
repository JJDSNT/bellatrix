# Emu68

Emu68 is the M68K→AArch64 JIT this project builds on. It lives at
`external/emu68` as a git submodule tracking upstream
[michalsc/Emu68](https://github.com/michalsc/Emu68), pinned at **`9b4379a`**.

**The submodule is never edited in place.** Every change is a patch in
`patches/emu68/`, applied on top of the pinned commit. This keeps the delta against
upstream visible and reviewable at all times, and makes rebasing onto a newer
Emu68 a matter of re-applying a small, named series.

## Why any changes are needed

Emu68 was built to run on a PiStorm: a real Amiga is on the other side of the
bus, and the accelerator reaches it through `ps_protocol`. Two consequences run
through the source as `#ifdef PISTORM_ANY_MODEL`:

- **Every Amiga address faults.** On PiStorm the whole Amiga address space is
  unmapped, so each guest access traps into `SYSReadValFromAddr()` /
  `SYSWriteValToAddr()` and is forwarded to the bus. Standalone, that space is
  plain RAM — nothing faults, so nothing can be intercepted.
- **A real chipset answers.** Paula owns the interrupt registers and the
  expansion bus answers autoconfig. Standalone, nobody does.

The result is that a guest with no Amiga hardware cannot arm an interrupt or
discover an expansion board, even though Emu68 already carries the machinery
for both. The patch series closes exactly that gap — nothing in it is specific
to this project, and all of it is a candidate for upstreaming.

## Change inventory

Four files. Line numbers are as of pin `9b4379a` with the full series applied,
and are given for orientation only — the anchors are the named regions.

### `src/aarch64/vectors.c`

The file is split into a PiStorm implementation (`#ifdef PISTORM_ANY_MODEL`,
L344) and a standalone one (`#else`, L736–L932). Both provide
`SYSWriteValToAddr()` and `SYSReadValFromAddr()`.

**Shared region, L311–L343** — moved *above* the `#ifdef`, out of the PiStorm
branch, so both variants see them:

| Moved out | Was | Now |
|---|---|---|
| `enum { CIAAPRA, CIABPRB, VPOSR, INTENA, INTENAR, INTREQ, INTREQR }` | PiStorm only | shared |
| `#include <boards.h>`, `int board_idx`, `struct ExpansionBoard **board` | PiStorm only | shared |

Added in the same region: `AUTOCONFIG_BASE` (`0xE80000`) and `AUTOCONFIG_END`
(`0xE8FFFF`).

This is pure code motion — no behaviour changes for a PiStorm build.

**Standalone `SYSWriteValToAddr()`, L738** — three new intercepts, all placed
before the fall-through `switch(size)` that performs the plain memory access:

- **L756, `INTENA`** — set/clear bits of `INT_shadow.INTENA` per the Amiga
  convention (bit 15 = set vs. clear). When `INT_shadow.ARMPending` is live,
  re-evaluates `ctx->INTF.ARM`, which is what the core-0 IRQ fast path in
  `__stub_vectors()` gates on.
- **L771, `INTREQ`** — same set/clear convention over `INT_shadow.INTREQ`.
  Clearing EXTER (`(value & 0xa000) == 0x2000`) also drops `ctx->INTF.ARM` and
  `INT_shadow.ARMPending`, which is how the guest acknowledges a host interrupt.
- **L802, autoconfig write** — `AUTOCONFIG_BASE..AUTOCONFIG_END`. Writing the
  base address at `0xe80044` (Z3) or `0xe80048` (Z2) maps the current board and
  advances `board_idx`; writing "shut up" (`0xe8004c`/`0xe8004e`) skips it.

**Standalone `SYSReadValFromAddr()`, L850** — three matching intercepts:

- **L862, `INTENAR`** — served from `INT_shadow.INTENA`; handles word and both
  byte halves.
- **L873, `INTREQR`** — served from `INT_shadow.INTREQ`, **plus bit 13 (EXTER)
  forced on whenever `INT_shadow.ARMPending`**. This is the load-bearing line
  of the series: it lets an unmodified Amiga level-6 handler conclude that
  EXTER fired and run its interrupt server chain, with no Emu68-aware code in
  the guest.
- **L895, autoconfig read** — byte reads return the current board's autoconfig
  ROM, skipping boards whose `enabled` is clear.

### `src/aarch64/start.c`

- **L22** — `#include "boards.h"`.
- **L1376–L1409**, a new `#ifndef PISTORM_ANY_MODEL` block in `boot()` (master
  already has unrelated `#ifndef` blocks further down, now at L1613 and L2316):
  - `mmu_map(0x00dff000, ..., 4096, 0, 0)` — carves the custom-chip register
    page out of the flat mapping so accesses to it fault and reach the
    intercepts above. The rest of the page still behaves like memory.
  - `mmu_map(0x00e80000, ..., 4096, 0, 0)` — same, for autoconfig space.
  - Initialises `board = &__boards_start; board_idx = 0`. On PiStorm the
    protocol code does this after a bus reset; standalone there is no reset, so
    it happens once at boot.

### `src/boards/emu68rom.c`

- **L35–L60** — the module list becomes `#ifdef PISTORM_ANY_MODEL` (the full
  list, unchanged) / `#else` (`devicetree.resource` and `brcm-sdhc.device`
  only). The omitted modules assume hardware a chipset-less guest does not
  have: `gic400.library` would be a second interrupt controller,
  `unicam.resource` is the Pi camera, `powerpc.library` is for a PPC board.

  This is a bring-up default, not a policy. The modules carry per-module
  `status` knobs in the device tree (`src/overlays/*.dts`), and driving the
  choice from there is the better answer once there is a reason to want more.

### `CMakeLists.txt`

- **L283** — adds `src/boards/emu68rom.c` to `BASE_FILES` in the `else()`
  branch of the variant block, i.e. for standalone builds. `z2ram.c` is
  deliberately left out: it exists to offer Zorro II RAM to a real Amiga, and a
  standalone guest already has all of memory.

## The patch series

| # | Patch | Files |
|---|---|---|
| 0001 | `emulate-amiga-interrupt-registers` | `vectors.c`, `start.c` |
| 0002 | `offer-zorro3-rom-board` | `vectors.c`, `start.c`, `CMakeLists.txt` |
| 0003 | `trim-standalone-module-list` | `emu68rom.c` |

### Why these cuts

Each patch is one coherent capability, and the series has no churn: no patch
removes or rewrites anything an earlier patch in the series added. Verified by
intersecting each patch's removed lines against every earlier patch's added
lines — the intersection is empty. The only lines any patch removes are
upstream lines being moved:

- 0001 removes 12 — the Amiga register `enum`, moved out of the PiStorm branch.
- 0002 removes 3 — `#ifdef PISTORM_ANY_MODEL` and `#include "ps_protocol.h"`,
  moved *below* the board declarations so those become shared.
- 0003 removes 1 — a blank line.

### The ordering constraint

0001 and 0003 apply to a pristine tree on their own; **0002 does not** — it
must follow 0001. The dependency is textual, not logical: interrupt emulation
and autoconfig emulation are independent features, but 0002 edits two regions
0001 has already reshaped (the declaration block in `vectors.c`, and the
`#ifndef PISTORM_ANY_MODEL` block in `start.c` that 0001 opens).

Making the three mutually independent would mean each opening its own
`#ifndef PISTORM_ANY_MODEL` block in `start.c` rather than sharing one — worse
code in exchange for a property a patch series is not expected to have. Ordered
application is the normal contract (it is what `git am` does), so the series
keeps the better code shape and documents the order instead.

## Working with the series

Apply one at a time, in order. `git apply` handed the whole series at once
checks every patch against the *original* tree, so 0002 fails:

```bash
cd external/emu68
for p in ../../patches/emu68/0*.patch; do git apply "$p" || break; done
```

The series is maintained as a branch in a fork — `feature/host-irq-abi` on
`github.com/JJDSNT/Emu68`, branched from the same upstream commit the submodule
is pinned to. Edit there, then regenerate:

```bash
git -C /path/to/Emu68 format-patch master..feature/host-irq-abi -o /path/to/bellatrix/patches/emu68
```

Rename the output to the `NNNN-<subject>.patch` form used here — `format-patch`
derives its filenames from the commit subjects, which are longer.

To prove a regenerated series still reproduces the branch exactly, apply it to
a pristine submodule and compare tree hashes — these must be equal:

```bash
cd external/emu68 && git add -A && git write-tree
git -C /path/to/Emu68 rev-parse feature/host-irq-abi^{tree}
```

For the current series both yield `e17d76c021aa1b9307014d82e0a3ec325cf72e55`.
