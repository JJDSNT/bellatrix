# Bring-up scaffolding

Things that make the port measurable, kept here because they are not part of
the product and do not belong in a build tree where they would drift silently.

## `Startup-Sequence.minimal`

The stock `S:Startup-Sequence` is ~120 lines. This is 25: the assigns Wanderer
needs and nothing else. Everything discretionary is out — `SetClock`,
`Automount`, `Mount`, `Path`, `Copy ENVARC: ENV: ALL`, `IPrefs`,
`AddDataTypes`, `ConClip`, `Decoration`.

**In place as of 2026-08-06** in
`/home/jaime/aros-build-emu68-m68k/bin/emu68-m68k/AROS/S/Startup-Sequence`,
with the stock one alongside as `S/Startup-Sequence.stock`.

That is a deliberate, recorded state, not drift — but it *is* a modified
reference distribution, so **any measurement taken against it is measuring the
minimal boot**, and comparisons with earlier numbers in ISSUE-0007 are not
like-for-like unless they say so. Swap the two files to go back.

**What it measured** (6 runs, 2026-08-06, against the stock sequence):

| | stock | minimal |
|---|---|---|
| screen opens | 38–50 s | **17–22 s** |
| icons | 46–55 s | **33–39 s** |
| `logo` failures | common | **none** |
| `workbench` failures | some | 4 of 6 |
| icons | ~38% | 2 of 6 |

It does not make the boot reliable. What it does is **split the failure in
two**: every early death (`logo`) comes from something in the discretionary
part of the startup sequence, and what remains is a single late failure —
Wanderer opens its screen, every time and much sooner, and then does not draw
the icons.

That is a far better target than the stock boot: a small system, a fast cycle,
and one failure instead of two. The obvious next step is to add the removed
steps back one at a time until `logo` returns, which names the early culprit.
