---
id: ISSUE-0015
title: "Upstream AROS fixes worth incorporating, and what has been taken"
status: ready
priority: high
type: bug
owner: agent
created_at: 2026-08-07
updated_at: 2026-08-07
tags:
  - aros
  - upstream
  - memory
  - fat
  - boot
blockers:
related_files:
  - external/aros/rom/graphics/allocbitmap.c
  - external/aros/rom/graphics/freebitmap.c
  - external/aros/rom/filesys/fat/ops.c
  - external/aros/workbench/c/iprefs/main.c
  - external/aros/rom/hidds/gfx/gfx_bitmapclass.c
  - external/aros/rom/exec/memory.c
  - external/aros/rom/kernel/tlsf.c
  - AI_context/issues/ISSUE-0007.md
---

# Summary

Our AROS submodule is pinned at `d0370bd757` (2026-07-27). Upstream has moved
**359 commits** since, **145 of them touching code this port runs**. Two of those
are defects verified present in our tree right now, one of which is the strongest
candidate we have for the memory corruption ISSUE-0007 has been unable to
attribute.

This issue is the ledger: what upstream has that we want, what we have taken,
what we deliberately will not take, and what we owe upstream in return.

**Emu68 needs none of this.** It is at upstream HEAD — zero commits since pin
`9b4379a`.

# Why this exists rather than a pin bump

Bumping 359 commits mid-investigation would destroy the measurement baseline
ISSUE-0007 is built on, and most of that traffic is for hardware this machine
does not have (p96gfx, amigavideo, pcixhci, nvme, riscv64, x86_64 SMP). The
repository's mechanism for changing someone else's code is a numbered patch
series, and that is what this uses.

The pin should be bumped eventually, and when it is, every row below marked
**taken** becomes a patch to drop rather than to rebase. That is the point of
tracking them individually.

# How the audit was done, so it can be repeated

```sh
cd external/aros && git fetch origin
git log --oneline d0370bd757..origin/master -- \
    rom/ arch/m68k-all/ compiler/ workbench/libs/muimaster/ \
    workbench/system/Wanderer/ workbench/c/ workbench/libs/graphics/
```

Then, for each candidate, the question is not "does it sound relevant" but
**"is the defective code in our tree"** — checked by reading our file, not by
trusting the commit message. Rows below say which were verified that way.

# Tracking table

| Commit | Date | Subject | In our tree? | Disposition |
|---|---|---|---|---|
| `b553067c52` | 08-04 | graphics: free a bitmap with the size it was allocated with | **verified present** | **TAKEN** — `patches/aros/0013` |
| `647791ec7a` + `921b33af58` | 08-03 | fat: free a lock into its own volume, bound the cluster walk, and the repair that makes it compile | **verified present** | **TAKEN** — `patches/aros/0014` |
| `7416119e73` | 08-03 | iprefs: Detach before the first prefs pass | **verified present** | **TAKEN** — `patches/aros/0015` |
| `e92efb9ee4` | 08-03 | IPrefs: avoid Workbench reset requester during startup | not verified | **TAKEN** — `patches/aros/0016` |
| `f9ccd4078d` | 08-05 | gfx: size framebuffer rows from the mode being entered | **verified present** | **TAKEN** — `patches/aros/0017` |
| `7957a02d64` | 08-05 | exec: name the caller when a free goes wrong | n/a — new code | take (instrument) |
| `5243044724` + `df1305075b` | 08-02/03 | kernel: tlsf corruption reporter, and make it link on m68k | n/a — new code | take partially, see below |
| `c4780bddbd` | 07-28 | dosboot, intuition: boot on NTSC-only display databases | not verified | evaluate |
| `0f85c52a72`, `384f838042` | 07-29..07-30 | fat: BPB parsing shared into a linklib | n/a — refactor | evaluate on pin bump |
| `442d33b4b9` | 08-03 | dos: a short read is not a successful one | — | **already superset**, see below |
| `29a07bec58` | 08-05 | Wanderer: queue a lister's removal only once | present | **skip**, see below |
| `887b06db1c` | 07-30 | dos64: don't corrupt fib_Size in ExamineFH64 | — | **skip** — emul-handler; we use FAT |
| amigavideo, p96gfx, pcixhci, nvme, riscv64, SMP x86_64 | — | — | — | **skip** — no chipset, no such hardware |

**Five taken on 2026-08-07**, as `patches/aros/0013`–`0017`, imported verbatim
and each its own unit of revert. Not yet measured.

## A dependency the first pass missed

`647791ec7a` **does not compile.** Upstream shipped it with an orphaned
continuation line where a diagnostic had been removed:

```c
    if (IsListEmpty(&fl->gl->locks) && fl->gl->node.mln_Succ == NULL)
            fl->gl->dir_cluster, fl->gl->dir_entry);
```

`lock.c:406`, *expected ';' before ')' token*. `921b33af58` is the repair, same
day and same author — and this table filed it under "evaluate on pin bump",
which was wrong. It is folded into `0014` rather than added after it, so no
state of our series fails to build.

Found by building, not by reading. **Verifying that a defect is real is not the
same as verifying that the fix compiles**, and the audit method above only did
the first. Any future row taken from this table gets built before it is
believed.

# The one to take first

## `b553067c52` — AllocBitMap frees less than it allocated

Verified in `rom/graphics/allocbitmap.c`:

```
:332   AllocMem(sizeof(struct BitMap) + sizeof(PLANEPTR) * HIDD_BM_EXTRAPLANES)
:480   FreeMem(nbm, sizeof(struct BitMap))                    <- 32 bytes short
:486   AllocMem(sizeof(struct BitMap) + (depth > 8 ? (depth - 8) * sizeof(PLANEPTR) : 0))
:515   FreeMem(nbm, sizeof(struct BitMap))                    <- short when depth > 8
:534   FreeMem(nbm, sizeof(struct BitMap))                    <- short when depth > 8
```

`HIDD_BM_EXTRAPLANES` is 8 (`rom/hidds/gfx/include/gfx.h:617`), so 32 bytes on
m68k. `freebitmap.c` has the sizes right, which is why this only shows up on the
*failure* paths of AllocBitMap.

Upstream's description of the consequence is our signature, word for word:

> A short free leaves the allocator's chunk boundaries out of step with the real
> allocations, so the damage lands on an unrelated free much later, **as a block
> that belongs to no MemHeader or one that overlaps its neighbour**.

That is what ISSUE-0007 records under "The remaining bad pointers: several sites,
one signature" — damage far from its cause, in consumers with nothing in common.

**What is not established:** that these failure paths run during our boot. They
execute only when creating a bitmap fails, which is plausible on a machine with
no chipset and a display path that has been marginal all along, but plausible is
not measured. This is a **testable prediction**, and the test is the ordinary
one: take it alone, three runs, look for the bad-pointer reports to stop.

# The rest, with what is actually known

## `647791ec7a` — an unsigned compared against zero

`rom/filesys/fat/ops.c:32`, present in our tree:

```c
while (cluster >= 0 && cluster < sb->eoc_mark - 7) {
```

`cluster` is unsigned, so the left half is always true and the walk would free
cluster 0 — the media descriptor. The same commit also fixes FreeLock reaching
the pool through a `glob->sb` that DoDiskRemove nulls by design.

## `7416119e73` and `e92efb9ee4` — IPrefs holds the Startup-Sequence open

Verified in `workbench/c/iprefs/main.c:359-363`:

```c
StartNotifications();
PreparePatches();
HandleNotify();     /* first prefs pass */
Detach();           /* only now */
HandleAll();
```

Upstream: *"IPrefs held the Startup-Sequence open until it had applied every
preference. The font pass can sit in a requester loop waiting for the user to
close windows so the Workbench screen can be reset, and nothing later in the
sequence runs until it gives up."*

**This is the shape of a failure this issue's parent has recorded repeatedly**
— IPrefs being the last thing the Startup-Sequence reports before the boot goes
quiet, which is why `AI_context/codex-2026-08-05` carried IPrefs tracing at all.
`e92efb9ee4` removes the requester that causes the loop; `7416119e73` stops the
sequence from waiting on it either way. They address the same failure from both
ends and should go together.

## `f9ccd4078d` — bytesPerRow computed from the mode being left

Verified in `rom/hidds/gfx/gfx_bitmapclass.c:4433-4434`:

```c
data->bytesPerRow = GetBytesPerRow(data, CSD(cl));
data->prot.pixfmt = pixfmt;
```

`GetBytesPerRow()` reads `data->prot.pixfmt`, which is still the old one. Rows
end up sized for the previous depth and `ConvertPixels()` then reads several
rows of pixels per row of output. ISSUE-0007 has a section titled "The screen
size was wrong, and that is fixed" — this is a second, independent way for the
same symptom to occur, and it is on the chunky path this port uses.

## `5243044724` + `df1305075b` — take the checks, not the backtrace

Upstream added to `rom/kernel/tlsf.c`: free-list metadata validation at
`REMOVE_HEADER` and `INSERT_FREE_BLOCK`, double-free detection, and
`FREE outside TLSF area` via `tlsf_block_in_area()` — the same area-list walk as
our `patches/aros/0010`. `df1305075b` exists specifically to make it link in
m68k kickstart modules.

**The backtrace half is dead weight here.** It calls `KrnBacktraceFromFrame`,
which has no m68k implementation (`rom/kernel/backtracefromframe.c` returns 0),
and writing one would not help: this kernel builds with `-Os` and carries no
frame pointers at all — 23 functions in `tlsf.o`, not one `linkw %fp`. That is
the finding behind `patches/aros/0012`, which gets the caller from
`__builtin_return_address(0)` at `FreeVecPooled` instead, the one level that
needs no frame.

So: take the checks, leave the backtrace calls out, and revisit if the build
ever keeps frames.

# Deliberately not taken

- **`442d33b4b9`** — upstream found the same `elf_read_block` defect we did, and
  **their fix does not cover the case we measured.** Their cache-hit test still
  bounds by `LOADSEG_SMALL_READ`; only the fill is checked. Replay our marker
  against their code: `srb_FileOffset=5688` with 1904 valid bytes, request
  `off=7412 size=400` — `7412 >= 5688` and `7812 <= 9784`, so it is a *hit*, and
  `CopyMem` from `+1724` for 400 bytes reaches 2124 against 1904 valid. Still 220
  bytes of uninitialised memory, exactly the number recorded in ISSUE-0007. Our
  `patches/aros/0009` bounds by `srb_Valid` and closes it. We also have their
  NULL-AllocMem guard.
- **`29a07bec58`** — real defect (a lister closed twice disposes the window twice,
  freeing every field it owns twice), but it needs a *second click* to close a
  lister. An unattended boot does not click.
- **`887b06db1c`** — `ExamineFH64` against a filesystem without
  `ACTION_GET_FILE_SIZE64`; the commit names emul-handler. We use FAT.

# What we owe upstream

Two things, both found here and absent there:

1. **The FAT byte order.** No `AROS_WORD2LE` work in `rom/filesys/fat/` since our
   pin — upstream still writes `first_cluster`, `file_size` and the date fields
   in host order. `patches/aros/0008` and `0011` are ours to contribute, and the
   defect affects every big-endian target: m68k-amiga, ppc-native, ppc-morphos.
2. **The hole in `442d33b4b9`**, above, with our marker as the reproduction.

# Acceptance criteria

- [x] `b553067c52` taken — `patches/aros/0013`
- [x] `647791ec7a` taken, with `921b33af58` — `patches/aros/0014`
- [x] `7416119e73` and `e92efb9ee4` taken together — `0015`, `0016`
- [x] `f9ccd4078d` taken — `patches/aros/0017`
- [ ] the five measured: distribution rebuilt, card regenerated, three runs
- [ ] if the rate moves, bisect the five — they are separate patches for this
- [ ] `7957a02d64` taken
- [ ] tlsf checks taken without the backtrace calls
- [ ] `c4780bddbd` evaluated against our display database
- [ ] FAT byte order offered upstream
- [ ] the `442d33b4b9` cache-hit hole reported upstream
- [ ] this table re-run against a fresh `git fetch` before any pin bump

# Execution log

- 2026-08-07 — audit performed against `origin/master` at 359 commits past pin
  `d0370bd757`. Four defects verified present in our tree by reading our own
  files: the bitmap short free, the unsigned cluster walk, the IPrefs detach
  ordering, and the bytesPerRow ordering. Emu68 needs nothing — it is at
  upstream HEAD. Nothing taken yet.
- 2026-08-07 — five taken as `patches/aros/0013`-`0017`. `647791ec7a` did not
  compile: upstream left an orphaned continuation line at `lock.c:406`, and
  `921b33af58` is its repair, filed in this table as "evaluate on pin bump".
  Folded into `0014`. The lesson is narrow and worth keeping: this audit
  verified that each defect was real in our tree, which is not the same as
  verifying the fix builds. Nothing measured yet.
