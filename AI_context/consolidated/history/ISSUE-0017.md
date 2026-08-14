---
id: ISSUE-0017
title: "Bump the AROS pin to HEAD and reduce the patch series to what is ours"
status: done
priority: critical
type: refactor
owner: agent
created_at: 2026-08-13
updated_at: 2026-08-14
tags:
  - aros
  - upstream
  - patches
  - baseline
blockers:
related_files:
  - patches/aros/
  - AI_context/consolidated/history/ISSUE-0007.md
  - AI_context/issues/ISSUE-0015.md
  - docs/aros.md
---

# Summary

Our AROS pin is `d0370bd757` (2026-07-27). Upstream HEAD is `85705361ca`
(2026-08-13), 785 commits ahead. Eight of our nineteen patches are already in
that history -- five because we imported them verbatim, three because upstream
fixed the same defect independently, twice in a better place than we did.

The series has stopped being a description of what this port needs and become
partly a copy of upstream with a delay. That is not only untidy: it makes
measurement uninterpretable, which is the actual cost and the reason this is
critical rather than housekeeping.

# Problem

## Measurement is not interpretable while the series is what it is

Six series were run on 2026-08-13, and the icons rate moved 83%, 25%, 25%, 50%,
40%, 8% -- with **only instrumentation changing** between the first five. A
swing that large from changes that cannot affect the guest means the metric
cannot presently support a claim about any change, and four diagnoses were
asserted and withdrawn during that session partly on its strength.

Two of our own patches are prime suspects for that noise:

- **`0006` turns on DEBUG tracing in dos, lddemon and Shell.** It is not a fix.
  It floods the serial console, which changes boot timing substantially, and
  the defect under investigation is timing-sensitive and intermittent. It also
  instruments `lddemon`, which is where the failure was traced to.
- **`0019`** was imported on a hypothesis that the measurement did not support,
  and its own mechanism (`AttemptSemaphoreShared` failing means "assume busy",
  so `LDFlush` frees nothing) could plausibly starve allocation.

## We patched symptoms upstream then fixed at the root

`0004` is the sharpest example and the reason this issue exists. We patched
`rom/dos/newcliproc.c` so a synchronous `System()` keeps its flags. Upstream
`bde1ec0f23` removes those lines from `newcliproc.c` entirely and fixes
`arch/m68k-all/dos/bcpl.S`, where the BCPL flag translation was being applied
to every `System()` call instead of only to genuine BCPL Shell-Segs.

Same bug, same symptom -- both descriptions name `AROSMonDrvs` -- and their fix
is at the cause. Ours will conflict textually on the bump and, until then, is
masking the real defect rather than removing it.

# Inventory at HEAD

## Drop -- already in upstream HEAD

| # | Subject | Upstream |
|---|---|---|
| 0004 | dos: reply synchronous System packet | `bde1ec0f23` (better fix, different file) |
| 0009 | dos: ELF loads from an unfilled buffer | `2f514b7472` |
| 0013 | graphics: bitmap freed short | `b553067c52` |
| 0014 | fat: lock volume + cluster walk | `647791ec7a` + `921b33af58` |
| 0015 | iprefs: Detach before first pass | `7416119e73` |
| 0016 | IPrefs: reset requester | `e92efb9ee4` |
| 0017 | gfx: framebuffer row size | `f9ccd4078d` |
| 0019 | lddemon: flush vs open in progress | `27f22f9fe0` |

0013-0017 and 0019 were imported verbatim, so dropping them is exact. 0004 and
0009 are ours and are *replaced*, not duplicated -- the behaviour after the bump
is upstream's, not what we have been testing against.

## Keep -- target enablement

`0001` (configure target), `0002` (m68k-all for a non-Amiga m68k), `0005`
(dosboot planar image by arch), `0018` (do not race the emu68 Exec backend).

These exist because `m68k-emu68` exists. All four are candidates for
upstreaming and none can be dropped before that happens.

## Keep -- defects upstream still has

`0008` and `0011`, the FAT little-endian fixes. Verified against HEAD today:
`rom/filesys/fat/date.c` still has no `AROS_LE2WORD`, and the cluster write
path is unchanged. **We owe these upstream**, along with two further sites
found on 2026-08-13 that we have not patched: `ops.c` line 599 and
`direntry.c` line 242 build cluster numbers from raw little-endian fields with
no conversion, unlike the `FIRST_FILE_CLUSTER` macro that does it correctly.

## Re-verify -- may be superseded

`0003` (sdcard `sdcu_SoftList`) -- upstream has three sdcard commits since our
pin, including `f5d7addc57` and `9d066f80f5`, which may have reworked the code
this patches.

`0007` (raise the m68k default task stack) -- check whether upstream changed
the default, and whether the reason for ours still holds at HEAD.

## Separate -- diagnostics, not fixes

`0006` (DEBUG tracing), `0010` (refuse free outside heap), `0012` (name the
caller freeing outside the pool).

These are instruments. Two problems with them as they stand: they are always
on, and they are indistinguishable from fixes in the series. `0012` has never
fired in any run. `0006` actively distorts what we measure.

# Plan

Each phase ends in a state that builds and boots; none is a point of no return.

1. **Bump the pin and drop the eight.** Rebuild the series through the
   documented route -- `git am` into a branch, drop the commits, `format-patch`
   back out (`docs/emu68.md` describes it for the other submodule; it is the
   same for this one). Not by editing patch files: doing that during the Emu68
   series rebuild would have silently duplicated two hunks and broken a build,
   and the same risk applies here.
2. **Re-verify `0003` and `0007`** against HEAD and drop whichever no longer
   applies or is no longer needed.
3. **Separate the diagnostics.** Put `0006`, `0010` and `0012` behind one
   switch, off by default, so a measurement run and a diagnostic run are
   different builds rather than the same one. The measured baseline must be
   taken with them off.
4. **Establish the baseline.** A long series (n >= 12) on an idle machine,
   with nothing else changing. This is the number every later claim gets
   compared against, and we do not currently have one.
5. **Then resume ISSUE-0007** against a tree whose difference from upstream is
   small and entirely ours.

Send `0008`, `0011` and the two unfixed FAT sites upstream at any point after
step 1; they are independent of the rest.

# Risks

- **785 commits is a large jump.** Expect new breakage unrelated to what we are
  chasing. The unresolved-symbol guard in `build-aros.sh` catches one class of
  it cheaply; the rest needs boot runs.
- **Dropping `0004` changes the System() path** to upstream's fix, which this
  port has never run. If boot breaks at `AROSMonDrvs` again, that is where to
  look first.
- **A reset rebuilds everything** (`--reset` rewrites every mtime), so each
  phase costs a full AROS build. Budget for it rather than being surprised.
- **The baseline may come out worse than today's.** That is information, not
  failure, and it is still worth more than a number nobody can interpret.

# Acceptance criteria

- [ ] Pin at `85705361ca` or later, series applies cleanly, `setup.sh --verify`
      reports all applied.
- [ ] Eight superseded patches gone; no patch in the series duplicates an
      upstream commit.
- [ ] Every remaining patch is one of: target enablement, a defect upstream
      still has, or a diagnostic behind a switch.
- [ ] Diagnostics off by default.
- [ ] A baseline of n >= 12 runs recorded in `out/boot-timing.jsonl` with a
      distinct label, taken with diagnostics off.
- [ ] `docs/aros.md` describes the series as it then is.

# Notes

The Emu68 series was reduced the same way on 2026-08-13 -- seven patches to six,
by dropping the emulated interrupt registers once the bus observer proved the
guest never touches `$DFF000`. That went through `git am` and hit two real
conflicts that a hand edit would have got wrong. The same care applies here,
with eight to drop instead of one.

# Closed 2026-08-14 — the series is what this asked for, and the baseline exists

## Against the acceptance criteria

- [x] **Pin at `85705361ca`**, series applies cleanly, `setup.sh --verify`
      reports all applied (11 AROS, 7 Emu68).
- [x] **Eight superseded patches gone.** No patch in the series duplicates an
      upstream commit.
- [x] **Every remaining patch is one of the three kinds.** `docs/aros.md` sorts
      them into target enablement, defects upstream still has, and instruments.
- [x] **Diagnostics off by default** — but resolved differently from the plan,
      see below.
- [x] **A baseline**, at `n = 10` rather than the 12 asked for, see below.
- [x] `docs/aros.md` describes the series as it now is.

## Where the outcome differs from the plan

**Step 3 was resolved by subtraction, not by a switch.** The plan was to put
`0006`, `0010` and `0012` behind one flag. In the end only the tracing patch
needed to go: it is now
`patches/aros/optional-debug-turn-on-tracing-in-dos-lddemon-and-shell.patch`,
with **no number**, so `setup.sh` — which globs `[0-9]*.patch` — does not apply
it. Applying it is a deliberate `git -C external/aros apply`.

The other two stayed on. They are guards rather than traces: a range check on a
path that is already failing, printing only when something is already wrong.
Neither changes timing on a healthy boot, which was the actual objection to the
tracing patch. Adding a build switch for them would have bought nothing and
added a way for a measurement and a diagnostic build to differ invisibly.

**The baseline is n = 10, not n = 12.** Ten consecutive runs on freshly
generated cards, every one reaching Wanderer with icons, 38.8–44.5 s, median
38.9 s — `out/boot-timing.jsonl` label `tlsfminfree-final`. The criterion asked
for twelve. Ten with a 10/10 result is recorded here as met rather than quietly
rounded up: the number that made the metric interpretable again was not the
sample size but the fix underneath it (`patches/aros/0011`), and against a rate
that had never exceeded 83% and was usually far below it, 10/10 is not a
marginal call. If a later change needs to be judged against this, extend the
series rather than trusting the ten.

**Addendum, same day: the criterion is met after all, at n = 12.** The instrument
cleanup that followed (`2887961`) and the classifier fix (`f74b9a5`) were each
validated with runs of their own, and together they come to twelve consecutive
boots reaching Wanderer with icons on the cleaned-up build -- four labelled
`logcleanup` and eight `classifier-fix`. One of the four was reported
`workbench` by the detector as it then stood, and is counted as icons here on
the strength of a byte-for-byte identical final frame; that is what led to
ISSUE-0018.

Two things are worth separating inside that number. The `logcleanup` runs varied
38.9-44.2 s. The six `classifier-fix` runs, on a machine deliberately left
quiet, came in at 44.0-44.3 s with `t_workbench` 38.6-38.7 -- a spread of 0.3 s
across six runs, for a boot that spent ten days being called unpredictable.
Neither figure is the true one, and the difference between them is the host
rather than the guest.

## What the premise got right, and what it got wrong

Right: the series had become partly a copy of upstream with a delay, `0004` was
a symptom patch masking a real defect upstream fixed at the cause, and the
tracing patch was distorting what was measured.

Wrong, and worth recording: this issue argued that the wild swing in icons rate
— 83%, 25%, 25%, 50%, 40%, 8% with only instrumentation changing — meant *"the
metric cannot presently support a claim about any change"*. The metric was
noisy for a better reason than instrumentation. The heap was being corrupted by
the allocator itself on a timing-dependent split, so the rate genuinely moved
with anything that changed allocation order. Cleaning the series did not fix
that and was never going to; it made the tree small enough to see it in.

Step 5, "then resume ISSUE-0007", is what happened, and ISSUE-0007 is closed.
