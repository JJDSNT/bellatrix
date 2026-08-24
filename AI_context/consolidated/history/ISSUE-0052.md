---
id: ISSUE-0052
title: "xSysInfo cannot allocate from its own MemHeader, and the program is not what should change"
status: done
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-23
updated_at: 2026-08-24
tags:
  - exec
  - memory
  - alignment
  - compatibility
  - m68k
blockers:
related_files:
  - external/aros/rom/exec/memory_nommu.c
  - external/aros/rom/exec/memory.c
  - external/aros/compiler/include/exec/memory.h
  - tests/sysinfo/xSysInfo
  - AI_context/issues/ISSUE-0037.md
  - AI_context/issues/ISSUE-0051.md
---

# Summary

`xSysInfo FULL` starts and dies before printing anything:

    Error: 0x8100000C - Sanity check on memory list failed
    Task : 0x04233ABC - xSysInfo
    Module xSysInfo Segment 0 - offset 0x000175CE
    In Allocate, size 16
    MemHeader 0x045ee534 (0x045ee554 - 0x045f2534)
    - Unaligned first chunk address (0x045ee554)

The program builds a private `MemHeader` and calls `Allocate()` on it, which
is ordinary Amiga practice. Its pool starts at `0x045ee554` — four-byte
aligned, which is `AROS_WORSTALIGN` on m68k — and Exec rejects it.

**The fix is not to patch xSysInfo.** Bellatrix's point is to be compatible
with the Amiga: a correct third-party m68k binary has to run as it is
shipped, and the moment the answer becomes "recompile the program" the
machine has stopped being the thing it is trying to be. So this issue is
about Exec, not about xSysInfo. xSysInfo is only the first program to have
noticed.

# Where it fires

`rom/exec/memory_nommu.c:172`, inside `#if !defined(NO_CONSISTENCY_CHECKS)`:

```c
if (((IPTR)p2|(IPTR)p2->mc_Bytes) & (MEMCHUNK_TOTAL-1))
{
    if (SysBase && SysBase->DebugAROSBase)
    {
        ...
        Alert(AN_MemoryInsane|AT_DeadEnd);
    }
    break;
}
```

Two things follow. With a debug SysBase it is a dead-end alert, which is what
we see. Without one it silently `break`s out of the chunk walk, so the
allocation simply fails — quieter, equally incompatible.

`rom/exec/memory.c:85` is the reporter that prints the `Unaligned first
chunk address` line; it is not the test.

# Why 4 is not aligned enough

    MEMCHUNK_TOTAL = max(AROS_WORSTALIGN, sizeof(struct MemChunk))

On m68k that is `max(4, 8)` = **8**, while `AROS_WORSTALIGN` is **4**. So
"aligned" means two different things depending on who is asking, and a
program that aligns to the target's own documented worst-case alignment is
rejected by the allocator of that same target.

On AmigaOS the same `MemHeader` works. Exec there does the arithmetic and
does not audit the caller's chunk alignment, so a pool at a 4-byte boundary
allocates and frees normally.

**This is the same seam for the fourth time in this port**, and it is worth
listing because each occurrence looked like an unrelated bug:

1. TLSF handed out 4-byte blocks that could not hold an 8-byte free node —
   [ISSUE-0037](ISSUE-0037.md), fixed by `patches/aros/0011`;
2. the FAT cache aligned its DMA buffers to whatever a header left, and the
   SD driver needs 32 — `patches/aros/0038`;
3. `MEMCHUNK_TOTAL` 8 against `AROS_WORSTALIGN` 4 in `rom/exec/memory.c:85`,
   noted while chasing [ISSUE-0051](../../issues/ISSUE-0051.md);
4. this.

# What is not yet decided

Three directions, none of them costed:

- **Tolerate what the caller gave.** Round `mh_First` up to `MEMCHUNK_TOTAL`
  on entry rather than refusing it, and stop treating a caller's alignment as
  a corruption signature. Closest to what AmigaOS actually does, and it does
  not change the ABI. It does mean the consistency check can no longer tell a
  caller's harmless alignment from real heap corruption, which is what that
  check is there to catch — so whatever replaces it has to still catch
  corruption.
- **Raise `AROS_WORSTALIGN` to 8 on m68k.** Makes the two definitions agree
  everywhere at once. It is also the ABI of every m68k binary already built
  for this target, and it costs memory on every small allocation, so it is
  not a local change.
- **Make the check non-fatal and let the allocation proceed.** Smallest
  change, and the one most likely to hide a real defect later.

The first is the most likely answer; it has not been tried.

# Why it matters beyond one program

`sdcard.md` sec.11 lists xSysInfo as a data point for the card measurements,
so this blocks a measurement we want. But the reason to fix it is not the
measurement: any Amiga program that manages its own memory pool — and that is
a common enough pattern — meets this on the first `Allocate()`.

# Resolved 2026-08-24

`patches/aros/0055`. `xSysInfo FULL` runs to its full report -- hardware,
caches, and the library list with versions and addresses -- from the shipped
binary with no changes to it.

The fix is the one this issue recommended, applied in three places rather than
one, because the same conflation appears three times:

| where | was | now |
|---|---|---|
| `memory_nommu.c` `stdAlloc` | address and size both vs `MEMCHUNK_TOTAL` | address vs `AROS_WORSTALIGN`, size vs `MEMCHUNK_TOTAL` |
| `memory.c` `validateHeader` | `mh_First` vs `MEMCHUNK_TOTAL` | vs `AROS_WORSTALIGN` (`mh_Free` unchanged) |
| `memory.c` `validateChunk` | `mc_Next` vs `MEMCHUNK_TOTAL`; gap >= `MEMCHUNK_TOTAL` | `mc_Next` vs `AROS_WORSTALIGN`; gap >= `AROS_WORSTALIGN` |

The third took a second look to get right. Rule 3 required at least
`MEMCHUNK_TOTAL` **allocated** bytes between two free chunks -- but allocated
bytes never have to hold a `MemChunk`. The smallest gap that can exist is the
smallest allocation the target can make, which is its worst-case alignment.
xSysInfo produced a legitimate 4-byte gap and it was called corruption. The
upper bound still uses `MEMCHUNK_TOTAL`, because the last free chunk does have
to hold one.

## What this cost, methodologically

Two mistakes worth recording, both of the same kind -- believing a result
before the method could produce it.

**The first fix changed the message, not the decision.** `memory.c:85` is the
reporter that formats the alert; `validateHeader` 350 lines below is what
raises it. Fixing only the reporter made the "Unaligned first chunk address"
line disappear while the alert stayed, which looked like partial progress and
was actually a diagnostic that had stopped telling the truth.

**And a boot test that reports to `DEBUG:` reported nothing.** `Echo >DEBUG:`
produced no serial output at all, across two runs of two different boot tests.
A run was read as "the crash is gone" when the crash simply could not have
been printed.

That turned out to have two causes, both since fixed, and neither of them a
reason to stop using `DEBUG:`:

- `DEVS:DOSDrivers/DEBUG` names `L:debug-handler`, and nothing built it. The
  target `workbench-fs-debug` existed and compiles first time; it is in
  `build-aros.sh` now.
- `make-sdcard.sh` inserted boot tests after `Assign "IMAGES:"`, which is line
  30 of `S:Startup-Sequence`, while `DEVS:DOSDrivers` is mounted on line 53.
  The probe ran twenty lines before the device it wrote to existed. The anchor
  is now immediately after the `Mount`, which is still long before Wanderer.

With both, `DEBUG:` reaches the serial line and `tests/gl/sysinfo` reports
there like the other six probes. Seven scripts had been writing into nothing.

# Verification

Done. `xSysInfo FULL` reaches its report from the unmodified binary, and a
plain boot still reaches `hold released: icons` at 1:03 -- unchanged from
baseline, which matters because this patch touches the Exec allocator's
validator.
