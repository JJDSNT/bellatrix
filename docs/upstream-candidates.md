# Five changes we would like to send upstream

Five patches from `patches/aros/` that are not about our target. Each is a
defect in code shared by every AROS port, or a value that makes m68k an
outlier for no reason we can find. None mentions `m68k-emu68`; none depends on
another; each applies to upstream HEAD on its own.

**Section 5 is the one to read first if you only read one.** It is a heap
corruptor in `rom/kernel/tlsf.c`, reachable on m68k and only on m68k, and it
cost this project ten days.

They are written up here so they can be reviewed before being offered, and so
the reasoning survives if the answer is no.

Context, because it affects how much weight to give the evidence: these were
found while bringing AROS/m68k up on a Raspberry Pi under Emu68, a machine
with no Amiga chipset and 840 MB of RAM. That is an unusual environment and it
exposes things a normal Amiga does not. Where the argument depends on this
being unusual, it says so.

Verified against upstream HEAD `85705361ca`: the first four on 2026-08-13, the
fifth on 2026-08-14. All are still present.

---

## 1. `sdcard`: `sdcu_SoftList` is used before it is a list

**Declared:** `rom/devs/sdcard/sdcard_unit.h:51`
**Used:** `rom/devs/sdcard/sdcard_device.c:393`, `AddHead(&unit->sdcu_SoftList, ...)`
**Initialised:** nowhere
**Our patch:** `0003-sdcard-initialise-sdcu-softlist-before-addhead-uses.patch`,
which adds the `NEWLIST` in `sdcard_bus.c`'s `RegisterUnit()`, where the unit
is allocated

`AddHead()` needs the list to be a list first. The driver's only two `NEWLIST`
calls are for `timermp.mp_MsgList` and `LIBBASE->sdcard_Buses`; the per-unit
list never gets one.

`rom/devs/ata` does the same `NEWLIST` for the identically named `au_SoftList`
in `ata_unitclass.c`. The call looks to have been lost when the sdcard driver
was derived from the ata one.

**Why it has not bitten before.** On a zeroed list, `AddHead()` writes the node
through a NULL `lh_Head` — that is, to address 4. On the ARM ports that have
used this driver so far, address 4 is unremarkable memory. On m68k it is
`AbsExecBase`, so the write destroys the pointer every library call in the
system goes through.

**The fix** is the missing `NEWLIST`, in the same place `ata` does it.

The latent corruption is there for every user of the driver; only the
consequence is port-specific.

---

## 2. m68k's default task stack is a quarter of everyone else's

**File:** `arch/m68k-all/include/aros/cpu.h`
**Our patch:** `0005-raise-the-m68k-default-task-stack-to-match-the-other.patch`

`AROS_STACKSIZE` is 16384 on m68k. Every other target in the tree is larger:
i386, ppc, riscv and x86_64 use 40960; MorphOS uses 32768. That makes m68k the
outlier by a factor of two and a half.

**This one has two arguments and they should be judged separately.**

The weaker one is the number. 16 KB is a figure from machines with 512 KB of
RAM. It is defensible on a real Amiga and indefensible on a target with
hundreds of megabytes, but "defensible on a real Amiga" is exactly the case
upstream has to keep working, so we do not press it.

The stronger one is that it is measurably not enough. With a frame validator in
the scheduler, `WANDERER:Wanderer` was caught writing its persistent task frame
**below `tc_SPLower`** — outside its own stack — 97 times in a single boot.
Everything the scheduler then saved and restored from that frame belonged to
something else, which is why pointers read back out of it came back as
fragments of unrelated strings.

Raising the stack took validator reports from 97 in one run to zero across
eight, and runaway exception recursion from one to none.

**We landed on 40960**, not on the 64 KB the test used, because agreeing with
the other targets is a better argument than any number chosen for our port.

**Honest limits.** It does not change our boot success rate; that was measured
and said so. And a stack overrun is a symptom — something is using more stack
than the default allows, and raising the default hides that rather than
answering it. We think matching the other targets is right on its own merits,
but it should not be mistaken for a fix to whatever consumes the stack.

---

## 3 and 4. FAT directory entries are little-endian on disk

**Files:** `rom/filesys/fat/direntry.c`, `ops.c`, `lock.c`, `date.c`
**Our patches:** `0006-fat-write-cluster-and-size-little-endian.patch`,
`0008-fat-convert-directory-dates-little-endian.patch`

A FAT directory entry is little-endian on disk regardless of the host. The read
path knows this — `FIRST_FILE_CLUSTER` in `fat_fs.h` is

```c
#define FIRST_FILE_CLUSTER(de) \
    (AROS_LE2WORD((de)->e.entry.first_cluster_lo) | \
     (((ULONG) AROS_LE2WORD((de)->e.entry.first_cluster_hi)) << 16))
```

— but several other places do not.

### 3. The write path stores native

Cluster numbers and file sizes are written to the directory entry without
conversion. On a little-endian host this is invisible. On a big-endian host,
every file the handler creates gets an unreadable entry: wrong size, and a
first cluster that points somewhere else entirely.

The fix converts on the way out, with `AROS_WORD2LE` on each 16-bit half of the
cluster and `AROS_LONG2LE` on the size.

### 4. `ConvertFATDate` / `ConvertDOSDate`

The same defect in the date and time fields. These two functions are the only
producers and consumers of those fields, so the conversion goes **inside them**
rather than at the call sites — one place to be right, and no call site can
forget.

### Two more sites we have not patched

Found while checking the above and left alone because neither is on our boot
path, but both are the same defect:

- `rom/filesys/fat/ops.c` line 599 (`Rename`): builds a cluster number as
  `(sde.e.entry.first_cluster_hi << 16) | sde.e.entry.first_cluster_lo` from
  the raw fields, with no `AROS_LE2WORD`.
- `rom/filesys/fat/direntry.c` line 242: compares the raw little-endian fields
  against a native cluster number, which on a big-endian host simply never
  matches.

Whoever takes the two patches above may want these in the same pass.

**Why this has not bitten before.** AROS's FAT handler is presumably used mostly
on little-endian hosts, where none of it matters. It is only visible on
big-endian, and there it is not subtle: a card written by the handler is
corrupt, and rereading it produces garbage that looks like data.

---

## 5. TLSF splits a block into a free node too small to hold its own links

**File:** `rom/kernel/tlsf.c`, `tlsf_malloc()`
**Our patch:** `0011-kernel-avoid-undersized-tlsf-free-blocks.patch`

A free block in TLSF carries a `free_node_t` — two pointers — in its payload,
written by `INSERT_FREE_BLOCK` when the block joins a free list. The split test
in `tlsf_malloc()` only asked whether the remainder had room for the *header*:

```c
if (likely(GET_SIZE(b) > (size + ROUNDUP(sizeof(hdr_t)))))
```

That guarantees the remainder is non-empty. It does not guarantee it can hold a
`free_node_t`. When the remainder comes out smaller than two pointers,
`INSERT_FREE_BLOCK` writes the second pointer **past the end of the block it
belongs to, across the following block's header** — overwriting that block's
`prev` link and its length-and-flags word.

The result is a free list that points into the middle of a live allocation. It
is silent when it happens and fatal some unbounded time later, in whatever code
next walks the chain.

**The fix** requires room for both:

```c
if (likely(GET_SIZE(b) >= (size + ROUNDUP(sizeof(hdr_t)) +
                           ROUNDUP(sizeof(free_node_t)))))
```

### Why this has not bitten before: it is reachable on m68k alone

`SIZE_ALIGN` is `AROS_WORSTALIGN`, and m68k is the only maintained target where
that is not 16:

| Target | `AROS_WORSTALIGN` |
|---|---|
| i386, x86_64, ppc, arm, aarch64, riscv, riscv64 | 16 |
| ppc-morphos | 8 |
| **m68k** | **4** |

Every payload is a multiple of `SIZE_ALIGN`, so the smallest non-zero remainder
a split can leave is `SIZE_ALIGN` itself. `sizeof(free_node_t)` is 8 on a 32-bit
target and 16 on a 64-bit one. So the remainder is large enough by construction
everywhere `SIZE_ALIGN` is 16, and on ppc-morphos at 8 it is exactly large
enough. On m68k, a 4-byte remainder is reachable and is half of what
`INSERT_FREE_BLOCK` writes.

That is why this is an old defect that nobody has hit: the arithmetic only fails
on the one target with 4-byte alignment.

### How it was caught, since "the heap is corrupt" is not a diagnosis

Validating the whole physical block chain at every allocator entry and exit —
not just at the point of failure — made it deterministic in two consecutive
runs, at the same address and the same operation:

```
[Kernel:TLSF] free-list corruption at MALLOC exit
bucket=0/1 block=020cd3f4 size=36 flags=0x2 head=020cd3e8 ... 'Boot Mount'
```

Both runs entered `tlsf_malloc()` with a **valid** heap and failed at its exit,
which is what identified the allocator as the author rather than an external
writer. The free-list head at `0x020cd3e8` and the following physical header at
`0x020cd3f4` are 12 bytes apart: an 8-byte `hdr_t` and a 4-byte payload.

Before this, the corruption was blamed in turn on a task switch, `CopyMem`'s
MOVEM fast path, `lddemon` expunge, and an overlap with the JIT's own allocator.
Each was tested and each was wrong. Worth stating because the entry point that
finally worked — validate the invariant on *entry and exit* of every operation,
so the first invalid state is attributed to the operation that produced it — is
more transferable than the fix.

### The same defect in two places we have not patched

Both in `tlsf_realloc()`, both left alone because neither is on our boot path:

- **The shrink path.** `if (new_size <= GET_SIZE(b))` splits unconditionally and
  gives the remainder `GET_SIZE(b) - new_size - ROUNDUP(sizeof(hdr_t))`. Since
  `GET_SIZE(b) - new_size` can be smaller than `ROUNDUP(sizeof(hdr_t))`, and
  `IPTR` is unsigned, that subtraction can also **underflow to a huge size**,
  which is worse than the malloc case rather than merely equivalent to it.
- **The grow-into-next path.** `if (rest_size > ROUNDUP(sizeof(hdr_t)))` then
  `rest_size -= ROUNDUP(sizeof(hdr_t))`, leaving a remainder that can again be
  a single `SIZE_ALIGN` unit before `INSERT_FREE_BLOCK`.

Whoever takes the patch above should look at these in the same pass. We have not
patched them because we have not reproduced them, and we would rather offer a
reading of the code than a fix we cannot demonstrate.

---

## What we are asking

Review, and an opinion on whether these are wanted as they are. The TLSF one,
the two FAT patches and the sdcard one we believe are straightforwardly
correct. The stack one is a judgement call and we would rather it were argued
than merged quietly.

If any are wanted in a different shape — split, combined, or fixed elsewhere —
that is fine; the point is the defects, not our patches. Upstream has twice now
fixed something we had patched, and fixed it in a better place than we did.
