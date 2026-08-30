---
id: ISSUE-0077
title: "The console sink drops every second byte when two producers write"
status: open
priority: medium
type: defect
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-30
tags:
  - console
  - multicore
  - diagnostics
blockers: []
related_files:
  - src/amiga/console.c
---

# Symptom

`PsdDevLister`'s output, through `DEBUG:`, on hardware:

```text
 oednDvD  Hb d=00PD00-0000-/-0
 rdc ae   Hb(0000) I:00,Vr:00) Mnfcue  :'/'(edr 00(nnw)
```

The first is `"  Poseidon DevID  : 'Hub: Vdr=0424/PID=9514-0424-9514-n/a-00'"`
with every even-indexed character removed. The second is two separate lines
that lost half their characters and then ran together, so the line breaks went
the same way.

It is intermittent: eighteen lines came out halved and the rest of the same
listing came out whole. A `[BELLATRIX:RIGEL:PERF]` line appears spliced into
the middle of the run, which is the other producer.

# Where to look

`src/amiga/console.c`. The ring is filled by whoever calls `kprintf` -- the
CPU core, the chipset core on 2, and the guest through the `DEBUG:` handler --
and drained by core 3. Losing exactly half the bytes of one stream is the
shape of two producers claiming the same slots: a read-modify-write of the
write index that is not atomic across cores.

It corrupts diagnostics, not behaviour, and it corrupts them exactly when
something else is talking -- which is when a diagnostic is worth reading.
Found while closing ISSUE-0076.
