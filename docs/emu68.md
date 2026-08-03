# Emu68

Emu68 is the M68K→AArch64 JIT this project builds on. It lives at
`external/emu68`, a submodule tracking upstream
[michalsc/Emu68](https://github.com/michalsc/Emu68), pinned at **`9b4379a`**.

The submodule is never edited in place. Every change below comes from a patch
in `patches/emu68/`, applied by `scripts/setup.sh`.

## Why anything is modified

Emu68 was built to run on a PiStorm, with a real Amiga on the other side of the
bus. Two consequences run through the source as `#ifdef PISTORM_ANY_MODEL`:
every Amiga address is unmapped and therefore faults into
`SYSReadValFromAddr()` / `SYSWriteValToAddr()` to be forwarded to the bus, and a
real chipset answers on the far end.

Standalone, that address space is plain RAM — nothing faults, so nothing can be
intercepted — and nobody answers. A guest with no Amiga hardware therefore
cannot arm an interrupt or discover an expansion board, though Emu68 already
carries the machinery for both. The series closes exactly that gap. None of it
is specific to this project; all of it is a candidate for upstreaming.

## What is modified

Line numbers are as of pin `9b4379a` with the full series applied, for
orientation only — the anchors are the named regions.

### `src/aarch64/vectors.c`

The file has a PiStorm implementation (`#ifdef PISTORM_ANY_MODEL`, L344) and a
standalone one (`#else`, from L736), each with its own `SYSWriteValToAddr()`
and `SYSReadValFromAddr()`. **All new interception is in the standalone half**,
so a PiStorm build is unaffected.

| Where | Change | From |
|---|---|---|
| L311–L343, above the `#ifdef` | The Amiga register `enum` (`CIAAPRA`, `VPOSR`, `INTENA`, `INTENAR`, `INTREQ`, `INTREQR`, …) moved out of the PiStorm branch into shared scope | 0001 |
| L311–L343 | `#include <boards.h>`, `int board_idx`, `struct ExpansionBoard **board` moved out likewise; `AUTOCONFIG_BASE`/`AUTOCONFIG_END` (`0xE80000`–`0xE8FFFF`) added | 0002 |
| L756, write | `INTENA` — set/clear `INT_shadow.INTENA` (bit 15 = set vs. clear), then re-evaluate `ctx->INTF.ARM`, which the core-0 IRQ fast path in `__stub_vectors()` gates on | 0001 |
| L771, write | `INTREQ` — same over `INT_shadow.INTREQ`. Clearing EXTER (`(value & 0xa000) == 0x2000`) also drops `ctx->INTF.ARM` and `INT_shadow.ARMPending`: this is how the guest acknowledges a host interrupt | 0001 |
| L867, read | `INTENAR` — served from `INT_shadow.INTENA`, word or either byte half. Gated on `size <= 2` | 0001 |
| L878, read | `INTREQR` — served from `INT_shadow.INTREQ`, **plus bit 13 (EXTER) forced on whenever `INT_shadow.ARMPending`**. Gated on `size <= 2` | 0001 |
| L802, write | Autoconfig — writing the base address at `0xe80044` (Z3) or `0xe80048` (Z2) maps the current board and advances `board_idx`; "shut up" (`0xe8004c`/`0xe8004e`) skips it | 0002 |
| L900, read | Autoconfig — byte reads return the current board's ROM, skipping boards whose `enabled` is clear. Gated on `size == 1` | 0002 |

The forced EXTER bit is the load-bearing line: it lets an unmodified Amiga
level-6 handler conclude that EXTER fired and run its interrupt server chain,
with no Emu68-aware code in the guest.

Every intercept is gated on access size, and must be: claiming an access means
returning without filling `*value2`, which on a 16-byte read hands the guest a
register full of stack garbage. Upstream's fall-through `switch(size)` fills
both for `case 16`; an early `return 1` does not. Wider accesses therefore fall
through to memory exactly as they did before the series.

### `src/aarch64/start.c`

A new `#ifndef PISTORM_ANY_MODEL` block in `boot()` at L1376–L1409 (master's own
`#ifndef` blocks are unrelated, now at L1613 and L2316), plus `#include
"boards.h"` at L22.

| Where | Change | From |
|---|---|---|
| L1385 | `mmu_map(0x00dff000, …, 4096, 0, 0)` — carves the custom-chip register page out of the flat mapping so accesses fault and reach the intercepts above. The rest of the page still behaves like memory | 0001 |
| L1394 | `mmu_map(0x00e80000, …, 4096, 0, 0)` — the same for autoconfig space | 0002 |
| L1402 | `board = &__boards_start; board_idx = 0`. On PiStorm the protocol code does this after a bus reset; standalone there is no reset, so it happens once at boot | 0002 |

### `src/boards/emu68rom.c`

| Where | Change | From |
|---|---|---|
| L35–L60 | The module list becomes `#ifdef PISTORM_ANY_MODEL` (full list, unchanged) / `#else` (`devicetree.resource` and `brcm-sdhc.device` only) | 0003 |

The omitted modules assume hardware a chipset-less guest does not have:
`gic400.library` would be a second interrupt controller, `unicam.resource` is
the Pi camera, `powerpc.library` is for a PPC board. This is a bring-up
default, not a policy — the modules carry per-module `status` knobs in the
device tree (`src/overlays/*.dts`), which is the better place to drive the
choice from once there is a reason to want more.

### `CMakeLists.txt`

| Where | Change | From |
|---|---|---|
| L283 | `src/boards/emu68rom.c` added to `BASE_FILES` in the `else()` branch of the variant block, i.e. standalone builds. `z2ram.c` is deliberately left out: it offers Zorro II RAM to a real Amiga, and a standalone guest already has all of memory | 0002 |

## Not patched: the toolchain

Emu68 ships `toolchains/aarch64-linux-gnu.cmake`, which selects the compiler by
name with the version pinned — `aarch64-linux-gnu-gcc-14`. That is upstream's
choice, so instead of patching it the build passes its own
`cmake/aarch64-linux-gnu.cmake`, identical but unpinned. It builds clean with
GCC 13.

`scripts/build.sh` and `./run.sh` use it; artefacts land under `out/`.

## The series

| # | Patch | Files |
|---|---|---|
| 0001 | `emulate-amiga-interrupt-registers` | `vectors.c`, `start.c` |
| 0002 | `offer-zorro3-rom-board` | `vectors.c`, `start.c`, `CMakeLists.txt` |
| 0003 | `trim-standalone-module-list` | `emu68rom.c` |

Applied in numeric order by `scripts/setup.sh`, which is idempotent and
verifies the result by tree hash. The order is required: 0002 edits two regions
0001 reshapes. That coupling is textual, not logical — the two features are
independent, but making the patches independent would mean three separate
`#ifndef PISTORM_ANY_MODEL` blocks in `start.c` instead of one.

No patch in the series undoes an earlier one. The only lines any of them remove
are upstream lines being moved: 0001 moves the register `enum` out of the
PiStorm branch, 0002 moves `#ifdef PISTORM_ANY_MODEL` and `#include
"ps_protocol.h"` below the board declarations.

To edit the series, rebuild it as commits inside the submodule and write it
back out:

```bash
cd external/emu68
PIN=$(git rev-parse HEAD)
git reset --hard $PIN && git clean -fd   # git am needs a pristine tree
git checkout -b patch-wip && git am ../../patches/emu68/0*.patch
# edit and commit, or git rebase to reshape
git format-patch --zero-commit --no-signature $PIN..patch-wip -o ../../patches/emu68
git checkout --detach $PIN && git branch -D patch-wip
```

Rename the output to the `NNNN-<subject>.patch` form used here, then confirm
with `./scripts/setup.sh --reset && ./scripts/build.sh`.

Applying the series to a pristine submodule currently yields tree
`b9e1d1832baa8e1130ed1d9a733e38fa1d63385c`. `setup.sh` derives that hash from
the patches themselves, so nothing has to be kept in sync — it is recorded here
only so an unintended change is visible.
