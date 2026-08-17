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
  - AI_context/consolidated/history/ISSUE-0036.md
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

A second sign points the same way: **the dying task is a CLI**, the boot shell
running the Startup-Sequence, not a Poseidon or USB task.

So the discriminating boot is not about USB. It is: **boot with the `ENV:`
population disabled** -- comment out the `Copy "ENVARC:" "ENV:" ALL ...` line
in the Startup-Sequence -- and see whether the corruption goes with it. That is
one card edit and one boot, and it tests the hypothesis this issue was opened
on rather than the one the last log line suggested.

What remains true from the section below is narrower and still worth keeping:
the real machine always enumerates its soldered hub, so the emulator's idle-USB
state does not exist on hardware, and any rate comparison between the two is
between different regimes.

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

1. **Same Pi, same card, no USB device.** One boot. If it is clean, USB is
   implicated and this stops being a search.
2. **Same Pi, USB device, USB stack disabled** -- remove the two
   Startup-Sequence blocks (`AddUSBClasses`, `AddUSBHardware`; see ISSUE-0042).
   That separates "a device is plugged in" from "the stack enumerates it".
3. Only then the backtrace -- and it needs a decision first, see below.

## Why the backtrace is empty, and why fixing it is not free

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

- 2026-08-17 — Moved to `backlog` at the user's request: no further occurrence
  on their own runs after the four measured here. Recorded rather than closed —
  one sighting of heap corruption is not a fixed defect, and the reasoning for
  waiting is above so that whoever meets it next does not re-derive it.
- 2026-08-17 — Opened. Seen on the first boot after `patches/aros/0023` landed
  and on none of the next three. Call site narrowed to `MERGE_PREV()` by
  elimination — the NULL block can only come from `block->header.prev` at
  `tlsf.c:626`.
