# The known-good baseline

Established 2026-08-05, after a regression that made the desktop unreachable and
took a day to trace because nothing in the tree could be rebuilt from the
repository.

## What it is

The state that boots AROS/m68k under Emu68 to the Workbench screen **with volume
icons drawn**. Two upstream forks define it:

| | branch |
|---|---|
| AROS | `JJDSNT/AROS` `feature/m68k-emu68-baremetal` |
| Emu68 | `JJDSNT/Emu68` `feature/host-irq-abi` |

This repository does not vendor those branches. It vendors the upstream commits
they are built on and reproduces the difference as patch series — which is only
meaningful if the reproduction is checked, so it is:

- Both pinned commits are ancestors of their branch (`d0370bd757` for AROS,
  `9b4379a5c5` for Emu68).
- Of the 78 files the AROS fork changes, 57 are under `arch/m68k-emu68/` — this
  repository's own code, carried as a symlinked directory rather than a patch —
  and 53 of those match byte for byte. The four that differ are the three fixes
  measured on 2026-08-05 (`emu68gfx` raster limits, `intc_dispatch()` enable
  mask, `systimer_arm()`) plus a README.
- Of the remaining 21, all are reproduced by `patches/aros/`.

`./scripts/setup.sh --verify` reporting `already applied` for both submodules is
the mechanical form of this claim, and it is the only one worth trusting.

**One deliberate divergence, 2026-08-06.** `main` no longer carries the Emu68
fork's Zorro III commits: `patches/emu68/` is one patch, not three. Nothing in
this port consumes the board — no `expansion.library`, no autoconfig sweep — and
while it is offered, the 64 KB window at `0xe80000` stops faulting and starts
answering stray accesses. The reference pair is still the reference for
everything else; this is the one place `main` is intentionally smaller than it,
and the reasoning is in `AI_context/issues/ISSUE-0007.md`.

## How to check you are on it

```bash
./scripts/setup.sh --verify        # must say "all series applied"
./scripts/build-aros.sh            # out/aros/aros-emu68-m68k.elf
./scripts/build.sh                 # out/images/Emu68.img
./scripts/make-sdcard.sh --dist <full distribution tree>
./run.sh
```

The lean `kernel-link-<target>` build produces only the ELF. A bootable card
needs a full distribution tree; `/home/jaime/aros-build-emu68-m68k/bin/emu68-m68k/AROS`
is the one these measurements used.

To ask the same question without watching it, and without counting by hand:

```bash
BELLATRIX_SD_DIST=<full distribution tree> scripts/boot-timing.py -n 10
```

One record per run in `out/boot-timing.jsonl`, a verdict of `icons`,
`workbench`, `blank`, `logo` or `dead`, and a summary at the end. It regenerates
the card before every run, refuses to start with another QEMU alive, and keeps
the whole frame-by-frame timeline in each record. See
`AI_context/issues/ISSUE-0011.md`.

**Correction, 2026-08-06: the boot is not slow, it is intermittent.** This
section used to say Wanderer took around 480 seconds to draw the icons and that
a run cut at 200 seconds was being judged too early. Measured with
`scripts/boot-timing.py` over ten runs on an idle machine, each on a freshly
generated card, that is wrong:

| | |
|---|---|
| Workbench screen opens | 39–48 s, in every run that got that far |
| icons drawn | 46–53 s, in the runs that finished |
| runs that finished | 4 of 10 |
| runs that stalled on an empty Workbench | 4 of 10 — two held for a full 900 s with the backdrop unchanged |
| runs that never left the Emu68 logo | 2 of 10 |

So a run that has not drawn icons by ~60 s is not slow; it is stuck, and waiting
eight minutes for it changes nothing. What the old text described as "still
loading `Zune/IconListview.mui`" is one of the stall states, not a phase.

Opening the screen is very nearly deterministic — the spread is nine seconds
across ten runs, whatever the outcome. What is intermittent is Wanderer
finishing after that. See `AI_context/issues/ISSUE-0007.md`.

## Two ways this baseline has been lost before

**Drift inside a submodule is invisible.** `ignore = dirty` in `.gitmodules` and
`skip-worktree` inside the submodule together mean `git status` answers this
question wrongly by construction, at both levels. On 2026-08-05 there were
nineteen drifted files in `external/aros` and one in `external/emu68` — and the
Emu68 one, an open-bus guard discarding every access above 16 MB outside
`sys_memory`, was throwing away every write to the framebuffer at `0x3c100000`.
The machine booted perfectly and nothing could ever appear on screen.

Ask `setup.sh --verify`, never `git status`. To find *what* drifted, check out
the pinned commit in a scratch worktree, apply the series into it, and
`diff -rq` against `external/<name>/`.

**The card poisons itself.** The font indexes (`arial.font`, `ttcourier.font`,
`xen.font`, `stop.font`, `fixed.font`, `fontcache`) ship in no distribution —
they are generated during boot. Written through a fat-handler without
little-endian conversion, their size, cluster and date fields are byte-swapped:
2604 bytes reads back as 738852864. Every boot after that hangs in
`OpenDiskFont` with nothing on the serial. `mdel` hangs too, walking the same
bogus chain.

`mdir -i out/aros/sd.img@@1M ::/Fonts` shows it immediately.

**Fixed 2026-08-06, and the earlier verdict here was wrong.** This section used
to say the repair had been measured as a regression — three runs, three
failures. It had been, and the measurement was misattributed: the change was
promoted as part of `aros-extras-drift.patch`, which bundles the FAT fix with
MUI and Wanderer tracing (`#define DEBUG 1` in four files, dozens of traces per
redraw). The failures are far more likely to have been that.

The real defect was located by looking at the bytes. A boot wrote a 17-byte file
and copied a 1944-byte binary to the card; on the host, the *sizes* read back as
`0x11000000` and `0x98070000` — byte-reversed — and the year as 2028, while the
file **contents** were byte-identical to the originals, all 1944 bytes. So the
block path is clean and the SD backend is innocent: the loss is entirely in how
the directory-entry fields are composed. `rom/filesys/fat` converts cluster and
size on read and not on write, and the dates on neither side.

`patches/aros/0008` converts them, with the date conversion inside
`ConvertFATDate`/`ConvertDOSDate` so the halves cannot drift apart again. After
it: sizes 17 and 1944, correct dates, and `mcopy` — which previously failed with
`Fat problem while decoding` — reads both files back byte for byte.

This is an upstream AROS defect on every big-endian target; `m68k-amiga`,
`ppc-native` and `ppc-morphos` share the code.

**It does not change the boot rate, and should not be expected to.** Every run
measured here is on a freshly generated card, whose files were written correctly
by `mcopy`; the read path already converted. What the bug broke was the *second*
boot, on files the first boot wrote — which the regenerate-every-run protocol
never exercised.

The lesson worth keeping is not about FAT: **a measurement of a bundled change
attributes to all of it.** The verdict recorded here was honest and still wrong,
because the thing measured was not the thing named.

## What is not fixed

Reaching the desktop is intermittent, at roughly 38% over 101 measured runs on
2026-08-06, and **nothing changed that rate all day** — every configuration
tried sits inside every other one's confidence interval. Three real defects were
closed and the failure became diagnosable rather than silent, which is a
different achievement and should not be reported as this one.

The failure now surfaces as a wild guest PC inside `InternalLoadSeg_ELF`. See
`AI_context/issues/ISSUE-0007.md`, and the measurement discipline in `CLAUDE.md`
before quoting any rate.
