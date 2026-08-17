---
id: ISSUE-0038
title: "FAT's on-disk fields are typed as if they were native, so every endian mistake compiles clean"
status: backlog
priority: medium
type: hardening
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - fat
  - endianness
  - upstream
  - maintainability
blockers:
related_files:
  - external/aros/rom/filesys/fat/fat_struct.h
  - external/aros/rom/filesys/fat/fat_fs.h
  - patches/aros/0023-fat-read-the-first-cluster-fields-little-endian.patch
  - AI_context/issues/ISSUE-0036.md
---

# Summary

`rom/filesys/fat` is now endian-**correct** — `ISSUE-0036` and patches 0006,
0008 and 0023 closed the last of it, field by field. It is not endian-**safe**.

Correctness rests entirely on every author remembering `AROS_LE2WORD` /
`AROS_WORD2LE` at every access site, and the compiler cannot help, because on a
little-endian host **every mistake is a no-op**. That is not a hypothetical
failure mode: it is exactly how the `GetDirEntryByCluster()` comparison survived
for two decades across four big-endian targets. It read correctly to everyone
who ever looked at it on x86.

# The lie is in the type

```c
struct FATDirEntry
{
    ...
    UWORD first_cluster_hi;
    UWORD first_cluster_lo;
    ULONG file_size;
} __packed;
```

`first_cluster_lo` is not a `UWORD`. It is *two bytes in little-endian order*.
While the declaration says otherwise,

```c
if (de->e.entry.first_cluster_lo == (cluster & 0xffff))
```

compiles without a warning on every host and is wrong on half of them.

# What would make it safe

Distinct types the compiler refuses to mix with native integers:

```c
typedef struct { UWORD v; } le16;    /* on-disk, little-endian */
typedef struct { ULONG v; } le32;

#define LE16(f)     AROS_LE2WORD((f).v)
#define TO_LE16(x)  ((le16){ AROS_WORD2LE(x) })
```

With the on-disk structs declared in those types, a raw comparison or a raw
assignment **fails to build** — on a little-endian developer machine as loudly
as on a big-endian target. Every access is forced through the conversion, and
the class of defect stops being invisible where it is usually written.

That is the property that matters. Accessor functions alone (`DE_FirstCluster(de)`)
would be an improvement in style and would not prevent anything, because
reaching past them still compiles.

# Scope, which is better than it looks

`fat_struct.h` is included by exactly one file — `fat_fs.h` — which is used only
inside `rom/filesys/fat`. Nothing outside the handler sees these structs: the
partition code and massstorage go through `compiler/fatbpb`, which is byte-wise
and endian-free already. So the blast radius is one directory.

**98 access sites to multi-byte fields, in six files:**

| file | sites |
|---|---|
| `volume.c` | 43 |
| `ops.c` | 21 |
| `direntry.c` | 14 |
| `names.c` | 7 |
| `fat.c` | 3 |
| `lock.c` | 1 |

The byte-wide fields (`name[]`, `attr`, `nt_res`, `create_time_tenth`,
`bpb_media`, the signatures) stay as they are; they have no byte order.

# The real cost is the patch, not the edit

Writing it is mechanical. Carrying it is not: 98 touched sites is a large
conflict surface against every future bump of the AROS pin, and this repository
vendors upstream and never edits it in place.

Two honest ways out:

1. **Upstream it.** It fixes the same latent defect for `m68k-amiga`,
   `ppc-native` and `ppc-morphos`, so it belongs there rather than here.
   Upstreaming is explicitly not the path this project is taking right now
   (see the parked branches, 2026-08-17), so this waits on that decision
   changing.
2. **Keep it local and pay the conflicts.** Only worth it if the pin stops
   moving.

Neither is urgent, which is why this is `backlog` and not `doing`.

# Notes

**This is not a bug report.** Nothing is known to be wrong in the handler today;
`ISSUE-0036` records the audit that established that. This is about the next
mistake, not the last one.

**If it is ever done, it goes in its own patch**, separate from `0023`. The
defect fix and the type hardening have to be revertible independently — `0006`
is the cautionary tale for bundling a measurement with a change.

**Under the standing freeze this is not an addition** — it adds no
functionality — but it is also not required for the system to be fast or
stable, which is the bar right now.

# Execution log

- 2026-08-17 — Recorded at the user's request while closing `ISSUE-0036`. The
  question that prompted it: having fixed the endianness, is the handler now
  robust? It is not, and the reason is in the type declarations rather than in
  any line of logic.
