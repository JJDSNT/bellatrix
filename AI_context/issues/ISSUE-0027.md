---
id: ISSUE-0027
title: "AROS TLSF: a live free-list corruption, and what Emu68's copy is worth"
status: backlog
priority: high
type: bug
owner: unassigned
created_at: 2026-08-16
updated_at: 2026-08-16
tags:
  - kernel
  - tlsf
  - memory
  - emu68
  - upstream
blockers:
related_files:
  - external/aros/rom/kernel/tlsf.c
  - external/emu68/src/tlsf.c
  - patches/aros/0011-kernel-avoid-undersized-tlsf-free-blocks.patch
  - patches/aros/0007-kernel-refuse-to-free-a-pointer-outside-the-heap-and.patch
  - AI_context/consolidated/history/ISSUE-0007.md
---

# Summary

Two things, and only one of them is what the question assumed.

**A live defect.** AROS's own free-list check fires on a Pi 3 during Bluetooth LE
scanning:

```
[Kernel:TLSF] free-list corruption at REMOVE_HEADER: mhe=02000000
  requirements=0x00000000 tlsf=02000058 bucket=19/0 block=00000000 size=0
  flags=0x0 head=00000000 prev=00000000 next=00000000 task=025912a4
[Kernel:TLSF] Backtrace (0 frames):
```

It reproduced across several boots on 2026-08-16, always around the LE scan, and
the boot survived it. `block=00000000` with `head=00000000` says `REMOVE_HEADER`
was reached for bucket 19/0 with an empty list, so either a block was removed
twice or a bucket index was computed from a size that is not the block's.

**Emu68's TLSF is not a source of improvements — it is behind ours.** The two are
the same implementation: identical `MAX_LOG2_SLI`, `MAX_FLI`, `FLI_OFFSET`,
`SMALL_BLOCK`, `HEADERS_SIZE`, same matrix, same bitmaps. Emu68's is 872 lines to
AROS's 1597 because AROS carries the `MemHeaderExt` integration, aligned
allocation, realloc, the area merge and the corruption reporting on top.

# What Emu68 has that we do not: nothing we lack

Checked function by function. `tlsf_malloc`, `tlsf_malloc_aligned`, `tlsf_free`,
`tlsf_realloc`, `tlsf_add_memory`, `tlsf_init`, `tlsf_init_with_memory`,
`tlsf_set_flags` — every one has an AROS counterpart
(`rom/kernel/tlsf.c:704, 912, 1218, 1271`), and AROS additionally has
`tlsf_add_memory_and_merge` and the integrity reporting Emu68 has no equivalent
of.

# What AROS has that Emu68 could use

The comparison is one-directional, so it is worth writing down in that
direction. Counted occurrences in each file:

| | Emu68 | AROS |
|---|---|---|
| the split fix (`free_node_t` room, not just `hdr_t`) | absent | `tlsf.c:563` |
| free-list validation (`tlsf_valid_bucket`) | 0 | 4 |
| block-in-area validation (`tlsf_block_in_area`) | 0 | 9 |
| corruption reporting, reported once (`TLSFF_CORRUPTREPORTED`) | 0 | 3 |
| `tlsf_add_memory_and_merge` | absent | `tlsf.c:1271` |

The validation is the part worth offering. Emu68's allocator runs on the host
side under a JIT, where a corrupted free list produces a fault with no
attribution at all; the AROS version answers *which* bucket, *which* block and
*which* task, which is the difference between a bug report and a mystery. It
costs a bounds check on a path that is already failing, and it prints once.

`tlsf_add_memory_and_merge` matters only if Emu68 ever grows a heap in pieces
that are contiguous, which it currently does not. Listed for completeness.

# The flow runs the other way, and that is the actionable part

`patches/aros/0011` fixed a real corruption in AROS: `tlsf_malloc()` split a
block whenever the remainder had room for a `hdr_t`, which on a 32-bit target
leaves a four-byte payload, and `INSERT_FREE_BLOCK` then writes its eight-byte
`free_node_t` across the following block's header. That patch ended
[`ISSUE-0007`](../consolidated/history/ISSUE-0007.md) after ten days.

**Emu68 still has that bug**, verbatim:

```c
emu68/src/tlsf.c:354       if (likely(GET_SIZE(b) > (size + ROUNDUP(sizeof(hdr_t)))))
aros/rom/kernel/tlsf.c:563 if (likely(GET_SIZE(b) >= (size + ROUNDUP(sizeof(hdr_t)) +
                                                      ROUNDUP(sizeof(free_node_t)))))
```

Emu68's allocator serves the JIT and its own structures, not the guest, so the
consequence is not the same — but it is the same defect, and we are the only
people who know.

# Goal

The corruption stops, and whichever project is behind gets the other's fix.

# What is left

1. **Find the corruption.** `REMOVE_HEADER` with an empty bucket is a
   double-remove or a bucket computed from the wrong size. It appears under BT
   scanning, which is the newest allocation traffic in the tree, so start by
   asking whether that path frees twice rather than assuming the allocator.
   `patches/aros/0007` already refuses out-of-heap frees and names the caller; a
   similar guard on double-remove would name this one.
2. **Decide whether the reporting should abort.** It currently reports once
   (`TLSFF_CORRUPTREPORTED`) and continues, which is right for an instrument and
   wrong once the cause is known: continuing means allocating from a list that
   has already been shown to be wrong.
3. **Offer patch 0011 to Emu68.** One line, and it is our own diagnosis of a
   defect their copy still carries.
4. **Offer the validation too, separately.** `tlsf_valid_bucket`,
   `tlsf_block_in_area` and the report-once flag are independent of the split
   fix and useful on their own -- more so on the Emu68 side, where a corrupted
   free list currently faults without naming anything. Send it as its own change
   so the one-line fix is not held up by a larger one.
5. **Consider whether anything should flow the other way at all.** On this
   reading, no. Worth re-checking if Emu68's allocator diverges later.

# Decisions taken

**Do not port Emu68's TLSF into AROS.** It is the same code minus the parts we
need, and it lacks the fix we already carry. Any work here is on AROS's copy.

# Acceptance criteria

- [ ] The `free-list corruption at REMOVE_HEADER` no longer reproduces on a Pi 3
      under BT scanning
- [ ] The cause is named -- a caller, not "the allocator"
- [ ] Emu68 has been offered the split fix, or the difference is recorded as
      deliberate
- [ ] Emu68 has been offered the free-list validation, separately from the fix

# Notes

**The corruption survived the boot, which is the dangerous part.** AROS reports
and continues, so the machine kept running with a free list it had just declared
invalid. Whatever this turns out to be, it has been silently corrupting memory
on every boot that scanned.

**It appears alongside a framing bug that is now fixed.** The same logs carried
impossible HCI packets caused by UART overrun (`ISSUE-0019`, fixed by draining
the FIFO and ticking at 1 ms). If the corruption came from garbage lengths
reaching an allocation path, it may already be gone -- **verify before
investigating**, because chasing a fixed bug is the expensive failure mode here.

# Execution log

- 2026-08-16 — Opened after the corruption appeared repeatedly in Bluetooth
  bring-up logs on hardware. Compared the two TLSF copies line by line: same
  lineage, same tuning constants, and Emu68 carries the exact bug our patch 0011
  fixed. The premise of the question -- that Emu68's copy has something to teach
  ours -- did not survive the comparison.
