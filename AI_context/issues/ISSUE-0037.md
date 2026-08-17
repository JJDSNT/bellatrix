---
id: ISSUE-0037
title: "A CLI task dies on a corrupt block header once the preferences actually load"
status: doing
priority: high
type: bug
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - memory
  - tlsf
  - boot
  - stability
blockers:
related_files:
  - external/aros/rom/kernel/tlsf.c
  - AI_context/issues/ISSUE-0036.md
  - patches/aros/0011-kernel-avoid-undersized-tlsf-free-blocks.patch
  - patches/aros/0007-kernel-refuse-to-free-a-pointer-outside-the-heap-and.patch
---

# Summary

With `ISSUE-0036` fixed, `ENV:` is populated and the Startup-Sequence runs the
work it has been skipping since this port existed. A CLI task then dies:

```
[Kernel:TLSF] free-list corruption at REMOVE_HEADER: mhe=02000000
  requirements=0x00000000 tlsf=02000058 bucket=19/0 block=00000000 size=0
  flags=0x0 head=00000000 prev=00000000 next=00000000 task=02296dc8
[Kernel:TLSF] Backtrace (0 frames):
```

and on screen, from the same task:

```
Software Failure!   Task: 0x02296DC8 - CLI
Error: 0x80000027 - Unknown CPU error    PC: 0x3461F4EE
```

The task ID matches in both, so this is one event, not two.

**The desktop survives it.** Wanderer comes up, the Ice theme renders, the icons
are there; the requester sits on top of a working screen. That is a change of
kind from 2026-08-06, when the same region of behaviour killed the boot 13 times
out of 13 in silence.

# What the report actually says

`block=00000000` — `REMOVE_HEADER()` was handed a **NULL block**. Only one of
its four call sites can produce that:

```c
static inline bhdr_t * MERGE_PREV(struct MemHeaderExt *mhe, tlsf_t *tlsf,
    bhdr_t *block)
{
    if (FREE_PREV_BLOCK(block))              /* tlsf.c:623 — the flag says yes */
    {
        bhdr_t *prev = block->header.prev;   /* tlsf.c:626 — and this is NULL  */
        MAPPING_INSERT(GET_SIZE(prev), &fl, &sl);
        REMOVE_HEADER(mhe, tlsf, prev, fl, sl);   /* tlsf.c:632 */
```

The other three cannot: `tlsf.c:560` is guarded by `if (!b) return NULL` four
lines earlier, and `:657`/`:979` derive their block by arithmetic from a
non-NULL one.

So the block being freed has `PREV_FREE` set in its flags while its
`header.prev` is zero. **Its header is corrupt** — the two fields disagree, and
one of them was written by something other than TLSF. `bucket=19/0` is then
meaningless: it is `MAPPING_INSERT(GET_SIZE(NULL))`, i.e. whatever lies at
address 0 read as a size.

`head=00000000` says `matrix[19][0]` is empty as well, which is consistent with
the bucket being fabricated rather than with a desynchronised bitmap.

That points at a **write past the end of the preceding allocation** — the
classic way for the following block's header to be clobbered — or at a free of
something that was never a TLSF block. It does *not* point at TLSF's own
bookkeeping, which is worth saying because that is where the eye goes first.

# Why this is a new issue and not a reason to revert ISSUE-0036

The FAT read path is either correct or it is not. What `0023` changed is that
`ParentDir()` now works below two levels, so code that could not run before now
runs: `Copy` populates `ENV:`, every `If EXISTS "ENV:..."` in the
Startup-Sequence takes its true branch, and `C:Decoration`, IPrefs and the Zune
preference readers do real work with real allocations. This defect was always
there and was unreachable.

`patches/aros/0006`'s own message predicted exactly this, in the other
direction: *"making it succeed wakes a code path that has not run here"*.

# Frequency

Intermittent, which matters for how this gets chased.

**Once in four runs.**

| run | reached takeover | TLSF corruption | screen |
|---|---|---|---|
| 1 | yes | **yes** | themed desktop **+ Software Failure requester** |
| 2 | yes | no | themed desktop, clean |
| 3 | yes | no | themed desktop, clean |
| 4 | yes | no | themed desktop, clean |

All four reached the Wanderer desktop with the Ice theme rendered, so the
failure costs a requester and a dead CLI, not the boot.

Runs are ~170 s headless under QEMU on an idle host, one at a time, runs 2-4 on
a `snapshot=on` copy of the same card so each starts from identical bytes. An
intermittent memory defect is the shape of a race or of an
allocation-size-dependent overrun, not of a deterministic off-by-one on a fixed
path.

# What to do, cheapest first

1. **Name the command.** The failing task is a CLI, which almost certainly means
   the boot shell running the Startup-Sequence. Bisect it the way ISSUE-0036 was
   bisected: instrument the sequence on a private card copy so each line
   announces itself to `SDCARD0P0:`, and read which one is last before the
   alert. One boot per attempt, no rebuild.
2. **Get a backtrace.** `KrnBacktraceFromFrame()` returned **0 frames**, so the
   most useful line in the report is empty. Find out why before spending runs on
   guesses — a working backtrace turns this from a search into a lookup.
3. **Only then look at the allocator.** The evidence says a corrupt header, not
   a corrupt free list. Instrumenting TLSF further will describe the victim
   again, not the culprit.

# Notes

**This is inside the standing freeze.** Nothing is being added; a desktop that
comes up with a Software Failure requester on it is not a stable desktop.

**Do not merge this with the older TLSF work.** Patches 0007, 0009 and 0011
guard the *reporting* of this family of defects — refusing a free outside the
heap, naming the caller, avoiding undersized free blocks. This is why this
failure has a name instead of being a silent death, and it is not evidence that
any of them is wrong.

# Execution log

- 2026-08-17 — Opened. Seen on the first boot after `patches/aros/0023` landed
  and on none of the next three. Call site narrowed to `MERGE_PREV()` by
  elimination — the NULL block can only come from `block->header.prev` at
  `tlsf.c:626`.
