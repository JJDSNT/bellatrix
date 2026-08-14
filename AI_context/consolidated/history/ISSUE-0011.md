---
id: ISSUE-0011
title: "A harness that times the boot, from emulation start to Wanderer with icons"
status: done
priority: high
type: infra
owner: agent
created_at: 2026-08-06
updated_at: 2026-08-06
tags:
  - infra
  - measurement
  - boot
  - qemu
blockers:
related_files:
  - scripts/boot-timing.py
  - run.sh
  - scripts/make-sdcard.sh
  - docs/known-good-baseline.md
  - AI_context/consolidated/history/ISSUE-0007.md
  - AI_context/issues/ISSUE-0009.md
---

# Summary

There is no way to ask "how long does the boot take" other than watching it.
Every timing statement in this repository so far — "around eight minutes",
"a run cut at 200 s reads as failure" — comes from a human with a clock, and
every intermittency statement comes from counting by hand.

This issue is for a harness that runs one boot unattended, times it from the
start of emulation to the moment Wanderer has drawn the volume icons, and emits
one machine-readable record. Run it N times and the distribution — and the
failure rate — fall out of it.

# Problem

## What has to be measured

`t0` — emulation starts. The honest marker is the wall clock immediately before
`exec qemu-system-aarch64`, taken on the host. Emu68's first serial line is a
tempting alternative and is worse: it excludes QEMU startup, and it is a guest
event seen through a buffer.

`t1` — the Workbench is on screen **with volume icons**. This is the reference
the project actually uses ("a referência são os ícones aparecerem"), and it is
strictly later than the Workbench screen appearing.

## Why t1 is the hard part

Classifying by dominant colour separates the two grey screens — `#787878` is the
Emu68 logo, `#989898` the Workbench — and that is all it separates. It does not
distinguish an **empty** Workbench, still loading `Zune/IconListview.mui`, from
a finished one. Around 200 s the screen is already `#989898` and titled plainly
"Workbench Screen"; around 480 s the icons appear and the title becomes
`Wanderer <n>M graphics mem <n>M other mem`. A harness that stops at the colour
would report a boot roughly 2.5× faster than the real one and would call a
hung run a success.

So the detector needs a second stage. Candidates, cheapest first:

- **Pixel activity in the icon region.** Wanderer draws the volume icons in a
  known area of the backdrop. Capture one reference frame of the empty Workbench
  once, then declare `t1` at the first sample whose icon region differs from it
  by more than a threshold. Cheap, no OCR, but it needs the reference frame
  regenerated whenever the screen size or the distribution changes.
- **Distinct-colour count over the whole frame.** The empty Workbench is close
  to two-tone; icons and their labels add colours. Coarser, but it has no
  reference frame to go stale.
- **Title-bar text.** The strongest signal — the string genuinely changes — and
  the most expensive: it means reading pixels as text.

Pick one, write down why, and make the harness say which signal fired.

## Why the serial log cannot be trusted for timing

The guest console is deferred: the order in which lines arrive does not tell you
when the events happened. Any timestamp the harness records must be the moment
the **host** saw the byte, not anything the guest says about itself, and the
harness should not infer phase boundaries from output order. If per-phase
timings are wanted later, they need markers emitted deliberately, not inferred.

## Why this is worth building now

ISSUE-0007's real subject — the intermittency — has never been measured on the
restored baseline, and cannot be honestly quoted until something counts runs the
same way every time. The same harness answers both questions: a run that never
reaches `t1` inside the timeout *is* the failure rate. It is also the only way
ISSUE-0009's and ISSUE-0010's A/B comparisons can be more than an impression.

# Goal

```
$ scripts/boot-timing.py
{"t_total": 51.8, "verdict": "icons", "signal": "backdrop-delta",
 "t_workbench": 41.6, "run": "2026-08-06T13:20:28Z", "sd": "regenerated", ...}
```

One run, one record, appended to a log. N runs give a median, a spread and a
failure count without anyone counting.

# What was done

`scripts/boot-timing.py`, and a `--serial FILE` option on `run.sh` so the
harness can drive the existing invocation instead of copying it.

**The detector, and why it ended up in two stages.** The first stage is dominant
colour: `#787878` is the Emu68 logo, `#989898` is a Workbench-grey screen. That
is not enough on its own in *either* direction, and both failures were measured
rather than anticipated:

- A poisoned card reaches a Workbench-grey screen that is perfectly uniform and
  stalls there. Anchoring the baseline on that frame would put `delta0` in the
  wrong place and then fire on the real screen's own chrome. So a frame counts
  as a screen only when the **title band** is not blank — flat fills make that
  an exact test, blank is exactly zero non-modal pixels.
- The screen appearing is not the boot finishing. In ten runs the Workbench
  screen opened at 39–48 s *every time it opened at all*, including four runs
  that then sat on an empty backdrop — two of them for a full 900 s without one
  pixel changing. A colour-only detector would have called all eight successes.

The second stage is self-calibrating rather than a constant: at the first frame
with a title band, the number of backdrop pixels differing from the backdrop's
own modal colour becomes `delta0`; icons are declared when that count grows by
`--icon-delta` beyond it. Measured margin: 0 → 3833 in a single sample, against
a threshold of 1500. Nothing depends on where Wanderer puts the icons.

A corroborating signal fell out of the same data and is recorded but unused: the
title band's own count jumps from 9466 to 10870 in exactly the sample where the
icons appear — the title changing from "Workbench Screen" to
"Wanderer <n>M graphics mem", detected without reading a character.

**What it measured on the restored baseline**, ten runs, idle machine, a freshly
generated card before each:

| verdict | runs | |
|---|---|---|
| `icons` | 4 | t_total 46.1 / 51.1 / 51.8 / 53.3 s |
| `workbench` | 4 | screen at 42.5–47.9 s, then nothing; two held 900 s |
| `logo` | 2 | never left Emu68; ~79 KB of serial against ~127 KB |

That is the first measured intermittency rate on this baseline, and it corrected
a documented figure: `docs/known-good-baseline.md` said the icons took around
480 s and that a run cut at 200 s was being judged too early. They take about
50 s. What was being read as "slow" was a stall, and waiting eight minutes for
it changes nothing.

# What is left

Nothing for the harness itself. Two things it has now made cheap, tracked
elsewhere: attributing the stall (ISSUE-0007) and the A/B comparisons in
ISSUE-0009 and ISSUE-0010.

Not done, and deliberately: whether polling perturbs the run was not measured
head-on. The 2 s and 5 s interval series agree to within the spread of the
`icons` runs, which is weak evidence that it does not matter at these rates.

The original plan, for the record:

**1. Do not copy the QEMU command line.** `run.sh` is where the invocation is
defined, and a harness that carries its own copy will drift from it silently —
that has already cost this project a measurement run against a stale ELF. Extend
`run.sh` with what the harness needs (a `--serial FILE` to complement the
existing `BELLATRIX_QEMU_MONITOR`, and a way to not `exec` into a terminal), and
have the harness drive `run.sh`.

**2. Sampling.** Poll `screendump` over the monitor socket on a fixed interval;
the interval is the resolution, and on a ~480 s boot 5 s is about 1%. Record the
interval in the output — a number without its resolution is not comparable.
Whether polling perturbs the run is itself a question the harness can answer, by
comparing a 5 s interval against a 30 s one.

**3. Verdicts, not booleans.** At minimum: `icons` (reached `t1`), `workbench`
(reached the screen, never the icons, timed out), `logo` (never left Emu68),
`dead` (no serial output at all), `error` (harness failure). Collapsing the
middle two into "fail" throws away exactly the distinction that made this hard.

**4. Preconditions the harness enforces, not documents.** Refuse to start if
`ps -eo cmd | grep -c '[q]emu-system'` is not 0 — concurrent QEMU changes the
outcome. Regenerate the SD card before every run while ISSUE-0009 is open, since
one boot poisons the card for the next. Record the host's load average and the
Emu68/AROS build identity in the record, so a surprising number can be traced to
a contaminated host rather than to the change under test.

**5. Report a distribution.** A thin wrapper for N serial runs that prints
median, min, max and the verdict counts. Never a single number from a single
run — that rule is in `CLAUDE.md` and the tool should make following it the
easy path.

# Decisions taken

The verdict is decided from pixels alone. The serial log is captured and its
size recorded, but nothing in the classification reads it — a guest whose
console is deferred cannot be asked when anything happened.

Python, against the repository's bash convention, because this drives a process,
speaks a protocol over a socket and reads pixels; splitting that across two
languages would split the state that decides the verdict. Noted in the file's
header so the deviation is not mistaken for drift.

# Acceptance criteria

- [x] One command runs one boot unattended and emits one record
- [x] `t1` is the icons, not the screen — four runs reached the screen and never
      the icons, two of them held for 900 s, and all four were classified
      `workbench`. *(The original wording named a 200 s cut-off; that number came
      from the figure this work corrected. The property it was testing for is
      what was verified.)*
- [x] A known-good run comes back `icons` with a plausible `t_total` — verified
      once by eye against the actual frame, title and both volume icons
- [x] The harness refuses to run with another QEMU alive
- [x] The QEMU command line exists in exactly one place in the repository
- [x] N runs produce median/spread/verdict counts without hand-counting
- [x] The intermittency rate of the restored baseline is measured with it and
      recorded in ISSUE-0007

# Notes

**Scope is QEMU.** The number is host-dependent and comparable only within one
machine and one sitting; it is a regression metric, not a property of the port.
Real-hardware timing is a different measurement with different tooling, and
magnitudes from QEMU do not carry over to the Pi.

There is a throwaway ancestor of this in the session scratchpad — one controlled
boot with `-serial file:`, `-display none`, a fixed sleep, a `screendump` and a
dominant-colour classifier. It is worth reading and not worth keeping: the fixed
sleep is the assumption this issue exists to remove.

# Execution log

- 2026-08-06 — opened. Every timing and intermittency figure in the repository
  to date is hand-counted; ISSUE-0007 cannot be closed without this and
  ISSUE-0009/0010 cannot be compared without it.
- 2026-08-06 — built and closed the same day. The first calibration run returned
  `icons` at 51.8 s, which looked like a false positive against the documented
  480 s until the frame was opened and showed the real desktop: title
  "Wanderer 832.93M graphics mem", RAM Disk and Aros icons. Ten runs then
  established 4/10, and a deliberate second series at a 900 s timeout ruled out
  the "the stalled ones are merely slow" reading — two of them held an empty
  backdrop for the full 900 s with `delta` exactly 0.
- 2026-08-06 — corrected `docs/known-good-baseline.md`, which carried the 480 s
  figure and the advice not to judge a run before eight minutes.
