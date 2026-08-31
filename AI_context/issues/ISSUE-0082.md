---
id: ISSUE-0082
title: "A wild jump out of Intuition's input path when an Amiga screen opens"
status: open
priority: critical
type: defect
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-30
tags:
  - exec
  - emu68
  - m68k
  - supervisor
blockers: []
related_files:
  - aros/arch/m68k-emu68/exec/
  - external/emu68/src/ExecutionLoop.c
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

# Where to start

The exception frame is a 68010+ format with a vector offset, so the frame the
handler builds and the frame it returns through have to agree. Read
`arch/m68k-emu68/exec/` -- `dispatch.S`, `switch.S` -- against what Emu68's
ExecutionLoop pushes for an exception, and check the S bit at the point
`Supervisor()` returns.

`Exec_Supervisor_Exit` faulting on its own return instruction is a narrow
enough symptom that reading the two sides against each other should settle it
without another probe.
