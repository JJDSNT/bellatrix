# patches

Changes applied on top of the `external/emu68` submodule, which is pinned to
upstream michalsc/Emu68 and never modified in place.

## Series

| # | Patch | What it does |
|---|---|---|
| 0001 | `emu68-emulate-amiga-interrupt-registers` | Carves a hole in the MMU at `0xdff000` and serves INTENA/INTREQ/INTENAR/INTREQR from `INT_shadow`, so a guest with no Paula can arm and acknowledge host interrupts the ordinary Amiga way. |
| 0002 | `emu68-offer-zorro3-rom-board` | Carves a hole at `0xe80000` and emulates the Zorro autoconfig protocol against Emu68's own board list, so a guest with no expansion bus can find the Z3 ROM board. Adds `src/boards/emu68rom.c` to the standalone build. |
| 0003 | `emu68-trim-standalone-module-list` | Offers a chipset-less guest only the modules it can actually use (`devicetree.resource`, `brcm-sdhc.device`) instead of the full PiStorm list. |

They form a dependency chain and must be applied in numeric order: 0002 builds
on 0001, and 0003 only takes effect once 0002 has added `emu68rom.c` to the
build.

## Applying

One at a time, in order. `git apply` given the whole series at once checks
every patch against the original tree, so 0002 and 0003 fail:

```bash
cd external/emu68
for p in ../../patches/0*.patch; do git apply "$p" || break; done
```

## Regenerating

The series is the diff between `master` and `feature/host-irq-abi` in the
Emu68 fork (`github.com/JJDSNT/Emu68`). Edit on the branch, then:

```bash
cd /path/to/Emu68
git format-patch master..feature/host-irq-abi -o /path/to/bellatrix/patches
```

Rename the output to the `NNNN-<subsystem>-<subject>.patch` form used here.

To verify a regenerated series still reproduces the branch exactly, apply it to
a pristine submodule and compare the resulting tree hash against the branch:

```bash
cd external/emu68 && git add -A && git write-tree   # must equal
git -C /path/to/Emu68 rev-parse feature/host-irq-abi^{tree}
```
