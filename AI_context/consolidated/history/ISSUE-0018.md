---
id: ISSUE-0018
title: "The icons detector reported reaching the desktop as failing to, in 21% of its 'workbench' verdicts"
status: done
priority: high
type: bug
owner: agent
created_at: 2026-08-14
updated_at: 2026-08-14
tags:
  - measurement
  - tooling
  - boot-timing
blockers:
related_files:
  - scripts/boot-timing.py
  - out/boot-timing.jsonl
---

# Summary

`scripts/boot-timing.py` decides "the desktop has icons" by counting backdrop
pixels that differ from the backdrop's modal colour, anchoring a baseline
`delta0` on the **first frame classified as `screen`**, and declaring `icons`
when the count grows by `--icon-delta` beyond it.

If the icons are already drawn in that first `screen` frame, `delta0` absorbs
them. The growth being looked for can then never happen, and the run is reported
as `workbench` — "reached the desktop but never drew icons" — having in fact
drawn them before the detector took its baseline.

**34 of the 165 `workbench` verdicts in `out/boot-timing.jsonl` (21%) have
`delta0 >= icon_delta`** and would now be classified `icons`.

# How it was found

Four runs after the 2026-08-14 log cleanup came out 3 icons, 1 workbench, which
did not match the 10/10 measured hours earlier and looked like a regression from
the cleanup.

It was not. The retained final frames of the `icons` run and the `workbench` run
were **byte-for-byte identical**, same MD5 `c72296ac3e5b...`, same 292497 grey
pixels and 8184 icon-white ones:

| Run | verdict | `t_workbench` | `delta0` | final frame md5 |
|---|---|---|---|---|
| `2026-08-14T115625Z` | icons | 33.4 | 0 | `c72296ac3e5b…` |
| `2026-08-14T115707Z` | workbench | 38.7 | 3833 | `c72296ac3e5b…` |

The only difference between them is which sampling window the screen opened in.
At a 5.4-second resolution, the screen and its icons can land in the same frame.

This is the same lesson as the `$E80000` probe in `boot.c`, in the other
direction: there, a silent instrument might have meant "nothing happened" or "I
am not watching". Here, a negative verdict meant "it did not happen" or "it had
already happened before I started looking". **An instrument's null and its
negative both need a way to be distinguished from its blind spot.**

# Fix

`delta0` is no longer anchored unconditionally. A first `screen` frame whose
delta is already past the threshold is icons, not a baseline:

```python
delta0 = 0 if frame["delta"] >= args.icon_delta else frame["delta"]
```

and the comparison became `if` rather than `elif`, so that frame can decide the
run instead of only seeding it.

Safe against a false positive: a bare backdrop reads `delta 0` in every sample
recorded across 380 runs, so nothing sits between 0 and the 1500-pixel threshold
for the baseline to be confused with.

`t_total` then equals `t_workbench` for such runs. That is honest — the two
events are not separable at this resolution — and it is visible in the record
rather than hidden.

# What this does and does not change

**Does not change the conclusion of ISSUE-0007.** The 10/10 baseline
(`tlsfminfree-final`) was 10 runs already verdicted `icons`; a detector that
under-reports icons cannot have inflated it.

**Does change the numbers reported during the investigation.** The affected
runs are spread across nearly every label — `memtrim` (6), `hostmem` (4),
`battclock` (3), `mungwall` (2), `tlsfscan` (2), `no-preserveall` (2) and
twelve others with one each. Several A/B rates quoted while chasing ISSUE-0007
understated success, by up to about 20% of the failures in a series.

That is worth stating plainly: **some of the "this made it worse" readings
during that investigation were partly the instrument.** It does not rescue any
of the withdrawn hypotheses — they were withdrawn on mechanism, not on rate —
but it is another reason the rate was never as informative as it looked.

The historical records are left as written. `delta0 >= 1500` on a `workbench`
row is the flag for a row that would now read `icons`; rewriting the file would
destroy the evidence for this issue.

# Acceptance criteria

- [x] A run whose icons arrive in the same window as the screen is verdicted
      `icons`.
- [x] The frame-hash comparison that proved it is recorded above.
- [x] The historical count of affected rows is recorded, and the rows are left
      unmodified.
