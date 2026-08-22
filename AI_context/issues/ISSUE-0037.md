---
id: ISSUE-0037
title: "The RAM: handler dies on a corrupt block header once the preferences actually load"
status: doing
priority: critical
type: bug
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-18
tags:
  - memory
  - tlsf
  - boot
  - stability
blockers:
related_files:
  - external/aros/rom/kernel/tlsf.c
  - external/aros/rom/filesys/ram/filesystem.c
  - external/aros/rom/filesys/ram/handler.h
  - external/aros/workbench/s/Startup-Sequence
  - external/aros/workbench/classes/zune/nlist/nlisttree_mcc/NListtree.c
  - AI_context/consolidated/history/ISSUE-0036.md
  - patches/aros/0011-kernel-avoid-undersized-tlsf-free-blocks.patch
  - patches/aros/0007-kernel-refuse-to-free-a-pointer-outside-the-heap-and.patch
---

# Summary

## Matching upstream defect backported, but not this first event (2026-08-20)

AROS upstream commit `6897aa2dc5` fixes a heap overwrite in
`NListtree.mcc` with the same failure shape. `InsertTreeImages()` and
`InsertImage()` appended image specifications with
`strncat(data->buf, tmpbuf, DATA_BUF_SIZE)`. That bound applies to the source,
not to the remaining space in `data->buf`, so a nearly-full destination was
overrun and the adjacent TLSF block header was overwritten.

`patches/aros/0046` backports the upstream correction: format one specifier in
a small temporary buffer and append it with `strlcat(..., DATA_BUF_SIZE)`.

The controlled QEMU rerun after rebuilding `NListtree.mcc` and `sd.img` still
reported the familiar `REMOVE_HEADER` NULL block at `STARTING DOS`, before
NListtree/Wanderer was loaded. Therefore this backport removes one proven heap
writer but does **not** explain that earlier event. Keep the issue open; the
same build continued to Wanderer and the vc4gfx mailbox framebuffer displayed
the desktop with the requester on top.

With `ISSUE-0036` fixed, `ENV:` is populated and the Startup-Sequence runs the
work it has been skipping since this port existed. A task then dies — a CLI in
the run below, the RAM: handler on hardware, and `ENV:` is served by that
handler, so both sit on the same copy:

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

# It recurred, on real hardware, with a USB device attached (2026-08-17)

**Unparked.** The user booted a card built from the current pack on a real Pi 3
and got the same signature:

```
[USB2OTG] Init: Device connected, resetting port
...
[Kernel:TLSF] free-list corruption at REMOVE_HEADER: mhe=022d07fc
  requirements=0x00000000 tlsf=022a8a78 bucket=19/0 block=00000000 size=0
  flags=0x0 head=00000000 prev=00000000 next=00000000 task=0228b880
[Kernel:TLSF] Backtrace (0 frames):
[AROS/Emu68] BootUI display takeover
```

Same defect -- `block=00000000`, `bucket=19/0`, so still `MERGE_PREV()` handed
a NULL `block->header.prev`. Different task and different addresses, as
expected on different hardware with a different memory map.

## Correction (2026-08-17): USB is the weak hypothesis, not the strong one

The section below was written on the strength of `Device connected, resetting
port` appearing above the corruption on hardware. **That over-weighted a
visible new line against evidence already in this issue**, and the user was
right to push back.

The contradicting fact was already recorded here: **the corruption occurred
under QEMU, once in four runs, with `No device connected`.** If it happens with
no USB device at all, a USB device is not necessary to produce it. USB was also
present and initialised on every boot before this defect ever appeared.

The stronger correlation is the one this issue opened with and then lost sight
of: **the corruption appeared on the same day `ENV:` started populating.**
Until ISSUE-0036 was fixed, `Copy "ENVARC:" "ENV:"` copied nothing, so no
preference loaded -- no IPrefs, no `C:Decoration`, no PNG through datatypes, no
Zune preference readers. All of that became reachable that day, and the defect
appeared that day.

A second sign points the same way: **the dying task on the QEMU event is a
CLI**, the boot shell running the Startup-Sequence, not a Poseidon or USB task.
(On the hardware event it is not a CLI at all -- see the section below, which
was written after the user read the requester. That is a stronger sign in the
same direction, not a weaker one.)

So the discriminating boot is not about USB. It is: **boot with the `ENV:`
population disabled** -- comment out the `Copy "ENVARC:" "ENV:" ALL ...` line
in the Startup-Sequence -- and see whether the corruption goes with it. That is
one card edit and one boot, and it tests the hypothesis this issue was opened
on rather than the one the last log line suggested.

What remains true from the section below is narrower and still worth keeping:
the real machine always enumerates its soldered hub, so the emulator's idle-USB
state does not exist on hardware, and any rate comparison between the two is
between different regimes.

## The task has a name, and it is `ram` (2026-08-17)

The user read the requester on the Pi. Task `0x0228B880` is **`ram`** -- the
RAM: filesystem handler.

That closes the argument of the previous section by replacing a correlation
with a mechanism, because of one line of the Startup-Sequence
(`workbench/s/Startup-Sequence:17`):

```
Assign "ENV:" "RAM:ENV"
```

**`ENV:` is not merely populated at the same time as the crash. `ENV:` is the
crashing task.** Every file `Copy "ENVARC:" "ENV:" ALL` creates is a packet to
this handler, and every byte of it is stored in memory this handler allocates.
Until ISSUE-0036 was fixed that copy transferred nothing, so this code was
being asked to create no files and store no bytes. It has never done this work
on this port before the day the defect appeared.

Two further assigns land on the same handler and matter for any control run:

```
Assign "T:"     "RAM:T"
Assign "CLIPS:" "RAM:Clipboards"
```

so removing the `ENV:` copy silences the largest writer, not the only one.
`T:` is still written during the boot (`Startup-Sequence:152` builds `T:P`).

### What the handler does with memory, read rather than assumed

`rom/filesys/ram/` keeps file contents in a chain of `struct Block`, each one
an `AllocPooled()` from a private pool created in `commands.c:121`:

```c
handler->muddy_pool = CreatePool(MEMF_ANY, MUDDY_PUDDLE_SIZE,   /* 16 KB */
   MUDDY_PUDDLE_THRESH);                                        /*  8 KB */
```

`min_block_size` is 64 and `max_block_size` is `0x8000` (`commands.c:109-110`),
so **a single RAM: data block can be up to 32 KB, four times the pool's
threshold**. Allocations above the threshold do not come out of a shared
puddle; they get memory of their own from the system heap. That is the only
route by which this driver can damage a TLSF free list rather than a pool's
internal bookkeeping -- worth stating explicitly, because it means the reported
`mhe`/`tlsf` pair does not have to be the system heap for RAM: to be the
culprit, and on hardware it was not (`mhe=022d07fc`, well above the base).

Blocks are freed with an **explicitly recomputed size**, which is the classic
place for this class of defect:

```c
/* AllocDataBlock, filesystem.c:1303-1311 */
block = AllocPooled(handler->muddy_pool, alloc_size);
block->length = alloc_size - sizeof(struct Block);

/* FreeDataBlock, filesystem.c:1360-1363 */
alloc_size = sizeof(struct Block) + block->length;
FreePooled(handler->muddy_pool, block, alloc_size);
```

**Checked, and it holds.** The two expressions are exact inverses, and
`length` being a `UWORD` (`handler.h:81-85`) does not truncate because
`max_block_size` caps `alloc_size` at 32768. So the obvious mechanism -- free
the wrong number of bytes -- is not obviously present, and should not be
assumed.

### Where to look next inside the handler

Not measured, listed in the order the code makes them suspicious:

1. **The shrink path of `ChangeFileSize()` (`filesystem.c:700-726`).** It frees
   the block removed by the *previous* iteration, keeps the last one alive
   outside the list to copy its data into a resized end block, and puts it back
   on the list if the resize fails. Three states for one pointer, one of them
   "allocated but in no list". It also dereferences `RemTail()` without a NULL
   check, and if the loop body never executes, `block` is still `NULL` when
   `WriteData()` is handed `((UBYTE *)block) + sizeof(struct Block)`.
2. **`DeleteObject()` (`filesystem.c:397-402`)** frees blocks under
   `if(block->length != 0)`, because the first element of a file's list is a
   zero-length `start_block` embedded *inside* `struct Object`
   (`filesystem.c:182`) and must never be freed. `FreeDataBlock()` has no such
   guard. Any path that can hand `start_block` to `FreeDataBlock()` frees a
   pointer into the middle of a `clear_pool` allocation.
3. **`Copy ... ALL` writes many small files**, which is the size regime where
   `min_block_size` (64) and the `alloc_size >>= 1` retry loop
   (`filesystem.c:1300-1306`) are exercised, and the regime this handler has
   never run in on this port.

### What this does to the plan

The discriminating boot in the previous section is unchanged and now cheaper to
interpret: disable the `ENV:` copy and the handler stops doing the new work. If
the corruption survives that, `T:` is the remaining writer and the same code is
still the target.

It also makes the empty backtrace (section below) a much smaller problem than
it looked. The interesting frame is no longer "somewhere in a boot"; it is
inside one module of about 3000 lines with exactly three `FreePooled()` call
sites. Bracketing those three is a cheaper instrument than restoring frame
pointers across the tree.

## Two corrections from the user, and the second one opens a suspect (2026-08-17)

**1. The four-run series was not QEMU.** The "Frequency" table above says
`~170 s headless under QEMU`; the user says it was the Pi. Their word stands —
they ran it. The consequence is not cosmetic: the argument in the correction
section, *"it happened under QEMU with `No device connected`, so a USB device
is not necessary"*, **is void**, because there may be no QEMU sighting of this
defect at all.

USB is still the weak hypothesis, but now for a better reason than that one:
the crashing task is `ram`, which is a mechanism rather than a correlation.
What replaces the void argument is a stronger statement in the other
direction — **this defect may be hardware-only**, which is a fact about how to
chase it, not about who causes it.

**2. There may have been no hardware boot at all between the Arasan and this
one.** The user's recollection: no run on real hardware after the card moved to
SDHOST (`1f2e463`, same day). If that is right, the Pi boot that produced this
report was **the first time this project ever ran the SDHOST backend on real
silicon** — first real DMA engine, first real cache, first real card.

That gives the "it appeared the day `ENV:` started populating" correlation a
competitor that changed on the same day, and the competitor writes into memory
by DMA. It does not displace the `ram` finding — a stray DMA write has to land
somewhere, and RAM: holds the largest and most numerous live allocations during
that copy, which makes `ram` a plausible *victim* as well as a plausible
culprit.

### Answered: it was SDHOST, and the silence proves nothing either way

The user looked and found **no** `[SDHost` or `[Arasan` line. That is not
evidence of Arasan — it is what a *healthy* SDHOST boot looks like. All five
unconditional `bug("[SDHost` calls in the backend are failure paths (mbox or
`dma.resource` failing to open, no core clock, no DMA channel, no control
block, DMA timeout, command failure); the eight informational ones are `D(`
and compiled out. A boot where nothing goes wrong says nothing at all.

What the log does say is decisive by other means:

```
[DMA:probe] allocated channel 9
[DMA:probe] released channel 9
[SDBus00] MMC0: [29818MB Capacity]
[SDBus00] controller interrupt is being delivered
```

`[DMA:probe]` is ours (`arch/m68k-emu68/boot/dma_probe.c`), and it only exists
in builds from the `dma.resource` work — which was done *because* SDHOST
requires DMA. The tree has `SDCARD_BACKEND := sdhost`
(`soc/sdcard/mmakefile.src:78`), the pack was built from it, and the SDHOST
backend refuses to initialise without a DMA channel and says so on serial. It
did not say so.

**So that Pi boot ran SDHOST, and it is very likely the first one ever to do so
on real silicon.** The card driver stays a first-class suspect alongside the
RAM: handler.

### Confirmed on real hardware, same path (2026-08-18)

The system-heap sighting below was under QEMU. A Pi 3 boot reproduced it
exactly, with the same seven frames at a different load base (0x36600000
instead of 0x34600000):

```
[Kernel:TLSF] free-list corruption at REMOVE_HEADER: mhe=02000000
  tlsf=02000058 bucket=19/0 block=00000000 task=022c9678
Task : 0x022C9678 - CLI      PC: 0x3661FB9C
```

Same offsets throughout -- `3662048e`, `3662b106`, `366264f0`, `366ca01e`,
`366ca0f6`, `366ca346`, `36624720` -- so the same chain:
`freeLocalVars` in a finishing shell, freeing on the system heap.

So this is not an emulator artefact and not confined to one host. Two hosts,
two pools, two unrelated callers.

### It came back, on a different pool and a different path (2026-08-18)

A boot under QEMU, with the vc4gfx driver linked, produced the corruption again
-- and this time the frame pointers were already on, so it arrived with a
resolved backtrace:

```
[Kernel:TLSF] free-list corruption at REMOVE_HEADER: mhe=02000000
  tlsf=02000058 bucket=19/0 block=00000000 task=022c71b4
[Kernel:TLSF] Backtrace (7 frames):

tlsf_freemem                  (the call site inside tlsf_freevec)
nommu_FreeMem         + 0xcc
Exec_35_FreeMem       + 0x54
__inline_Exec_FreeMem + 0x16
freeLocalVars.isra.0  + 0x36
DosEntry              + 0xa2
Exec_TaskFinaliser
```

Load base 0x34600000, established the same way as before: `PC: 0x3461FB9C`
minus the base is `0x1FB9C`, and the ELF has `1fb9a: 4e47 trap #7`.

**This is not the RAM: handler and not `muddy_pool`.** `mhe=02000000` is the
system heap. The caller is a shell process finishing:
`freeLocalVars()` (`rom/dos/createnewproc.c:617`) walking `pr_LocalVars` and
calling `FreeMem(varNode->lv_Value, varNode->lv_Len)` on the way out.

#### What that does to the hypotheses

**It widens the defect and narrows the explanation.** Two sightings, two
different pools, two unrelated callers -- `rom/filesys/ram` freeing a name
string in `muddy_pool`, and `rom/dos` freeing a local variable on the system
heap. A bug inside either module cannot produce both. Something that scribbles
across the heap can.

That makes the guard bytes in `patches/aros/0030` insufficient by construction:
they cover `muddy_pool`'s two allocation kinds and nothing else, so this
sighting was invisible to them. Whatever is written next has to cover the
system heap.

**It is not vc4gfx**, tempting though a brand-new driver is. This exact
signature -- `mhe=02000000`, a CLI task -- is the one this issue opened with,
months of commits before vc4gfx was built.

**The size argument is not the mechanism, again.** `freeLocalVars()` frees with
a remembered `lv_Len`, which is the same shape as `SetString()`'s recomputed
size and just as unable to cause this: `tlsf_freemem()` discards the size and
works from the block header.

So the conclusion from the RAM: sighting stands and now applies more broadly:
**something overran an allocation and damaged the neighbouring block header**,
and it is not confined to one pool.

### RESOLVED BACKTRACE (2026-08-17) — it is a file being deleted from `RAM:`

The frame-pointer build produced **9 frames**, and they resolve.

```
[Kernel:TLSF] Backtrace (9 frames):
[Kernel]  3662004a   3661f758 = PC
[Kernel]  366260f0
[Kernel]  366b43bc
[Kernel]  366b38d8
[Kernel]  366b3b28
[Kernel]  366b2d56
[Kernel]  366b18b6
[Kernel]  366b10b2
[Kernel]  366c206e
```

**Load base = `0x36600000`**, established rather than guessed: the alert's
`PC: 0x3661F758` minus the base is `0x1F758`, and the ELF has

```
1f756:  4e47        trap #7          <tlsf_fail_corruption+0xf8>
1f758:  4286        clrl %d6
```

— the m68k TRAP frame pushes the address *after* the instruction, so the two
agree exactly. Nearby, `1f744: jsr %fp@(-414)` and `1f752: jsr %fp@(-420)` are
LVO 69 and 70, `KrnBacktraceFromFrame` and `KrnPrintBacktrace`, confirming the
function.

Resolved against the ELF's symbols:

| frame | symbol |
|---|---|
| `3662004a` | inside **`tlsf_freevec`** — the return of its `jsr tlsf_fail_corruption` at `0x20046` |
| `366260f0` | `Exec_119_FreePooled + 0x30` |
| `366b43bc` | **`SetString + 0x6c`** |
| `366b38d8` | **`DeleteObject + 0xa8`** |
| `366b3b28` | `AttemptDeleteObject + 0x72` |
| `366b2d56` | `CmdDeleteObject + 0x70` |
| `366b18b6` | `RAMMain + 0x6ae` |
| `366b10b2` | `ram_Handler + 0x46` |
| `366c206e` | `cleanup` |

*(Frame 0 first resolved as `tlsf_freemem + 0x0` because the return address
`0x2004a` happens to coincide with the start of the next symbol. Disassembly
places the call site at `0x20046`, inside `tlsf_freevec`.)*

So the sequence is: **a file is deleted from `RAM:`, `DeleteObject()` frees the
object's name string through `SetString()`, and the free list is found corrupt
there.**

```c
/* filesystem.c:408 */
SetString(handler, (TEXT **)&((struct Node *)object)->ln_Name, NULL);

/* support.c:86-88 */
size = StrSize(old_str);
FreePooled(handler->muddy_pool, old_str, size);
```

And the boot path has such a deletion, in `RAM:`, by construction — `T:` is
`RAM:T` (`Startup-Sequence:19`):

```
:152  List "ENV:SYS/Packages" ... TO "T:P" ...
:153  Execute "T:P"
:154  Delete "T:P" QUIET
```

#### The obvious theory is dead, and that is progress

`SetString()` recomputes the free size by walking the string
(`StrSize(old_str)`) instead of remembering what it allocated, which is the
classic setup for freeing the wrong number of bytes. **It cannot be the cause
here.** AROS's TLSF discards the argument outright:

```c
void tlsf_freemem(struct MemHeaderExt * mhe, APTR ptr, IPTR size)
{
    (void)size;
    tlsf_freevec(mhe, ptr);
}
```

Everything is taken from the block header at `ptr`. A wrong size passed to
`FreePooled()` is harmless in this allocator.

#### Which forces the conclusion

`tlsf_freevec()` read the header of the string's block, found `PREV_FREE` set,
and followed `block->header.prev` — which was `NULL`. That header sits in the
bytes **immediately before the name string**. Nothing in the free path wrote
it, so it was already wrong when the free began.

**Something overran its allocation and landed on the neighbouring block
header.** And the neighbour is in `muddy_pool`, which holds exactly two kinds
of allocation: **file data blocks** and **name/comment strings**. That is a very
short list of suspects, and it is all inside `rom/filesys/ram`.

The SDHOST and USB hypotheses are not refuted by this, but nothing in nine
resolved frames points at either.

#### A concrete candidate, found by reading `WriteData()`

`filesystem.c`, the write loop:

```c
block_length = block->length - block_pos;
...
if(block_length == 0) { /* advance to the next block */ }
...
write_length = MIN(remainder, block_length);
CopyMem(buffer, ((UBYTE *)block) + sizeof(struct Block) + block_pos, write_length);
```

`block->length` is a **`UWORD`** (`handler.h:81-85`); `block_pos` is a `UPINT`.
The subtraction promotes `length` to `int` and then converts it to `unsigned
long`, so if `block_pos` ever exceeds `block->length` the result is not
negative — it is **enormous**. `block_length == 0` is then false, so the loop
never advances to the next block, `write_length` becomes the whole remainder,
and `CopyMem()` writes it past the end of the block, straight through whatever
follows.

Nothing has been shown to make `block_pos > block->length` yet. The place to
look is the shrink path of `ChangeFileSize()` (`filesystem.c:700-726`), which
rebuilds the end block and can lower `block->length` while an `Opening` still
holds a `block_pos` from before. Independently of this bug, the signedness here
is a landmine worth fixing on its own terms.

#### The Bluetooth stack is not idle on these boots (2026-08-17)

Photographed from a Pi 3 running the current pack: BTScan lists **five devices
with RSSI**, one identified as a mouse with `HID = yes`, and reports
`ready - 5 devices, 1 input` (see ISSUE-0031).

So `btuart.resource` is not merely resident during the boots where this
corruption appears — it is taking PL011 interrupts and framing real traffic
throughout them. Under QEMU's `raspi3b` there is no chip on that UART and the
handler never fires.

That does not implicate it. The receive handler was read and is bounded: the
ring is 8192 and a power of two, `head` is masked on every step, and it does no
allocation and no copy. What it does establish is that the single most obvious
"what is different on hardware" is **active**, not dormant, which is worth
knowing before the guard bytes come back with an answer.

#### An unexplained coincidence in the log, recorded not chased

Immediately above the corruption:

```
FMUL / FBcc / FCMP / FADD / FMOVE from SPECIAL / FMOVE to SPECIAL / FDIV
```

Those are Emu68's one-shot traces (`static int shown = 0;` in
`M68k_LINEF.c`, 49 of them), printed the **first time each FPU opcode is
translated**. So something exercised the FPU for the first time in the same
breath as the corruption.

Probably incidental. Worth writing down anyway, because ISSUE-0038's root cause
was an Emu68 register clobber — `x12`/`v28`, GCC ignoring `-ffixed` in the
prologue — and `CMakeLists.txt:36-39` reserves `v19`-`v26` for the JIT. A newly
exercised FPU translation path touching a reserved register is the kind of
thing that has already happened here once.

### How to advance — and why mungwall, the obvious answer, does not apply

**AROS has a guard-byte debugger and it cannot see these allocations.**
`--enable-debug=mungwall` wraps pool allocations in `MungWall_Build()` /
`MungWall_Check()` (`rom/exec/memory.c:1068,1103`), which is exactly the
instrument this defect calls for. It is on the `InternalAllocPooled()` path —
and `muddy_pool` does not take that path.

Established from the disassembly of the frame this backtrace names, not from
reading source:

```
260d6:  cmpil #1299072868,%a0@(32)   ; 0x4D6E4764 = 'MnGd' = MEMHEADER_EXT_MAGIC
260e0:  moveal %a0@(56),%a2          ; mhe->mhe_Free
260ee:  jsr %a2@
260f0:                               ; <- frame 1, Exec_119_FreePooled + 0x30
```

`FreePooled()` took the `IsManagedMem()` branch and called `mhe_Free`
(`tlsf_freemem`) directly. `InternalFreePooled()`, and therefore
`MungWall_Check()`, are not on this path at all. Turning mungwall on would cost
a rebuild and report nothing.

(This also confirms the earlier deduction about the odd `mhe`/`tlsf` pair:
`tlsf=022a8a78` sits *below* `mhe=022d07fc` because a 16 KB puddle has no room
for `sizeof(tlsf_t)`, so `tlsf_init()` took its second branch and allocated the
context separately — `tlsf.c:1307`.)

#### Confirmed from the other end (2026-08-17)

The user deleted the whole `If EXISTS "ENV:SYS/Packages"` block from the
Startup-Sequence — `List ... TO "T:P"`, `Execute "T:P"`, `Delete "T:P"`,
`CD "SYS:"` — and **the corruption stopped**. Two independent lines of
evidence, a resolved backtrace and a removal test, name the same code.

Three things to keep straight about that result:

* **It is a workaround, not a fix, and it costs something.** That block is what
  runs `S/Package-Startup` for everything under `ENV:SYS/Packages`, which
  includes the bluzing package shipped in the pack. The machine now boots
  without package startup.
* **It is not a minimal control.** The block does at least four things, and one
  of them is not obvious — see below.
* **The defect was one boot in four.** A clean boot or two is consistent with
  the fix and does not on its own establish it.

#### The package bisect: it is none of them (2026-08-17)

The block runs one `Execute` per package that has a startup script, and each
`Execute` makes a `T:` temporary of its own. Counting what is actually there:

| package | has `S/Package-Startup`? | `T:` cycles it adds |
|---|---|---|
| `Developer` | no | 0 |
| `AROSTCP` | yes, 532 B | 1 |
| `aros-bluzing` | yes, 146 B | 1 |
| the block itself: `List → T:P`, `Execute "T:P"` | | 2 |

So the boot performs **four** create/write/delete cycles in `RAM:`, three of
them with generated, variable-length names.

The user removed them one at a time:

| configuration | cycles | result |
|---|---|---|
| everything | 4 | corruption |
| no `aros-bluzing` startup | 3 | **corruption** |
| no `aros-bluzing`, no `AROSTCP` startup | 2 | **corruption** |
| whole block removed | 0 | clean |

**No package is the culprit.** What remains at two cycles is the block's own
`List "…" TO "T:P"` / `Execute "T:P"` / `Delete "T:P"`, and that is enough to
produce it. The `aros-bluzing` startup in particular is four `Assign`/`Path`
lines and could not plausibly have been the cause; it is now measured rather
than assumed.

**What this does not establish.** Removing RAM: traffic removes the code that
*frees* blocks, and freeing is how the damaged header gets noticed. The clean
result at zero cycles is equally consistent with "the defect is in this path"
and with "the defect is elsewhere and this path was the only witness". That
distinction is what `patches/aros/0030` exists to settle.

The remaining cut inside this path is still a card edit: replace the block with
`List` + `Delete` and no `Execute` (short fixed name, one write), or with an
`Execute` of a file already on the card and no `List` (the temporary, whose
name is generated and varies in length). Those are `ram-stress-a` and
`ram-stress-b` reduced to one iteration per boot.

#### `Execute` runs a second cycle, and it is the more interesting one

`Execute` copies the script into a temporary file of its own before running it
(`workbench/c/shellcommands/Execute.c:126-170`):

```c
__sprintf(tmpname, "%sTmp%lu%lu%lu%lu%lu", tmpdir,
          proc->pr_TaskNum, ds.ds_Days, ds.ds_Minute, ds.ds_Tick, count);
tmpfile = Open(tmpname, MODE_NEWFILE);
while((c = FGetC(from)) != -1 && FPutC(tmpfile, c) != -1);
...
DeleteFile(tmpname);
```

So the removed block created, filled and deleted **two** `RAM:` files per boot,
and the second one's **name is generated and varies in length** — task number
plus four clock fields. `muddy_pool` holds file data blocks and name strings
and nothing else, and the crash was in freeing a name string, so a name whose
length changes every run is worth isolating rather than lumping in.

(The `FPutC` loop is buffered by dos.library, so the handler sees buffer-sized
writes rather than one byte at a time. Worth stating because the raw loop
invites the opposite assumption.)

#### The reproducer — `tests/ram-stress/`

Three scripts, written against the split above: `ram-stress-c` repeats the
boot's sequence, `ram-stress-a` drops `Execute`, `ram-stress-b` keeps only
`Execute`'s temporary. Each varies name lengths and file sizes within the
iteration, and prints a marker per pass so the log says how many it survived.
Run `c` first; `a` and `b` then split it in one run each.

#### The plan, cheapest first

1. **A reproducer, before any more building.** The trigger path is now known
   exactly, and the Startup-Sequence walks it once per boot:
   `List ... TO "T:P"` / `Execute "T:P"` / `Delete "T:P"`. A shell script that
   loops creating, writing and deleting files in `T:` — varying name lengths
   and file sizes, since name strings and data blocks are the only two things
   in `muddy_pool` — should hit it far faster than one boot in four. **One card
   edit, no rebuild.** Everything below is cheaper once this exists, and if it
   reproduces under QEMU the loop closes in minutes instead of card-writes.
2. **Guards inside `rom/filesys/ram`, which is mungwall for the one pool that
   matters.** `AddDataBlock()` and `SetString()` are the only allocators in
   `muddy_pool`; over-allocate each by a guard word, write a magic, verify it in
   `FreeDataBlock()` and in `SetString()`'s free path. That names the overrun,
   says which of the two kinds of allocation was overrun, and is perhaps fifteen
   lines in one directory. In the same build, test the named hypothesis for
   nothing:

   ```c
   /* WriteData(), and the same in ReadData() */
   if (block_pos > block->length)
       bug("[ram] block_pos %lu > length %u\n", block_pos, block->length);
   ```

   If it fires, the `UWORD`-minus-`UPINT` overrun is the defect. If it never
   fires across a reproducer run, that hypothesis is dead for free.
3. **Only if 1 and 2 do not settle it: a TLSF consistency walk.** Each block
   header carries size and prev, so an area can be walked linearly and checked.
   Run on every operation for pools under some size, behind a flag, it catches
   the damage within one allocation of when it happened rather than whenever a
   neighbour is next freed — which is the whole distance between the culprit and
   the report today.

**Deprioritised:** the "boot with the `ENV:` copy disabled" test that this issue
has been carrying. It would still work, but it no longer discriminates:
`Startup-Sequence:151` guards the `T:P` sequence on `ENV:SYS/Packages`
existing, so removing the copy removes the trigger without saying anything
about the mechanism.

### The full alert, from serial — and the "CPU error" is ours (2026-08-17)

The requester has a **`More...`** button, which appends the register dump, and
then a **`Log`** button, which does an unconditional
`NewRawDoFmt(..., RAWFMTFUNC_SERIAL, ...)` (`rom/exec/useralert.c:52`). No debug
define, no rebuild. The user pressed it:

```
*** Logged alert:
Program failed
Task : 0x0228B880 - RAM
Error: 0x80000027 - Unknown CPU error
PC   : 0x3661F646
CPU context:
D0: 00000024 FFFFFFFF 00000000 00000000
D4: 00000000 00000000 00000000 00000013
A0: 3661DC94 00000000 022A8A78 00000000
A4: 022D07FC 022CD608 0200200C 022CD5A0
SR:     0004
PC: 3661F646
Stack trace:
0x00000000 Address not found
```

**The registers tie the alert to the TLSF report with no inference at all:**

| register | value | the TLSF line |
|---|---|---|
| task | `0228B880` — `RAM` | `task=0228b880` |
| `A2` | `022A8A78` | `tlsf=022a8a78` |
| `A4` | `022D07FC` | `mhe=022d07fc` |
| `D7` | `00000013` = 19 | `bucket=19/0` |

Same event, same function, same live values still in registers.

**And `0x80000027` is not a CPU fault at all — it is our own trap.** The
corruption reporter in `rom/kernel/tlsf.c` ends:

```c
    __builtin_trap();
    __builtin_unreachable();
}
```

`__builtin_trap()` on m68k compiles to `trap #7` — verified, not assumed, by
running the target compiler on a three-line file. `trap #7` is vector 39 =
`0x27`, `Exec_TrapHandler()` ORs in `AT_DeadEnd`, and `0x27` is absent from the
`ACPU_*` table in `exec/alerts.h`, so it prints as "Unknown CPU error".

So `PC: 0x3661F646` is the `trap #7` inside the reporter, `A0: 3661DC94` is the
format string beside it, and the whole alert is the *deliberate* end of the
detection path. Nothing here says anything about who corrupted the heap.

### Retracted: the odd-PC reading

The section below reads `0x3661FB4B` as an odd address and builds a story on it
— corrupted return address, Emu68 not enforcing the address error a real 68040
would, execution stumbling into `0x4E47`. **All of that is withdrawn.** The odd
value came from a photographed requester transcribed by hand; the logged dump
gives `0x3661F646` in both the header and the context, and it is even, and it
is our own trap instruction. Kept below only so the reasoning is not
re-invented.

### Why the alert's own stack trace was empty, which is a different reason

`FormatAlertExtra()` (`rom/exec/alertextra.c:150`) walks `iet_AlertStack`,
which `Exec_TrapHandler()` set to `ctx->FP` — and on m68k `FP` is **`A6`**.

`A6 = 0200200C`, which is **SysBase**: the m68k library-base register, not a
frame pointer, because the tree is built `-fomit-frame-pointer`. `TypeOfMem()`
accepted it (it is inside the heap), `UnwindFrame()` read two words of SysBase
as if they were a frame link, and the caller came out `0x00000000` — hence
`Address not found` rather than `Invalid stack frame address`.

**The frame-pointer rebuild therefore lights up two independent readouts**, not
one: our `KrnBacktraceFromFrame()` inside the reporter, *and* this section,
which is the better of the two because it runs each address through
`DecodeLocation()` and prints module, segment and symbol names.

### The A6 conflict that would have made the rebuild pointless, and does not

Worth recording because it is the obvious objection and it took reading the
generated header to settle. On m68k `A6` carries the library base, and GCC's
`-fno-omit-frame-pointer` prologue is `link.w %a6,#-N`, which overwrites it. If
the C function read its base from `A6`, frame pointers would corrupt every
library call in the system.

It does not. `AROS_LHn` passes the base as an **ordinary trailing parameter**
(`__AROS_LH_BASE`, `gen/include/aros/libcall.h:195-213`), on the stack in the
normal m68k C ABI; the `A6`-to-stack move happens in the genmodule assembly
stub outside the compiler's view. `A6` is free inside the function body.

Upstream's own m68k-amiga target also passes `CFLAGS_OMIT_FP`
(`configure:11536`), which is worth knowing but is a size decision, not this
one.

### The CPU error decodes to something that fits a corrupted return address (retracted, see above)

```
Error: 0x80000027 - Unknown CPU error   PC: 0x3661FB4B
D0: 000024?? ...                        SR: 0004   PC: 0x3661F646
```

`0x80000027` is `AT_DeadEnd | 0x27`. On m68k the low bits are the **exception
vector number**: `M68KExceptionAction()` computes `Id = vector >> 2`
(`arch/m68k-all/kernel/m68k_exception.c:279`) and `Exec_TrapHandler()` does
`trapNum |= AT_DeadEnd` (`rom/exec/traphandler.c`). Vector `0x27` is 39, and
vectors 32-47 are `TRAP #0`-`#15`, so this is **`TRAP #7`** — not in the
`ACPU_*` table in `exec/alerts.h`, which is why it prints as "Unknown CPU
error" rather than by name.

Nothing in AROS issues `TRAP #7`. A reading that fits every number here, and is
**not established**:

* the alert PC `0x3661FB4B` is **odd**, which no `BSR`/`JSR` return address can
  be — so control reached it through corrupted data, not through a call;
* a real 68040 would raise an address error (vector 3) on an odd instruction
  fetch, but Emu68 is a JIT and does not necessarily enforce that, so execution
  continues into whatever the bytes happen to be;
* `TRAP #7` is opcode `0x4E47`, an ordinary halfword to stumble into.

If that is what happened, the alert is **downstream of the corruption, not the
corruption** — the heap damage came first and overwrote a return address. It
also means the alert PC is not a place to go looking in a disassembly.

### Making the requester reach serial — `patches/aros/0029`

**Corrected before it was needed.** This section was written claiming `Alert()`
has two exits and only the unusable one logs. It has three. The requester's
`More...` button appends the register dump and swaps the first gadget for
`Log`, which writes the whole text to serial unconditionally — no define, no
rebuild. That is the mechanism the user remembered, and it is what produced the
dump above.

So patch 0029 does not make the dump *possible*; it makes it *automatic*, which
still matters for a headless boot, an unattended run, or a requester that
cannot be clicked. Everything below stands except its claim of necessity.

`Alert()` has two exits and **only the unusable one logs**. With Intuition up,
`Exec_UserAlert()` draws the requester and the machine carries on, so the alert
code, task, PC and register dump exist only as pixels — which is why the
reports in this issue arrived photographed and partly transcribed, with `A0`-`A7`
and the SegTracker line missing entirely. `Exec_SystemAlert()` does log, through
`KrnDisplayAlert()`, but it is reached only when the requester *cannot* be
drawn.

The fix is small because the machinery already exists: `Alert_DisplayKrnAlert()`
(`rom/exec/systemalert.c:14`) builds exactly that text and hands it to
`KrnDisplayAlert()`, whose generic implementation
(`rom/kernel/_displayalert.c:70`) prints the framed text through `krnPutC()`
**and returns** — no halt, no reboot, no Intuition. Patch 0029 calls it in
`Alert()` before the `Exec_UserAlert()` attempt, so every alert is logged
first and displayed second.

What that buys beyond convenience: `FormatCPUContext()`
(`arch/m68k-all/exec/alert_cpu.c:27`) prints D0-D7, A0-A7, SR and PC, and then
asks SegTracker to resolve the address to a segment and offset. On serial that
is complete and copyable instead of partial and retyped.

One more thing already in place, which matters for the walker above:
`Exec_TrapHandler()` stores `iet_AlertStack = ctx->FP` with the comment
*"Remember also stack frame for backtrace"*. The frame pointer at the moment of
the crash is therefore already captured — it just had nothing to walk it.

### What was checked in the SDHOST path, and did not pan out

Two hardware-only mechanisms were read for and are **not** present. Recorded so
nobody spends the afternoon on them again:

* **Partial-cache-line invalidate discarding a neighbour's dirty data.** The
  attractive theory: `sdcard_sdhost_bus.c:511` calls `CacheClearE(dma_dest,
  sdDataLen, CACRF_ClearD)` on a buffer aligned to 32 while the A53's line is
  64. It does not bite. AROS's 68040 `CacheClearE` only ever emits `cpushp`
  (`arch/m68k-all/exec/cachecleare_.S:80,87`) — page granularity, and *push*,
  not invalidate — and Emu68 translates `CPUSHP` to a `dc civac` loop
  (`M68k_LINEF.c:5461-5473`), which is clean-then-invalidate. Dirty neighbours
  are written back, not dropped. Note this holds for the `CACRF_InvalidateD`
  calls in `soc/mbox/mbox_init.c:108,176` too: that routine does not honour the
  distinction, it tests only `CACRF_ClearI`.
* **Bounce-buffer overrun.** `sdcard_sdhost_init.c:231` allocates `65536 + 31`
  and aligns the pointer up by at most 31, so exactly 65536 bytes are usable
  and `dma_bounce_size` is 65536. The guard at `:463` is
  `sdDataLen <= priv->dma_bounce_size`, and the else branch is a hard failure
  (`retval = -1`), not a direct-DMA fallback — so an oversized transfer cannot
  quietly take an unaligned path.

What has **not** been checked: what `sdhost_dma_setup()` programs into the
descriptor, whether the transfer length written to the engine can exceed
`sdDataLen`, and what happens on a DMA timeout — `sdhost_dma_wait()` returning
non-zero leaves the engine running while the code returns and the buffer is
reused.

## The USB correlation, kept for the record

`[USB2OTG] Init: Device connected, resetting port`.

**Every previous observation of this corruption was on a machine with no USB
device attached.** That was established independently while writing ISSUE-0042:
across the seven instrumented boots of 2026-08-17 every serial log says
`No device connected`, and `scripts/boot-timing.py` never attaches one. So a
device enumerating -- descriptors, class binding, interrupt pipes, all of it
allocating -- is a variable that has never been present before, and it is
present here.

That is a correlation, not a cause, and it is worth being precise about what
else changed at the same time, because **three variables moved together**:

| variable | previously | in this boot |
|---|---|---|
| host | QEMU | **real Pi 3** |
| USB device | none, in every run | **connected and enumerating** |
| card backend | Arasan (PIO) | **SDHOST (DMA)** |

Any of the three could matter. The SDHOST one is the least likely on the
evidence available: the QEMU boot on SDHOST did *not* corrupt, and the QEMU
boots that did corrupt were on Arasan. That leaves hardware and USB, and USB is
both the newer variable and the one with an allocation-heavy path.

## Why QEMU almost never shows it and hardware does

The user's observation: the card `run.sh` boots does not corrupt under QEMU,
and a card from the same tree does on the Pi. The difference is not the card.

**A Pi 3B always has a USB device, because the hub is soldered to it.** The
LAN9514 carries the hub and the Ethernet, so the stack always enumerates
something on real hardware whether or not anything is plugged in -- which is
what `Device connected, resetting port` is. Under QEMU's `raspi3b` with no
`-device`, nothing enumerates and the stack goes idle after init.

So "no USB device attached" is a state that **only exists in the emulator**.
Every clean measurement this project has was taken in a regime the real machine
never enters, which is the same finding ISSUE-0042 records from the other
direction.

That does not make USB the cause. It does explain the rates: 1 in 4 under QEMU
with the stack idle, and apparently reliable on hardware with it enumerating.

## What to do now, cheapest first

**Superseded by the `ram` finding above**, which reorders this list rather than
adding to it. Kept because the reasoning behind items 1 and 2 is still sound if
the first test comes back clean; they are now the second question, not the
first.

1. **Same Pi, same card, `ENV:` copy commented out.** One boot, one card edit.
   The crashing task is the handler that copy writes into.
2. **Bracket the three `FreePooled()` call sites in `rom/filesys/ram`** if the
   first test implicates it. Cheaper than restoring frame pointers, and it
   names the block rather than the frame.
3. Same Pi, same card, no USB device. Now a control, not the lead.
4. Same Pi, USB device, USB stack disabled -- remove the two Startup-Sequence
   blocks (`AddUSBClasses`, `AddUSBHardware`; see ISSUE-0042). Separates "a
   device is plugged in" from "the stack enumerates it".
5. The backtrace last -- and it needs a decision first, see below.

## Why the backtrace is empty — the answer, replacing the section below (2026-08-17)

**There was no walker.** `rom/kernel/backtracefromframe.c` is a stub:

```c
{
    AROS_LIBFUNC_INIT
    /* The implementation of this function is architecture-specific */
    return 0;
    AROS_LIBFUNC_EXIT
}
```

`arch/aarch64-all/kernel/`, `arch/x86_64-all/kernel/` and
`arch/riscv64-all/kernel/` each override it. **m68k never did.** So
`Backtrace (0 frames)` was the literal `return 0` above, and no compiler flag
was ever going to change it.

The section below reached the opposite conclusion by reading the autodoc — which
does talk at length about needing `-fno-omit-frame-pointer` — and not the body
underneath it. It is kept because its *cost* reasoning is still correct and now
applies for real; only its diagnosis was wrong. Worth stating plainly: the user
had offered to pay for a full rebuild with frame pointers, and on its own that
rebuild would have produced **exactly the same zero frames**.

### What was done

1. **`aros/arch/m68k-emu68/kernel/backtracefromframe.c`** — walks the chain
   GCC's `link.w %a6,#-N` / `unlk %a6` builds: saved `%a6` at `0(%a6)`, return
   address at `4(%a6)`, the same shape as the AArch64 x29 record. Validity
   tests are m68k's: odd return addresses cannot come from a `BSR`/`JSR`,
   frames must climb, and a jump of more than 256 KB ends the walk. The task's
   `tc_SPLower`/`tc_SPUpper` bound it **only when the starting frame is inside
   them** — this reporter is also reached from supervisor and interrupt
   context, where the live stack is neither, and requiring those bounds there
   would reject every frame and reproduce the bug being fixed.
2. **`patches/aros/0028`** — the target hard-coded `$(CFLAGS_OMIT_FP)`. It now
   reads `BELLATRIX_FRAME_POINTERS=1` at configure time:

   ```
   BELLATRIX_FRAME_POINTERS=1 ./scripts/build-aros.sh
   ```

   A switch rather than a default, because unlike the other three
   architectures m68k pays for it in JITted code and this target is built for
   size. `build-aros.sh` stamps the setting and forces a reconfigure when it
   changes, so a tree configured one way cannot be quietly rebuilt the other —
   the flag is per-object, and a half-converted tree yields half a backtrace.

**Both are needed and neither is sufficient.** The walker with frame pointers
omitted finds nothing to climb; the flag without the walker still returns 0.

## Why the backtrace is empty, and why fixing it is not free (superseded)

`[Kernel:TLSF] Backtrace (0 frames)` is not a defect in the walker. From
`rom/kernel/backtracefromframe.c`'s own autodoc:

> *"This function relies on standard frame-link conventions and **requires code
> to be compiled with frame pointers enabled** (for GCC or Clang, use
> `-fno-omit-frame-pointer`)."*

and under BUGS: *"may produce incomplete results if compiler optimizations omit
frame pointers"*.

This port compiles with **`-fomit-frame-pointer`** -- visible in any failing
compile line, e.g.

```
-march=68040 -Os -fno-strict-aliasing -ffreestanding -fomit-frame-pointer ...
```

With no frame chain there is nothing to walk: the walker looks at the first
link, finds nothing valid, and stops. Zero frames, no error. AROS already has
both halves available (`config/features.in:310-311` defines
`aros_cflags_omitfp` and `aros_cflags_noomitfp`), so building some targets
without omitting is anticipated.

**The scope of the fix is a cost decision, not an obvious correction.** A
backtrace is only useful to this issue if the frame that *called the free* is
in it, and that caller is almost certainly outside `rom/kernel/`. So building
only the kernel with frame pointers would produce a backtrace that stops
exactly before the interesting frame. Building everything with them costs a
register and some speed in JITted m68k code -- in the middle of a performance
push. Measure that cost before choosing the scope.

Note step 2 is the same card modification ISSUE-0042 wants for its first
measurement, so one card serves both.

# Previously: parked until it recurs (2026-08-17)

**The user's call, and the right one.** It has been seen once. Chasing an
intermittent heap corruption that will not reproduce burns runs and concludes
nothing, and this project has been burned by exactly that before.

What the parking does **not** mean:

- **It is not gone.** Heap corruption goes quiet when the allocation pattern
  stops lining up, not when it is fixed. And the pattern is precisely what
  changed today: `ENV:` now populates, so IPrefs, `C:Decoration` and the Zune
  preference readers allocate for real where they used to do nothing. Silence
  from a handful of boots after that is weak evidence.
- **It is not a reason to skip step 2 below.** The most useful line of the
  report came back empty:

  ```
  [Kernel:TLSF] Backtrace (0 frames):
  ```

  Fixing that is not chasing this bug — it does not need a reproduction, and it
  decides whether the *next* occurrence (of this or of any other memory defect)
  is a lookup or another dead end. The trap is set and the camera has no film.

So: do not spend runs trying to provoke it. Do make sure it is worth catching.

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

- 2026-08-22 -- **This is what stops the boot splash reaching the desktop.**
  The boot presentation is meant to stay up until Wanderer reports its icons
  (video.md §12), and it does not: it goes at ~18-20s on every QEMU boot. The
  reason is not the splash logic, which is behaving exactly as designed.

      [BootUI] STARTING DOS...
      [Kernel:TLSF] free-list corruption at REMOVE_HEADER ...
      ############ Software Failure! ############
      [graphics/display] LoadViewPorts ... (the requester's screen)
      [BootUI] [00:20.094] display takeover

  `bootui_hold()` refuses to hold for a screen that opens before Wanderer has
  been announced -- "a screen opening before Wanderer was started is something
  the user needs to see now" -- which is §5's rule that a boot presentation
  must never hide an error. The requester this issue produces is that screen.
  So the splash steps aside for it, correctly, and never comes back.

  Two consequences worth stating. The first is that "splash until icons" needs
  no further work in the graphics stack; it needs this defect fixed. The second
  is that this is now visible on every boot rather than only when someone reads
  the serial log, which is a better place for it to be.

- 2026-08-18 -- **Correction: the missing header was mungwall's own bug, not a
  write.** The dump added the same day answered on the first run, and it said
  the block was fine:

      [MungWall] header region at 0x020c87e8:
      [MungWall]   +00: 000000ac 020c8738 020015b0 1adebca1
      [MungWall]   +10: 00000000 00440000 00003662 f46b0204
      [MungWall]   +20: 16243662 5934dbdb dbdbdbdb dbdbdbdb

  0x1adebca1 is MUNGWALL_HEADER_ID and it sits at +0x0c, not +0x08. Read from
  four bytes higher, with m68k's two-byte packing (a 2-byte `BOOL mwh_fault` at
  +12 and no padding after it), the whole header parses: magic, fault 0,
  allocsize 68 for an AllocVec of 68, pool 0, owner 0x02041624 -- the very task
  doing the free. Nothing overwrote anything.

  `MungWall_Check()` rounds the pointer down to `MEMCHUNK_TOTAL` before
  computing the header address. That is for AllocAbs, and it is wrong whenever
  the allocator aligns more finely than MEMCHUNK_TOTAL -- which on m68k it
  does: MEMCHUNK_TOTAL is `sizeof(struct MemChunk)` = 8, `AROS_WORSTALIGN` is
  4, TLSF hands out 4-aligned blocks. The message printed its own arithmetic:
  0x020c8830 for a real 0x020c8834, and 72 bytes for a real 68 plus the 4 it
  had just added. The rounded pointer then went to FreeMem, so TLSF took a
  block header from inside a wall and called it a double free.
  `patches/aros/0043` looks where the caller pointed first and only falls back
  to the AllocAbs alignment when there is no header there.

  So the previous entry's conclusion is withdrawn: that run did not find this
  issue's writer, and mungwall could not have found anything on m68k in that
  state. It found why. What survives from it is the call chain -- the failure
  happens inside `IntuitionInit`, at the mouse pointer, long before `ENV:` --
  and that only holds if the next mungwall run still stops there.

- 2026-08-18 -- **MungWall moved the failure two thirds of a boot earlier and
  gave it a name.** With `mungwall` on the command line the Pi 3 does not reach
  the CLI crash at all; it dies in `Exec Bootstrap Task` with a sixteen-frame
  chain, resolved against the current ELF:

      IntuitionInit -> MakePointerFromPrefs -> MakePointerFromData
        -> NewObjectA (pointerclass) -> AllocSpriteDataA
          -> Graphics_113_BitMapScale -> HIDD_DoMethod
            -> BM__Hidd_BitMap__BitMapScale -> FreeVec

      [MungWall] FreeMem(0x020c8830, 72) from FreeMem: no mungwall header
      [Kernel:TLSF] free-list corruption at double free: block=020c8828
          size=3688618968 flags=0x3 prev=dbdbdbdb next=00000044

  The disassembly places the return address (`BitMapScale+0xbc`) at the second
  of the three tail frees, so the pointer is `dstbuf`. 0xDBDBDBD8 and
  `prev=0xdbdbdbdb` are wall fill: the memory under that pointer is a mungwall
  wall and the header that belongs below it is not there.

  `BM__Hidd_BitMap__BitMapScale()` allocates three buffers and frees the same
  three pointers -- read it and there is no mismatch -- so the pointer is not
  the defect. A missing header on a correctly-freed pointer is somebody else's
  write, which is the same conclusion the CLI crash reached from the other end
  and now with a live neighbour to look at.

  `patches/aros/0041` makes that path answer the remaining question. Three
  different situations print the same "no mungwall header" line today -- a
  block older than mungwall, a block whose header was overwritten, and a
  pointer that was never the start of an allocation -- and only the raw words
  separate them, so it prints them. It then runs `MungWall_Scan()`, which
  checks every live allocation's walls and reports a broken one together with
  the task and caller that allocated it. A neighbour that overran its own block
  shows up there as "Post-wall broken", with the writer attached.

  Note what this already rules out: whatever is wrong happens before Intuition
  has a mouse pointer, i.e. long before `ENV:` or the RAM: handler, which is
  where the first three sightings pointed. The earlier readings were all of the
  same damage seen later.

- 2026-08-18 -- **The chain is resolved through dos.library, and MungWall is
  the tool this needs.** Against the current ELF (base 0x36600000, link map
  plus `nm` on the pre-localize objects):

      3661fb9c  fault PC, inside tlsf_freemem's inlined REMOVE_HEADER
      3662048e  tlsf_freemem
      3662b106  nommu_FreeMem
      366264f0  Exec_35_FreeMem
      366ca226  dos.library __inline_Exec_FreeMem
      366ca2fe  dos.library freeLocalVars.isra.0
      366ca54e  dos.library DosEntry

  `freeLocalVars()` (`rom/dos/createnewproc.c:617`) has exactly one FreeMem:
  `FreeMem(varNode->lv_Value, varNode->lv_Len)`. The report says `block=0`
  from `MERGE_PREV`, which means the freed block's header claims its
  predecessor is free while `header.prev` is NULL -- a header that was
  overwritten, not a bad free. So this call site is the first to *touch* the
  damage, not its cause, and reading further up the DOS side will not find the
  writer.

  What finds the writer is `mungwall`: AROS walls every AllocMem and checks the
  walls on FreeMem, so an overrun is reported against whoever wrote past its own
  allocation. It is a boot argument, not a build option
  (`rom/exec/prepareexecbase.c:332` reads it out of `KRN_CmdLine`), so it costs
  nothing but a run. `scripts/make-sdcard.sh` now takes
  `BELLATRIX_CMDLINE_EXTRA` for exactly this, and on a card already written it
  is one word appended to `cmdline.txt`.

  Two upstream observations from reading the same file, neither of them this
  bug: `copyVars()` leaves `newVar->lv_Value` aliasing the parent's buffer when
  `lv_Len == 0` (harmless only because `FreeMem` early-returns on size 0), and
  `SetVar()` dereferences `lv` unconditionally after an `AllocVec` that is
  allowed to fail.

- 2026-08-18 — **Fatal on hardware, not cosmetic.** On the Pi 3 the same
  `freeLocalVars` chain now raises the requester twice for task `CLI` and the
  boot ends at `PC = 0x00000000` instead of carrying on; in QEMU the same build
  raises it once and reaches Wanderer. Two alerts and a jump to zero is what
  alert-during-alert looks like: `Exec_UserAlert` needs a screen to draw on,
  and with `GFX_BACKEND=vc4gfx` the HVS takeover was declining (ISSUE-0043),
  which is the difference between the two hosts worth testing first. Raised to
  `critical` at the user's reading: this is now the defect that decides whether
  a Pi boot finishes, so it outranks the video work it was competing with.
  The guards from `patches/aros/0030` cover muddy_pool only; this chain frees
  on the system heap and they never see it.

- 2026-08-17 — **The backtrace works and names the path.** Nine frames, load
  base `0x36600000` established from the `trap #7` instruction rather than
  assumed. The chain is `ram_Handler` → `RAMMain` → `CmdDeleteObject` →
  `AttemptDeleteObject` → `DeleteObject` → `SetString` → `FreePooled` →
  `tlsf_freevec`: **a file being deleted from `RAM:`, freeing its name
  string**. The attractive theory — `SetString()` recomputing the free size
  from the string's contents — is dead, because `tlsf_freemem()` discards the
  size argument. That forces the conclusion that a **neighbouring allocation in
  `muddy_pool` was overrun**, and that pool holds only file data blocks and
  name strings. A concrete candidate came out of reading `WriteData()`: a
  `UWORD`-minus-`UPINT` subtraction that turns an out-of-range `block_pos` into
  an unbounded `CopyMem()` rather than a negative length.
- 2026-08-17 — **The `Log` button gave the full dump, and it says the "CPU
  error" is our own `__builtin_trap()`.** `A2`, `A4` and `D7` still held `tlsf`,
  `mhe` and the bucket index from the TLSF report, so the alert and the
  corruption are one event with no inference. `0x80000027` decoded to `trap #7`
  — checked by compiling `__builtin_trap()` with the target compiler — which
  retracts the odd-PC story written an hour earlier from a photographed
  requester. The alert's own empty `Stack trace:` turned out to be `ctx->FP`
  being `A6` holding SysBase, which is a second reason the frame-pointer build
  pays: it repairs that section too, and that one resolves symbols.
- 2026-08-17 — **SDHOST confirmed on that boot, and the requester now reaches
  serial.** The absence of a `[SDHost`/`[Arasan` line turned out to be evidence
  of nothing — every unconditional line in that backend is a failure path. The
  backend was settled from `[DMA:probe]` and the tree's `SDCARD_BACKEND` line
  instead. `0x80000027` decoded to `TRAP #7` on an odd PC, which reads as a
  consequence of the corruption rather than its cause. The user's idea about
  the serial port became `patches/aros/0029`: `Alert()` logs before it displays,
  so the register dump and the SegTracker line stop being something to
  photograph.
- 2026-08-17 — **The empty backtrace was not a flag, it was a missing
  function.** The user offered to pay for a tree-wide rebuild with frame
  pointers; that rebuild alone would have changed nothing, because
  `KrnBacktraceFromFrame()` has no m68k implementation and the generic body is
  `return 0`. Wrote one, and made the flag a switch
  (`BELLATRIX_FRAME_POINTERS=1`) rather than a default, since m68k pays for it
  in JITted code. Not yet built or run.
- 2026-08-17 — **The user read the requester on the Pi: the task is `ram`.**
  That turns the ENV: hypothesis from a same-day correlation into a mechanism,
  because `ENV:` is `RAM:ENV` and the RAM: handler is what stores every byte the
  copy writes. Title and summary corrected — this issue was opened calling it a
  CLI, which was true of the QEMU sighting and not of this one. `rom/filesys/ram`
  read for the obvious size-mismatch defect (`FreePooled` with a recomputed
  size); it is not there, and the three suspicious paths that remain are
  recorded above rather than guessed at.
- 2026-08-17 — Moved to `backlog` at the user's request: no further occurrence
  on their own runs after the four measured here. Recorded rather than closed —
  one sighting of heap corruption is not a fixed defect, and the reasoning for
  waiting is above so that whoever meets it next does not re-derive it.
- 2026-08-17 — Opened. Seen on the first boot after `patches/aros/0023` landed
  and on none of the next three. Call site narrowed to `MERGE_PREV()` by
  elimination — the NULL block can only come from `block->header.prev` at
  `tlsf.c:626`.
