# Four changes we would like to send upstream

Four patches from `patches/aros/` that are not about our target. Each is a
defect in code shared by every AROS port, or a value that makes m68k an
outlier for no reason we can find. None mentions `m68k-emu68`; none depends on
another; each applies to upstream HEAD on its own.

They are written up here so they can be reviewed before being offered, and so
the reasoning survives if the answer is no.

Context, because it affects how much weight to give the evidence: these were
found while bringing AROS/m68k up on a Raspberry Pi under Emu68, a machine
with no Amiga chipset and 840 MB of RAM. That is an unusual environment and it
exposes things a normal Amiga does not. Where the argument depends on this
being unusual, it says so.

Verified against upstream HEAD `85705361ca` (2026-08-13): all four defects are
still present.

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
**Our patch:** `0006-raise-the-m68k-default-task-stack-to-match-the-other.patch`

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
**Our patches:** `0007-fat-write-cluster-and-size-little-endian.patch`,
`0009-fat-convert-directory-dates-little-endian.patch`

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

## What we are asking

Review, and an opinion on whether these are wanted as they are. The two FAT
patches and the sdcard one we believe are straightforwardly correct. The stack
one is a judgement call and we would rather it were argued than merged quietly.

If any are wanted in a different shape — split, combined, or fixed elsewhere —
that is fine; the point is the defects, not our patches. Upstream has twice now
fixed something we had patched, and fixed it in a better place than we did.
