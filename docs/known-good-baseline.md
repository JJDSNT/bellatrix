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

`mdir -i out/aros/sd.img@@1M ::/Fonts` shows it immediately. The remedy today is
to regenerate the card.

The obvious repair does not work, and this was measured rather than reasoned
about. The fat-handler change parked on branch `codex-2026-08-05` as part of
`aros-extras-drift.patch` was made into `patches/aros/0007`, applied through
the normal path, and built: **three runs, three failures**, one of them stopping
as early as `IPrefs`. Reverted, on the same freshly generated card, two runs and
two Workbench screens. It is a regression, not a fix.

The change bundles two different things: little-endian conversion at the *write*
sites, which is what stops the corruption, and substituting `FIRST_FILE_CLUSTER()`
plus `AROS_LE2WORD` at the *read* sites. If the handler already converts on the
way in, the read half converts a second time and breaks every lookup. Splitting
the two and taking only the write side is the next thing to try — with the same
protocol, since the unfixed handler poisons the card and one run therefore
contaminates the next: **regenerate the card before every single run.**

## What is not fixed

Reaching the desktop is intermittent. Reliability has not been measured on this
baseline yet — see `AI_context/issues/ISSUE-0007.md`, and the measurement
discipline in `CLAUDE.md` before quoting any rate.
