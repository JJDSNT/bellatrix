---
id: ISSUE-0011
title: "A harness that times the boot, from emulation start to Wanderer with icons"
status: backlog
priority: high
type: infra
owner: unassigned
created_at: 2026-08-06
updated_at: 2026-08-06
tags:
  - infra
  - measurement
  - boot
  - qemu
blockers:
related_files:
  - run.sh
  - scripts/make-sdcard.sh
  - docs/known-good-baseline.md
  - AI_context/issues/ISSUE-0007.md
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
$ scripts/boot-timing.sh
{"t_total": 486.2, "verdict": "icons", "signal": "icon-region-delta",
 "t_workbench": 203.9, "run": "2026-08-06T14:02:11Z", "sd": "regenerated", ...}
```

One run, one record, appended to a log. N runs give a median, a spread and a
failure count without anyone counting.

# What is left

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

None yet.

# Acceptance criteria

- [ ] One command runs one boot unattended and emits one record
- [ ] `t1` is the icons, verified against a run deliberately cut at 200 s, which
      must come back `workbench` and not `icons`
- [ ] A known-good run comes back `icons` with a plausible `t_total`
- [ ] The harness refuses to run with another QEMU alive
- [ ] The QEMU command line exists in exactly one place in the repository
- [ ] N runs produce median/spread/verdict counts without hand-counting
- [ ] The intermittency rate of the restored baseline is measured with it and
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
