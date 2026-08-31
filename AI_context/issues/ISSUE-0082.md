---
id: ISSUE-0082
title: "The chipset writes over AbsExecBase at chip RAM address 4"
status: open
priority: critical
type: defect
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-31
tags:
  - exec
  - emu68
  - m68k
  - rigel
  - chipset
blockers: []
related_files:
  - src/amiga/bus.c
  - aros/arch/m68k-emu68/boot/mmakefile.src
  - aros/arch/m68k-emu68/include/amiga/memory_map.h
---

# How it presents

Deluxe Paint IV is started from its icon and asked for an Amiga mode. The
chipset driver does its whole job:

```text
[AmigaVideo:Hidd] CreateObject: modeid = 00021000
[AmigaVideo:Bitmap] AmigaVideoBM__Root__New: 140x39x5
[AmigaVideo:Blitter] blit_fillrect(0x0,319x255,0,3)
[AmigaVideo:Blitter] blit_copybox: shift=7 rev=0 sw=2 dw=2 01ff fffe
[AmigaVideo] setspritevisible()
```

A 320x256x5 Amiga screen, filled and blitted **through Rigel**. That is the
chipset doing real work for a real application for the first time.

Then the guest dies:

```text
[JIT] opcode 7555 at 01ffffbb not implemented
[AROS/Emu68] CPU exception vector 0x00000010 at PC 0x01ffffbd
[SYS:JIT] RAM dump from 0x01ffffad:  5555 5555 5555 5555 ...
```

`0x01ffffbd` is below the heap (`0x02000000`) in memory filled with `0x5555`,
and the PC is odd. That is the end of a runaway, not its cause.

# CORRECTION: the supervisor stack is not the cause

The first reading of this dump was wrong and is kept here because the mistake
is instructive.

The supervisor stack holds the same eight-byte frame two dozen times, and it
was read as an exception loop. It is neither a loop nor a fault.

`Supervisor()` on m68k works by deliberately faulting
(`arch/m68k-all/exec/supervisor.S`): `or.w #0x2000, %sr` is privileged, so
calling it from user mode raises a privilege violation, and
`Exec_Supervisor_Trap` recognises the faulting PC and grants permission by
**rewriting the frame's PC to `Exec_Supervisor_Exit`** before jumping to the
user's function. The frames in the dump are exactly that, correctly formed.

Emu68 enforces the privilege properly -- `M68k_LINE0.c:838` tests SRB_S and
emits `VECTOR_PRIVILEGE_VIOLATION` -- so the mechanism is working end to end.

And the dump walks *upward* from SSP, into addresses the stack has already
released. Those frames are history, not live state.

Two readings of one dump, both wrong, before reading the code that produces
it. The rule this cost: resolve the mechanism in the source before drawing a
shape from a memory dump.

# What the stacks actually say

`SSP 0x02020600` holds the same exception frame, over and over, for the whole
dump:

```text
+0x00  0x00103062      SR    = 0x0010
+0x04  0x4efc0020      PC    = 0x30624efc,  vector offset 0x0020
   ... repeated ~24 times ...
```

`0x30624efc` resolves against the kernel ELF (base `0x30600000`) to
**`Exec_Supervisor_Exit +0x0`**, and vector offset `0x0020` is **vector 8,
Privilege Violation**.

So `Exec_Supervisor_Exit` takes a privilege violation, the handler returns to
it, and it faults again -- a tight exception loop that fills the supervisor
stack until the PC walks off into unmapped fill.

The user stack says what was going on above it:

```text
USP 0x0203a27c
  +0x00  0x3067c03c   Intuition_69_LockIBase +0x18
  +0x10  0x3068ebd8   notify_mousemove_screensandwindows +0x14
```

Intuition, notifying mouse movement across screens and windows -- which is
exactly what a newly opened screen causes.

# What it means

A wild jump, and the cause is not known.

`0x01ffffbd` is odd and sits just below the heap base (`0x02000000`) in memory
filled with `0x5555`. `A6 = 0x0200011b` is odd too and the reporter says it is
not a library base. Two accesses at `pc=0x01fffee7` reached `0x00b7a2a4`, in
the unmapped classic domain. All of that is a runaway already in progress.

What is not runaway is the user stack, and it is the only evidence of where
this started:

```text
Intuition_69_LockIBase +0x18
notify_mousemove_screensandwindows +0x14
```

`rom/intuition/inputhandler.c` -- the input handler chain, which runs from an
interrupt, walking screens and windows after the pointer moves. A screen that
has just been created on a monitor that has never had one before is the new
thing in that walk.

It is not the chipset, not amigavideo and not DPaint: all three finished their
work, correctly, before this fires.

# Why it is visible only now

Nothing had opened an Amiga screen before. `amigavideo` could not register at
all until `a5ecc0f` -- it asked for monitor 0, `vcgfx` had been given an ID
inside the block amigavideo claims, and `AddDisplayDriver` answered
DD_ID_EXISTS. With no classic display driver there were no Amiga modes, so no
screen ever opened on that monitor and this path was never taken.

The report itself is also new: `patches/emu68/0024` stopped `PC == 0` from
quietly ending MainLoop, so a guest fault now reaches the trap reporter
instead of stopping the machine with a register dump that reads like a clean
exit.

# The transfer that goes wild is named

The dump of 2026-08-31 (`vector 0x0010 at PC 0x01fffe7d`) has the user stack
deep enough to settle where control left. One longword carries the answer, and
it is the one at the very top:

```text
USP 0x0203a27c
  +0x00  0x3067c06c
```

Against the ELF that boot used (base `0x30600000`, so offset `0x7c06c`) that
address is not merely "inside `LockIBase`" -- it is an exact instruction
boundary, and the instruction before it is a call:

```text
0007c062:  2c79 <SysBase>   moveal  SysBase,%a6
0007c068:  4eae fdcc        jsr     %a6@(-564)
0007c06c:  2002             movel   %d2,%d0        <- the longword on the stack
```

`jsr` pushes exactly that return address and then transfers control. Finding
it at USP+0 with the PC at `0x01fffe7d` is the signature of that transfer not
arriving.

And `-564` is not an unknown: `LVOObtainSemaphore` is 94
(`gen/include/defines/exec_LVO.h:98`) and a vector is six bytes, so
`-564` is **`ObtainSemaphore`**. `LockIBase` calls it, and the call lands
nowhere.

# Which leaves exactly two candidates

`%a6` at that `jsr` is not a value some caller handed in. It is loaded two
instructions earlier from the global `SysBase` -- the one symbol with **3011
relocations** in this ELF. A corrupt `SysBase` does not produce one broken
path; it stops the machine everywhere at once, and this machine went on
running.

So either the global is wrong anyway, or `%a6` was right and the six bytes it
indexed are no longer a vector. On m68k a library's jump table is
`struct JumpVec { UWORD jmp /* 0x4EF9 */; void *vec; }`
(`arch/m68k-all/include/aros/cpu.h:60`), laid out downwards from the base, so
entry *n* is at `base - 6n`. Nothing about a smashed entry is visible in a
register dump: the base is right, the offset is right, and the damage is in
memory the call passes *through*.

It is visible in the table's shape, though -- an entry whose first word is not
`0x4EF9` is not a vector any more. The trap probe now walks all of ExecBase's
vectors and says how many fail that test, which separates a stray write that
hit one slot from a heap block that overran the table. Exec is the right
library to walk because every path goes through it.

# CORRECTION: the wild PC is not "below the heap"

Written above, twice: `0x01ffffbd` / `0x01fffe7d` "is below the heap
(`0x02000000`)". That is wrong, and it is my instrument that said it.

`boot.c:533` floors the heap's `lower` at `0x01000000` and then raises it to
`host_mem_end` -- wherever Emu68's own pools end. On this machine that lands
below `0x02000000`, and Rigel publishing its frame at `$01000000` is the other
half of the same picture. So `0x01fffe7d` is *inside* fast RAM, and so is
`0x01f9a2a4`, the other unmarked value on the stack.

The `<- heap` marker in `trapprobe.c` carried `0x02000000` as a hard-coded
lower bound. It was a guess, it was wrong, and it labelled the two most
important longwords in the dump as debris. The bounds were never a constant to
correct: every `MemHeader` on `SysBase->MemList` records the range it owns, so
the probe walks them instead.

# CORRECTION: this is not the input handler, it is OpenScreen

`notify_mousemove_screensandwindows` lives in `inputhandler_support.c`, and
that is why it was read above as "the input handler chain, which runs from an
interrupt". It is not, here. The rest of the user stack names the caller:

```text
  int_openscreen +0x8c
  ActivateMonitor +0xd6
  notify_mousemove_screensandwindows +0x14
  Intuition_69_LockIBase +0x18      <- top of stack
```

`rom/intuition/misc.c` ends `ActivateMonitor` with
`SetAttrs(newmonitor, MA_PointerVisible, TRUE, TAG_DONE)` and then
`notify_mousemove_screensandwindows(IntuitionBase)`. The last driver line in
the log before the fault is `[AmigaVideo] setspritevisible()`, which is that
`SetAttrs` arriving at the chipset driver.

So this runs in DPaint's own task, inside `OpenScreen`, on the first monitor
switch this machine has ever performed -- not from an interrupt.

Only the top-of-stack longword is verified against the disassembly. The others
are candidates: a kernel-range longword on a stack is not automatically a
frame, and the probe marks them precisely so they can be checked rather than
believed.

# ANSWERED: SysBase is address 4, and it is in chip RAM

The vector scan was built to decide between "A6 was wrong" and "the vector
slot was overwritten". It reported neither:

```text
 A6 as a library base: 02000121  (not a library base)
  SysBase 0x02000121  (not walkable)
```

`SysBase` -- the global itself, read out of memory by the reporter -- holds the
same garbage as A6. So A6 was loaded correctly; the thing it was loaded *from*
is what changed.

And that thing is not in the kernel's data segment at all:

```text
$ m68k-aros-nm aros-emu68-m68k.elf | grep -wE 'SysBase|AbsExecBase'
00000004 A AbsExecBase
00000004 A SysBase
```

`arch/m68k-emu68/boot/mmakefile.src` links both as **absolute address 4**
(`-Wl,--defsym,SysBase=0x4`). Every `moveal SysBase,%a6` in this kernel is
`moveal 4,%a6`, and address 4 is a longword of **chip RAM** -- the region
Rigel owns.

That is why `AMIGA_CHIP_RAM_ALLOC_BASE` is `0x1000` and not `0`: the first
page is reserved and nothing allocates there. But nothing was checking that
nothing *writes* there either, and something does.

The three A6 values across three runs are the evidence that it is data and not
a pointer: `0x0200011b`, `0x020000b1`, `0x02000121`. Same high word, low word
different every time -- whatever the chipset happened to be moving.

# Why every earlier reading was consistent with this

- The wild PC is always just below the heap and always odd. `A6 - 564` with a
  corrupt A6 lands wherever it lands, and an odd base gives an odd target.
- It only started when DPaint opened an Amiga screen. That is the first time
  the blitter has moved real data through Rigel.
- The call that dies is whichever library call comes next. `LockIBase ->
  ObtainSemaphore` is not special; it is simply the first `jsr -LVO(A6)` after
  the write.
- Nothing in Intuition, amigavideo or DPaint is at fault. All three finished
  their work.

`setspritevisible()` being the last line before the fault does not make it the
culprit either: `csd->copper1_spritept` points into the copper list, not into
low memory, and the console on this port is deferred, so the last line printed
is not the last thing that ran.

# The guard

`src/amiga/bus.c:amiga_chip_ram_write16()` is the single funnel every chipset
DMA write to chip RAM passes through -- blitter, copper, sprites, bitplanes,
disk, audio. It now reports any write below `AMIGA_CHIP_RAM_ALLOC_BASE`, with
the address and the value, and marks the one that lands on AbsExecBase.

The value is the part that matters. A copper instruction, a sprite word and a
run of blitter output do not look alike, so the value names the unit without
needing a per-unit probe.

# Where to start

Read the `[BELLATRIX:RIGEL:LOWCHIP]` lines from the next run.

- **A run of consecutive addresses** -- a DMA channel with a destination
  pointer that is zero or has wrapped. The blitter's D channel is the only one
  moving that much data here.
- **A single write to 4 or 6** -- a pointer register that was loaded with a
  low value, once.
- **Nothing at all** -- then the write does not come from the chipset, and
  the CPU-side path (`machine_chip_ram_write16`'s other callers, and Emu68's
  own classifier) is where to look next.

