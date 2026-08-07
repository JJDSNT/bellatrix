---
id: ISSUE-0007
title: "Intermittent Emu68 boot loses task context before Wanderer icons"
status: doing
priority: critical
type: bug
owner: agent
created_at: 2026-08-04
updated_at: 2026-08-07
tags:
  - aros
  - emu68
  - scheduler
  - fat
  - wanderer
blockers:
related_files:
  - aros/arch/m68k-emu68/platform/bcm283x/interrupt_controller.c
  - external/aros/arch/m68k-emu68/exec/
  - external/aros/arch/m68k-emu68/kernel/context.c
  - external/aros/arch/m68k-emu68/platform/platform.c
  - external/aros/arch/m68k-emu68/platform/bcm283x/interrupt_controller.c
  - external/aros/arch/m68k-emu68/platform/bcm283x/system_timer.c
  - external/aros/arch/m68k-emu68/doc/host-interrupts.md
  - external/aros/rom/kernel/addirqhandler.c
  - external/aros/rom/filesys/fat/date.c
  - external/aros/rom/filesys/fat/direntry.c
  - external/aros/rom/filesys/fat/ops.c
---

# Summary

QEMU sometimes reaches Wanderer but does not draw volume icons; other runs
fail at different points of `startup-sequence`, with corrupt PC/A6 or an
Emu68 exception.

**Status 2026-08-05, end of day.** The desktop is reachable again, with icons,
from a build made by this repository — see `docs/known-good-baseline.md`. Two
regressions were found and removed, neither of them the subject of this issue:

1. An open-bus guard drifted into `external/emu68/src/aarch64/vectors.c`,
   discarding every access above 16 MB outside `sys_memory`. The framebuffer is
   at `0x3c100000`; every write to the display was thrown away.
2. The card generates its font indexes at boot and an earlier boot wrote them
   through an unfixed fat-handler, byte-swapping size, cluster and date. After
   that every boot hangs in `OpenDiskFont` with nothing on the serial.

The original subject — **the intermittency** — is untouched and is now the only
thing left. No rate has been measured on the restored baseline; do not quote one
until it has been.

**Status 2026-08-06, end of day.** The rate is ~38% and nothing moved it. Three
real defects were closed (`patches/aros/0007`, `0008`, `0009`) and one was
found and deliberately *not* fixed, because fixing it makes the boot worse: the
FAT cluster comparison, which has been masking an uninitialised environment.
The failure is now diagnosable end to end and reproducible deterministically.
See the sections below, newest first.

**Earlier that day: measured, and it is sharper than expected.** Ten runs with
`scripts/boot-timing.py` (ISSUE-0011), idle machine, a freshly generated card
before each — so the card is mechanically ruled out, each run has its own image
hash in `out/boot-timing.jsonl`:

| verdict | runs | |
|---|---|---|
| `icons` | 4 | 46.1 / 51.1 / 51.8 / 53.3 s |
| `workbench` | 4 | screen at 42.5–47.9 s, then nothing |
| `logo` | 2 | never left Emu68; ~79 KB serial against ~127 KB |

Three things follow, and they narrow this issue considerably.

**Opening the screen is very nearly deterministic.** Across all eight runs that
got there, the Workbench screen appeared between 39 and 48 s — including in
every run that later failed. Whatever is intermittent happens *after* Intuition
has a screen.

**The stall is a stall, not slowness.** Two runs were given a 900 s timeout and
sat on an empty backdrop for the whole of it, with the non-modal pixel count
exactly 0 — not a pixel changed in fifteen minutes. This also corrects a figure
this repository carried: the icons were documented as taking ~480 s. They take
about 50 s, and what was being read as "still loading" was this stall.

**There are two distinct failures, not one.** `logo` runs die much earlier, with
a third less serial output; `workbench` runs get all the way to a drawn screen
and then stop. Attributing them together is a mistake.

The suspects worth taking to this, in order: ISSUE-0002 (`STOP` does not consult
`INT64` on stock builds — and `STOP` is exactly where an idle guest that has
finished opening a screen would be sitting), then the delivery-mechanism
question in ISSUE-0004/ISSUE-0010.

## Where the stall actually happens (2026-08-06, six runs with serial kept)

The pixel measurement above says *when*; this says *where*. Six more runs, serial
logs kept — 1 `icons`, 5 `workbench`. Comparing the tails:

**Every stall is at a different point, and every one is inside the library
loader.** This is the objective form of the long-standing impression that it
"stops in random places":

| run | last thing on the serial |
|---|---|
| 143507 | `[LDDemon] Lddemon_0_OpenLibrary()` / `LDRequestObject()` |
| 143711 | `[LDLoadSeg] name=libs/z1.library` |
| 143915 | `[LDDemon] Lddemon_0_OpenLibrary()` / `LDRequestObject()` |
| 144208 | `[LDLoadSeg] name=SYS:Classes/datatypes/picture.datatype` |
| 144412 | `[BOARD] Loaded module to 7defb548...` — see below |

**It is not a particular library.** The successful run loads `z1.library`,
`png.library` and `picture.datatype` to completion, and every run — successful or
not — reaches `IconListview.mui` the same number of times. All six get to the
same depth by every milestone that can be counted. The failing ones simply stop,
mid-load, somewhere different each time.

**It is late, and after the screen.** The screen opens at 42–48 s; the stalls are
after Wanderer has already loaded its MUI classes and is pulling in more code.
So "the intermittency lives after Intuition has a screen" can be sharpened: the
boot keeps making real progress past the screen and then dies during `LoadSeg`
— which is the DOS → fat-handler → sdcard path, not the graphics path.

**One run shows something worth chasing.** In 144412, the last five lines are
Emu68 constructing an FDT and *mapping the Zorro III ROM board* — the board from
`patches/emu68/0002` — immediately after AROS had finished initialising
`IconListview.mui`. Those lines appear **nowhere else**: not earlier in that run,
and not at all in the successful run or in the other stalls. A Zorro III
autoconfig sweep does not belong there. The shape fits a wild access landing in
the autoconfig window and being answered as if it were a probe, which would be a
symptom of exactly the lost context this issue is named after. One run is not
evidence of a mechanism, but it is a concrete thing to look for.

## Next: the one question that splits this issue in two

**Is a stalled guest executing, or is it waiting?** Everything else follows from
the answer, and it has never been asked. If it is waiting, this is a lost
wakeup — an interrupt that never arrives. If it is executing, this is lost
context, a wild PC, which is what the issue is named after. Two disjoint
investigations, treated as one until now.

**The probe.** The QEMU monitor reports ARM state (`info registers` →
`PC=fffffff00014f254`, `info cpus` → the four cores), and
`scripts/boot-timing.py` already holds that socket open for the whole run. Add
PC sampling to it and the answer arrives with every future stall, at no extra
cost — the lesson already paid for once today by discarding the serial logs.

What makes it decisive is `EMIT_STOP`: on a stock build it emits a bare `wfi()`
(ISSUE-0002). So if the ARM PC on a stalled run parks at that `wfi`, the guest
executed `STOP` and is waiting for something that never comes. One run would
promote ISSUE-0002 from suspect to mechanism. If the PC keeps moving inside the
JIT arena, ISSUE-0002 is out of the way and the other branch opens.

**Branch A — waiting.** ISSUE-0002 is the target, not by analogy: `STOP` is
literally where an idle guest sits, and `wfi` wakes on a masked IRQ without ever
consulting the `INT64` state the rest of the delivery machinery is built on. It
drags along a cheap check of ISSUE-0005 (a 32-bit access spanning
`INTENAR`+`INTREQR` falls through to memory instead of the shadow). Only after
that does ISSUE-0010/ISSUE-0004 become worth its cost, because then the
delivery-mechanism question has a concrete symptom to explain.

**Branch B — executing.** The frame question returns — 66 versus 68 — and with
it the `arch/m68k-emu68/exec/` backend parked on `codex-2026-08-05`, now
assessable piece by piece against a base that works and an instrument that
measures.

One data point already leans to B: in run `2026-08-06T144412Z` Emu68 mapped the
Z3 board *after* AROS had stopped, which means the guest performed an access at
`0xe80000` — it was executing, and executing wrong code. That is one run, and
the two populations (`logo` and `workbench`) may well have two mechanisms. The
probe distinguishes those too.

**A target, so "stable" stops being an impression.** 10 of 10 runs under the
current protocol — fresh card, idle machine, verdict from pixels. The standing
figure is 13 of 34.

## Answer: the guest is neither executing nor waiting — Emu68's ARM stack is gone

The probe was added to `scripts/boot-timing.py` and answered on its first
series. Six runs, 2 `icons` / 4 `workbench`. Three of the four stalls are the
same thing, and it is not what either branch predicted:

```
PC = ffffff80001a4200    curr_el_spx_sync + 0      (AArch64 sync exception vector)
SP = ffffff7fffffff80    below ffffff8000000000
SP = ffffff8000000040    0x40 above the floor
X04= 0000000000dff01e    INTREQR
```

Emu68 reports `ARM stack top at 0xffffff8000080000` at boot, and the stack grows
down into the 512 KB below it. In these runs it is **exhausted** — one run 64
bytes short of the floor, two already past it. `curr_el_spx_sync` opens with
`stp x0, x1, [sp, #-160]!`, so once the stack is gone that push faults, which
takes another synchronous exception, which does the same push, which faults. A
two-instruction loop, which is why the PC sits at exactly `+0` in every sample
(`pc_distinct_tail` = 1 across the whole tail). One run carries `X04 =
0x96000044`, which read as an ESR is *data abort at the same EL, write,
translation fault level 0* — the push itself.

**In two of the three, the guest address in flight is `0xdff01e` — INTREQR.**
That register is served from `INT_shadow` by `patches/emu68/0001`, through a
page fault into this very vector. Every INTENA/INTREQ access the guest makes is
one trip through `curr_el_spx_sync`; 512 KB at 160 bytes a frame is about 3200
nested exceptions, which is not a slow leak but genuine unbounded recursion.

A candidate is already written down and marked untested in ISSUE-0005: a 32-bit
access spanning `INTENAR`+`INTREQR` (`0xdff01c`–`0xdff01f`) is one access to two
registers, and it **falls through to memory** — but `0xdff000` is the trapped
page, so the fall-through is itself in the faulting window. That is the right
shape. It is not yet proven to be the path.

**The fourth stall is a different animal**: PC inside the JIT arena
(`fffffff000715b40`), moving between samples, stack healthy at
`ffffff800007fd40`. So the two populations the pixels suggested really do have
two mechanisms, and the earlier run that mapped the Z3 board after AROS stopped
belongs to this second one.

**What this means for the queue.** ISSUE-0002 (`STOP`/`wfi`) is not implicated in
the majority failure — the guest is not idle-waiting, it is drowning in
exceptions. ISSUE-0005 moves from a tidiness item to the first thing to look at.
And the ISSUE-0010 question — Paula shadow versus IPL injection — stops being an
architectural preference: the shadow's cost is a page fault per interrupt-register
access, and those faults are what the register dump shows at the moment of death.

## Not a leak, a collapse — and both populations share it

SP was added to the probe next to PC, because "the stack is exhausted" leaves
two very different defects on the table: a leak on some handler path, or genuine
recursion. Six runs settle it.

| | ARM stack used |
|---|---|
| normal operation, whole run | **704–1568 bytes**, flat, no drift |
| after the failure | **524416 bytes** = 512 KB + 128, the entire stack and past the floor |

The transition happens **between two five-second samples**. From about 700 bytes
to fully exhausted. That is not a leak — a leak would show as a downward drift
across the run and would drain a 512 KB stack long before 45 s at MMIO rates. It
is unbounded recursion, roughly 3200 nested 160-byte frames, effectively
instantaneous.

**And the `logo` run in that series carries the same signature.** So the two
populations separated earlier are very likely one defect firing at different
moments, not two mechanisms. It is not tied to Wanderer or to `LoadSeg`; those
are just where the boot happened to be.

The frame layout, from the disassembly, for anyone reading a stack dump:

```
curr_el_spx_sync:
  +0    stp x0, x1, [sp, #-160]!     <- where a stalled run is parked
  +16   x2,x3   +32 x4,x5   +48 x6,x7   +64 x8,x9
  +80   x10,x11 +96 x12,x13 +112 x14,x15 +128 x16,x17  +144 x18,x30
  +0x28 mov x0, #0x200 ; mov x1, sp ; bl SYSHandler ; b ExceptionExit
```

`X00 = 0x200` in every stall dump is that constant, and `X30 = ...1a4234` is the
return address after the `bl`. One captured stall has `X30` inside `SYSHandler`
itself (`ffffff8000089f14`), which places the re-entry inside the handler rather
than at its entry.

`x4` sits at offset 32 of each frame, so a stack dump shows what every nested
frame was working on. The harness now captures two 64-quadword windows into the
stack on any non-`icons` run — but no stall has been caught since it was added
(the following series returned 4 `icons` out of 4, which is itself a reminder of
how wide the variance is).

## Two questions closed by reading the arbitration (2026-08-06)

**66 versus 68 is settled: 68 is right.** `ExecutionLoop.c:424-426` pushes the
interrupt frame itself:

```c
strh SRcopy, [sp, #-8]!     /* SR   at +0 */
str  PC,     [sp, #2]       /* PC   at +2 */
strh vector, [sp, #6]       /* format/vector at +6 */
```

Eight bytes — a 68010-style format-0 frame, not the 68000's six. A scheduler
saving 60 bytes of registers plus the frame therefore needs **68**, and a
66-byte save is assuming a 6-byte frame this machine never produces. The change
parked on `codex-2026-08-05` was right about this specific point; that says
nothing about the rest of that backend, which is still unassessed.

**ARM interrupts are not left masked.** The IRQ fast path sets `SPSR.I` before
`eret`, and the only `msr daifclr` in the whole 4 MB binary is inside
`StartupPPC` — the PowerPC path. That looked like host interrupts could only
ever be delivered once. Measured instead of argued: sampling `PSTATE` across
runs shows `0x...0205` (I clear, interrupts enabled) throughout normal
execution, `0x...0285` occasionally when a sample lands inside the handler, and
`0x...03c5` — all of DAIF masked — only after the collapse, which is what being
stuck in the exception loop looks like. Something clears the bit that static
inspection does not show, most likely emitted by the JIT at run time and so
absent from the ELF. The worry was unfounded.

## The first fault, caught — two defects, not one (2026-08-06)

A depth guard in `SYSHandler` (`patches/emu68/0004`) reports the **first**
re-entry instead of leaving 3200 identical frames behind, and it caught the same
thing in three stalls out of three:

```
[JIT:SYS] RE-ENTERED at depth 1 on core 0
[JIT:SYS]   vector=00000200 ELR=0xffffff80000853d8 ESR=96000010
            FAR=0xffffff9064656275 SPSR=600003c5
```

`ESR 0x96000010` decodes as a data abort at the same EL, **write**, DFSC 0x10 —
a synchronous external abort, which is what an unassigned physical address
produces. And `FAR` is the guest-memory alias base `0xffffff9000000000` plus a
guest address:

| run | guest address | as bytes |
|---|---|---|
| 1 | `0x64656275` | `64 65 62 75` = **"debu"** |
| 2 | `0xc0000058` | |
| 3 | `0xfeb80051` | |

`0x64656275` is a pointer to the string "debug" being used as an address. The
other two are equally out of range — the machine has 840 MB.

So the chain is: the guest writes to a wild address; the write faults into
`SYSHandler`; `SYSWriteValToAddr()` emulates it through the linear alias
(`*(uint8_t*)(far + 0xffffff9000000000) = value`); that address is unmapped;
the handler faults; recursion. Nothing about the interrupt path, which is why
moving to IPL did not change it.

**That separates two defects that have been one problem all along.**

**A — the guest goes wild.** A pointer holding string data is used as an
address. This is the lost context this issue is named after, and it is on the
AROS side. It is what has to be fixed for the boot to be reliable.

**B — Emu68 turns a wild guest write into an unbounded recursion.** A real 68k
would take a bus error the guest could trap. Here the handler dereferences the
faulting address without checking it is mapped, so the machine silently
destroys its own stack instead.

B is worth fixing on its own merits: it is what makes A undebuggable, and it
would do the same to any future wild pointer. And it is recognisable — this is
the **open-bus guard** that had drifted into `vectors.c` and was removed on
2026-08-05 because it made the desktop unreachable. The idea was right; the
implementation rejected anything outside `sys_memory` above 16 MB, which
included the framebuffer at `0x3c100000`, so every write to the display was
discarded. A correct version rejects what is genuinely unmapped and lets the
framebuffer and the peripheral window through.

With the guard in place the stack no longer collapses — `sp_used` is 4640 bytes
instead of 524416 — and the machine halts with a message rather than hanging.

## The 66-byte frame: fragile by pairing, not obviously broken

Worth writing down before it is tested, because the obvious reading is wrong.

`dispatch.S:57-62` rebuilds a **6-byte** frame and executes `rte`:

```
lea.l   %a5@(66),%a2      /* USP = frame + 66 */
move.l  %a5@+,%sp@-       /* PC (4) */
move.w  %a5@+,%sp@-       /* SR (2) */
movem.l %a5@,%d0-%d7/%a0-%a6
rte
```

Emu68 pushes an 8-byte format-0 frame (`ExecutionLoop.c:424-426`) and its `RTE`
consumes 8. Six pushed against eight popped looks like an immediate two-byte
error — but `switch.S:19-23` only ever *removes* six from the supervisor stack
when it saves:

```
move.l  %sp@+,%a5@(13*4)  /* PC */
move.w  %sp@+,%a5@-       /* SR */
```

so the format word is left sitting on the supervisor stack, and the `rte` that
follows the next dispatch consumes exactly that. **Entry and exit balance, as
long as they are paired.** That is why the port has been reaching the desktop at
all with a 66-byte context, and it is the answer to "it worked before without
the 68-byte change".

What it does not survive is a dispatch whose supervisor stack does not carry the
matching leftover — a task dispatched for the first time from a synthetic frame
(`kernel_cpu.c:100-107` builds 66 bytes), or any path where entry and exit are
not the same depth. Then `rte` reads the format word from whatever is below,
and the supervisor stack pointer moves by two.

Which makes it a **candidate for defect A**, not a proven cause: intermittent by
construction, and its failure mode is a wrong PC or a shifted stack — the shape
of the wild writes actually observed. It should be tested against the measured
rate rather than reasoned about further, and the 68-byte backend parked on
`codex-2026-08-05` is one way to test it.

## Defect B fixed: open bus instead of a fault (2026-08-06)

`patches/emu68/0005` checks the guest address against the MMU — `at s1e1r` /
`at s1e1w` and `PAR_EL1`, not a table of regions, so it cannot disagree with
what is actually mapped the way the earlier attempt did when it rejected the
framebuffer. An unmapped address is answered the way a 68k board answers an
unclaimed one: writes swallowed, reads all ones, reported on the console with
the count capped so a memory-sizing sweep cannot become the bottleneck.

Eight runs after:

| | before | after |
|---|---|---|
| ARM stack used, worst case | 524416 bytes (exhausted) | **720–1584 bytes** |
| PC at a stall | `curr_el_spx_sync+0`, frozen | inside the JIT arena, **moving** |
| icons | 3/6 | 3/8 |

**The emulator no longer destroys itself.** Every stall now leaves a guest that
is still executing, which is defect A in its pure form and, for the first time,
something that can be debugged rather than a machine that has already
overwritten the evidence.

**The rate is unchanged, and that is the expected result** — this fixes
survivability, not the guest bug.

**Not finished.** A single bounded re-entry still occurs (`RE-ENTERED at depth
1`, never `runaway`), and the addresses in the open-bus reports are
alias-shaped — `0xffffff908a100003` rather than `0x8a100003`. That means the
guard is catching the *second-level* fault, not preventing the first: something
in the handler dereferences before reaching the guarded fall-through.
`SYSPageFaultWriteHandler` opens with `LE32(*(uint32_t *)elr)` and there are
other dereferences besides. Finding and guarding the first one is the remaining
half.

## The guest addresses are string fragments (2026-08-06)

The first version of the open-bus guard tried to predict which addresses are
backed, with `at s1e1r` and `PAR_EL1`. It does not work, and the reason is worth
keeping: the alias is mapped as one blanket range, so translation always
succeeds. The abort comes from the **bus**, not the page tables — ESR DFSC 0x10,
a synchronous external abort, which is what unassigned physical space answers
with. No translation check could ever have seen it coming, and the `isb` it cost
on every emulated access bought nothing.

The working version does not predict. It catches the abort where it lands
inside `SYSHandler`, gives the guest all ones on a read and nothing on a write,
steps over the faulting instruction and carries on. With that, the reports name
**guest** addresses instead of alias-shaped ones, and they say something:

```
open bus read: guest 0x616c6e75      "alnu"
open bus read: guest 0x6b657400      "ket\0"
open bus read: guest 0x6b65740e      "ket\x0e"
open bus write: guest 0x6761e410     "ga.."
open bus read: guest 0xd080d080  /  0xd080d06e  /  0xffffffff
```

**The guest is dereferencing string data as pointers**, and the same string at
two offsets two bytes apart — `"ket\0"` and `"ket\x0e"`. A longword read
misaligned by two bytes returns exactly that: the tail of one value glued to the
head of the next, which for text is ASCII fragments.

That is what a two-byte frame error produces, and it is the strongest
corroboration so far for the 66-versus-68 hypothesis. It is corroboration, not
proof: nothing here shows *where* the misalignment comes from.

**State of the guard.** Most cases now recover and the boot carries on. Some
still reach the depth limit and halt deliberately (`runaway exception recursion,
halting core 0`) — an abort outside the alias window, or an instruction form the
step-over does not handle. Stack use stays at 736–4784 bytes either way, against
524416 before, so the emulator survives in both.

**Rate: 1 of 8 in this series**, against 3 of 8 in the previous one and a spread
from 0/3 to 4/4 across the day. That is noise at these sample sizes and should
not be read as the guard helping or hurting. One real risk is worth naming
rather than buried: stepping over a faulting instruction skips any side effect
it had, such as an addressing-mode writeback, so a wrong step-over could corrupt
where the old behaviour merely hung.

## What the failures look like once the emulator survives (2026-08-06)

With the open-bus recovery, the 68-byte frame and a 64 KB task stack in place,
the emulator no longer destroys itself and the **guest's own trap handler**
catches the failure and freezes with a register dump. That changes what there is
to look at.

Across one series of eight: **every failing run reports a CPU exception and
every successful run reports none.** All of them vector `0x2c` — which
`boot/trapprobe.c` prints as `frame[3] & 0x0fff`, the vector *offset*, so 0x2c/4
= vector 11, Line 1111 Emulator.

Two signatures:

| runs | PC | |
|---|---|---|
| 3 × `logo` | `0x0088520a`, identical, with identical registers | deterministic |
| 1 × `workbench` | `0xfffffe62` | wild |
| 1 × later run | `0x345fee1c` | wild |

**F-line here is a symptom, not an FPU gap, and the correction matters.** The
guest memory at `0x345fee1c` was read back through the alias:

```
ffffff90345fee10: 0x0000 0x7c49 0x0000 0x4000 0x0000 0x984c 0xffff 0xfcff
                                                            ^ PC
```

Zeros and scattered values — **data, not code**. The word at the PC happens to
be `0xffff`, which is in the F-line range, so the CPU raises vector 11. The
exception is what a wild PC produces when it lands on data whose bytes decode
that way. Nothing here says an FPU instruction was involved.

So this is defect A, unchanged in nature and much better instrumented: the PC
goes somewhere it should not, and now the guest says so with registers instead
of the emulator dying. The `logo` case is the more promising of the two to
chase, because it is **deterministic** — same PC, same registers, three runs out
of three.

## Chasing 0x0088520a: what the wild PC looks like up close

**`0xc0000058` is the first wrong thing, and it is specific.** Read exactly once
in each of the three `logo` failures and in **none** of the five successes, at
the same point every time -- inside a `Lddemon` library open, immediately after
`LDRequestObject()`, while the Shell is running the `If EXISTS "C:Decoration"`
builtin. It is not a legitimate probe, which is what the earlier note guessed
from seeing it in other series. The chain is: something computes
`0xc0000058`, reads it, gets open bus, and the PC goes wild.

**The F-line vector is a consequence of the open-bus policy, not a clue.** Two
more wild PCs were captured with guest memory:

| PC | guest memory there |
|---|---|
| `0x4cf40010` | `Cannot access memory` |
| `0x6c695fcc` | `Cannot access memory` — bytes `6c 69 5f` = `"li_"` |

Both are unmapped, so the instruction fetch returns open bus, which is all ones,
which is `0xFFFF` -- an F-line opcode. Vector 0x2c is therefore what *any* wild
PC into unmapped space now produces. That is a designed property and a good one:
it turns a wild jump into an immediate trap the guest reports with registers,
instead of the emulator hanging. It says nothing about why the PC was wrong.

`0x6c695fcc` is another string fragment, like the open-bus addresses before it.

**`0x0088520a` remains the best target** precisely because it is unlike these:
it is inside mapped RAM at 8.9 MB, above every library load address seen in the
same run (`0x0027`–`0x003a`), and it repeats exactly, with identical registers,
three runs out of three. Registers worth carrying forward:

```
A6 0x00002b58   A2 0x00002f04   A0 0x0000000b   <- far too low to be pointers
A1 0x0032904c = D3             A3 0x00355728   A4 0x003558bc  <- library region
D0 0x000008ff  D2 0x000008e3   D4 0x0000000c   D5 0x00000004
```

A6 holds a library base in this ABI, and `0x2b58` is not one.

## Root cause direction: the crash is inside InternalLoadSeg_ELF

The guest's trap handler prints USP, and the return addresses on the user stack
are code, so they symbolise against the m68k ELF this repository builds. The AROS
image loads at `0x34600000` (`[BOOT] Loading ELF executable ... to
0x0000000034600000`), so `addr - 0x34600000` indexes it directly. QEMU's `x/w`
reads little-endian, so the words need swapping first — without that the stack
looks like noise.

```
004041d8: 346aa5fc   InternalLoadSeg_ELF+0x336
0040425c: 346a340a   Dos_7_Read+0x3e
00404260: 7f454c46   "\x7fELF"
00404264: 0102010f   ELF32, MSB, version 1
00404270: 00010004   e_type = 1 (ET_REL), e_machine = 4 (EM_68K)
```

**The failure is inside `InternalLoadSeg_ELF`, loading a module from disk**, with
that module's ELF header sitting in a buffer on the stack. That matches the
serial exactly: the LDDemon was opening a library.

Two earlier readings of mine are wrong and are corrected here:

- **`A6 = 0x00002b58` is not corruption.** The same value is on the stack at
  `0x4041d4`, deliberately saved. It is 11096 — a size. In a stretch of compiled
  C, A6 is not a library base.
- **`0x0088520a` is not a random address.** At 8.9 MB it is in the region
  `InternalLoadSeg_ELF` allocates segments into.

**The hypothesis this supports**, and it is a hypothesis: the loader allocates a
segment, the read does not fill it, the segment stays zero, and the entry point
is jumped into anyway. Running through zeros to the first non-zero word is
exactly what the memory dump shows.

If that is right, the defect is in what `Dos_7_Read` returns — which is the
fat-handler path, and therefore **ISSUE-0009**. The two investigations may be
one.

## What to do next, without more runs

- Instrument `InternalLoadSeg_ELF`'s read results rather than sampling boots:
  the sizes requested and returned, per section. A short read proves it.
- The recovery path's `open_bus_report` is called with `size` 0, so the width of
  the `0xc0000058` access is not recorded. One line, and it narrows what code
  could be making it.

## What the ELF-loader trace says (2026-08-06)

`patches/aros/0008` traces every read the loader makes: each `ilsRead` request
and result, each `elf_read_block` with the small-read buffer's state, and each
section load with its file offset, size and destination. Three runs, ~1600-2600
trace lines each.

**Established, and it kills the simple version of the hypothesis:**

- **No section load is ever short.** `read_block` returns the full count on
  every direct read, in all three runs. A `load_hunk` of 4326 bytes reads 4326.
  So "the segment was allocated and not filled" is **not** what happens — at
  least not through a short read.
- **The 4 KB small-read fills fail constantly and benignly.** 127 failures in
  one run, every one of them at end of file: a fill of 4096 at offset 4378 gets
  3434 and then 0, because the file is 7812 bytes long. `elf_read_block`
  discards that result, and `srb_Buffer` comes from `AllocMem(MEMF_ANY)`, so the
  tail is uninitialised. Every cached read seen by hand afterwards lands inside
  the part that was validly read.

**Suspected, and explicitly not established:** whether *any* cached read reaches
past the validly-filled part of the small-read buffer. A crude scan of the trace
flags 193-312 candidates per run, but the scan's tracking of how much of the
buffer is valid is unreliable across refills and should not be quoted. Making
that detector correct is the next concrete step, and it is analysis of logs
already captured, not more boots.

The shape of the upstream defect is worth stating plainly whatever the answer:
`elf_read_block` ignores the return of the fill, and the buffer it fills is not
cleared. On a filesystem that reads short for any reason other than EOF, that
hands uninitialised memory to the loader as file contents. This port is exactly
the environment where a filesystem might do that — see ISSUE-0009.

## Confirmed: the ELF loader serves uninitialised memory as file contents

The suspicion recorded above — that a cached read might reach past the filled
part of the small-read buffer — is now established, from the logs already
captured rather than from new runs. A correct detector, tracking how many bytes
each 4 KB fill actually delivered, finds it **twice per boot at the same offsets
in all three traced runs**:

```
off=7412  size=400  buffer filled from 5688,  1904 valid  ->  220 bytes past
off=32292 size=520  buffer filled from 30344, 1172 valid  -> 1296 bytes past
```

`elf_read_block()` bounds its cache by `LOADSEG_SMALL_READ` instead of by how
much was read, `srb_Buffer` comes from `AllocMem(MEMF_ANY)` and is never
cleared, and the fill's return value is discarded. So 220 and 1296 bytes of
whatever was in that memory are handed to the ELF loader as file contents, with
no error raised anywhere. That is a mechanism for a loaded module containing
something other than the file — which is what the crash looks like.

`patches/aros/0009` records how much the fill delivered and bounds the cache by
that. A short fill stays what it is, normal at end of file, and only limits what
may be served; a request the file genuinely cannot satisfy now reports
`ERROR_BAD_HUNK` rather than inventing the bytes. The recursive re-read goes with
it — the fill starts at the requested offset, so the request is at the head of
the buffer — which also removes the chance of recursing forever when a refill
cannot satisfy the request either.

**The fix was verified against the marker, on `experiment/frame-68`** — the same
change carried on top of the LSREAD tracing, so the count could be read directly
rather than inferred. Three runs: reads past the filled part go from **2 per boot
to 0**, and no request is refused for want of bytes. The loader stops receiving
uninitialised memory as file contents.

**Whether it changes the boot rate is not measured yet, and those three runs do
not say.** All three failed to reach the icons, and that is not a rate: the build
carried ~2000 lines of tracing per boot plus the 68-byte backend and a 64 KB
stack, so it is not comparable to `main`, and three failures at a ~38% base rate
happen a quarter of the time anyway. What is established is a confirmed defect,
a confirmed mechanism, and a fix confirmed to remove it — none of which is the
same as being the cause of the intermittency. The day's record is full of reasons
not to conflate them.

## The reference is no better: 5 of 13 (2026-08-06)

The premise underneath this issue — that something was lost and the reference
pair reaches the desktop — had never been measured. It is now, with the same
harness, the same card protocol and an idle machine, against
`/home/jaime/Emu68` `8946834` plus the ELF from
`/home/jaime/aros-build-emu68-m68k`:

| | n | icons | rate |
|---|---|---|---|
| reference pair | 13 | 5 | **38%** |
| this repository | 101 | ~38 | **~38%** |

Same rate. Same two failure modes, `logo` and `workbench`. All thirteen runs
carry identical build hashes, so it is one clean set rather than a mixture.

**So there is no regression, and this issue's title is misleading.** Nothing was
lost; the port has never booted reliably. That does not make the three defects
closed today less real — they were real, and one of them (the ELF loader serving
uninitialised memory) is a genuine upstream AROS bug. It does mean the framing
that drove much of this work, *find what broke*, was the wrong one, and the
right one is *finish what was never finished*.

The reference is a little faster to the icons — 38.6–49.3 s against 46–55 s here
— which is worth noting and is not the question.

## The raw cluster comparison is accidentally load-bearing (2026-08-06)

Promoting three fixes to `main` without measuring them together took the boot
from ~38% to **0 of 13**. A bisect found it, and it was not what either of us
guessed:

| configuration | icons |
|---|---|
| 0007 + 0008 (full FAT) + 0009 | 0 / 13 |
| 0007 + 0008 (full FAT) | 0 / 6 |
| 0007 + 0008 without `date.c` | 0 / 6 |
| 0007 alone | 3 / 6 |
| 0007 + 0008 **write side only** | 3 / 6 |
| 0007 + 0008 write-only + 0009 | 2 / 5 |

**The FAT patch, and inside it neither the date conversion nor the write
conversion — the read-side substitution.** Replacing

```c
if (de->e.entry.first_cluster_hi == (cluster >> 16)
    && de->e.entry.first_cluster_lo == (cluster & 0xffff))
```

with `FIRST_FILE_CLUSTER(de) == cluster` in `GetDirEntryByCluster`, and the
matching one in Rename, is what does it. Those comparisons put a
little-endian field against a native value, so on a big-endian host they
**never match**: `GetParentDir`'s search has always failed silently here.
Making it correct wakes a code path that has never run on this port and does
not work.

So the wrong comparison is holding the boot up. Fixing it is a separate
problem, and a real one — parent-directory resolution is broken on every
big-endian AROS target and has presumably never been exercised.

Two corrections to this issue's own record follow:

- `docs/known-good-baseline.md` said, from the beginning, that only the write
  half of the FAT change is worth keeping. **That was right.** The
  reinterpretation written earlier today — that the original three-of-three
  failure had been misattributed to bundled MUI tracing — was itself wrong, or
  at least incomplete: the FAT change does break the boot, and the bundling was
  a red herring.
- The lesson stands and is now doubled. A measurement of a bundled change
  attributes to all of it, and promoting three patches together after measuring
  none of them together is the same error in the other direction.

## What the raw comparison was masking (2026-08-06)

Traced with the read-side fix deliberately re-applied, because that makes the
failure deterministic — 0 of 13 — and a failure that happens every time is where
tracing is worth spending. (Blanket FAT tracing was tried first: 70k lines, so
slow the boot never reached the failure. The instrument changed the thing being
measured. Surgical tracing on the parent-dir path only, four `bug()` calls, gave
the answer.)

**The fix is correct and works.** `OpLockParent` is called **860 to 1709 times
per boot** — DOS leans on `ParentDir()` constantly — and with the corrected
comparison the deep search succeeds 76 to 193 times, with the right names and
zero errors:

```
search for cluster 23061 in grandparent 2     -> err=0 name='PREFS'
search for cluster 23209 in grandparent 23061 -> err=0 name='ENV-AR~1'
search for cluster 23212 in grandparent 23209 -> err=0 name='CLASSES'
```

**And that is exactly why the boot breaks.** Those paths are
`SYS:Prefs/Env-Archive/Classes` — the persistent environment-variable tree.
Before the fix, `ParentDir()` failed for everything two or more levels deep, so
that traversal aborted immediately and the boot carried on. With FAT correct the
traversal proceeds, and what breaks is *in the traversal*, not in FAT.

**So the broken comparison was holding the boot up by masking a second defect.**
Two bugs stacked: parent-directory resolution has never worked on any
big-endian AROS target, and whatever walks the Env-Archive tree cannot survive
it working.

That is where the next session starts, and it starts from a deterministic
reproduction rather than a 38% one: apply the read-side fix, and the failure is
guaranteed.

## The whole chain, and a minimal boot that splits the failure (2026-08-06)

Tracing the parent-dir path with the FAT read fix applied gave the complete
causal chain, and it ends somewhere none of the day's hypotheses pointed:

1. `rom/filesys/fat`'s cluster comparison puts a little-endian field against a
   native value, so on big-endian it **never matches**. `GetParentDir()` fails
   for anything two or more levels deep, and with it `OpLockParent()` — which
   DOS calls **860 to 1709 times per boot**.
2. `Copy >NIL: "ENVARC:" "ENV:" ALL` (Startup-Sequence line 61) therefore
   copies little or nothing, and `ENV:` stays empty.
3. `If EXISTS "ENV:SYS/theme.var"` is false, so the boot takes the `Else` and
   assigns `THEME:` to `THEMES:AROSDefault`.
4. Fix the comparison and the copy works — measured: the deep search succeeds
   76 to 193 times per boot, with the right names (`PREFS`, `ENV-AR~1`,
   `CLASSES`) and zero errors. `theme.var` now exists and names `THEMES:Ice`.
5. `C:Decoration` loads that theme, and the boot dies there. Every time, 13 of
   13.

Confirmed by the difference in the logs: working runs count `AROSDefault` four
times (the `Else` ran), failing runs count it zero (the `If` ran). And switching
`theme.var` to `AROSDefault` with the fix still applied changes the outcome —
from 13 of 13 dying early to reaching the Workbench screen — so the theme is a
large part of it, though not all.

**So the broken comparison has been holding the boot up by leaving half the
system uninitialised.** Fixing FAT does not introduce bugs; it exposes ones that
were always there in code this port has never run.

## The minimal boot

`AI_context/bringup/Startup-Sequence.minimal` — 25 lines against the stock 120,
the assigns Wanderer needs and nothing else. Six runs:

| | stock | minimal |
|---|---|---|
| screen opens | 38–50 s | **17–22 s** |
| icons | 46–55 s | **33–39 s** |
| `logo` failures | common | **none** |
| icons | ~38% | 2 of 6 |

It does not make the boot reliable, and it **splits the failure in two**: every
early death comes from the discretionary part of the startup sequence, and what
is left is one late failure — Wanderer opens its screen, always and much
sooner, then does not draw icons.

Next step, and it needs no new tooling: add the removed steps back one at a
time until `logo` returns.

## Named at last: bad pointers reaching tlsf_freevec (2026-08-06)

`patches/emu68/0005` now prints the **guest PC** alongside every open-bus
access. Emu68 keeps the m68k context in a reserved register and the AROS image
loads at `0x34600000`, so the address resolves against the ELF this repository
builds. That turns the most persistent artefact of the whole investigation into
a function name.

Six runs on the minimal boot, 4 `icons` / 2 `workbench`:

```
icons      guest=0xc0000058  m68kPC=3460379a  tlsf_freevec+0x11e
workbench  guest=0x70ff4e81  m68kPC=3460388a  tlsf_freevec+0x20e
workbench  guest=0x69b0dde0  m68kPC=3460377a  tlsf_freevec+0xfe
workbench  guest=0x42617274  m68kPC=3460388a  tlsf_freevec+0x20e   "Bart"
workbench  guest=0x70742052  m68kPC=346b2eb4  LibNextTagItem+0x0    "pt R"
workbench  guest=0xfffffd64  m68kPC=fffffd64  (PC already lost)
```

**It is the allocator.** `rom/kernel/tlsf.c:663`, `tlsf_freevec(mhe, ptr)`, does
`fb = MEM_TO_BHDR(ptr)` and then reads the block header. A garbage `ptr` gives a
garbage header address, and reading it is the open-bus access.

So the diagnosis is sharper than "heap corruption": **something calls free with a
pointer that is not one** — and the values are text (`"Bart"`, `"pt R"`), so a
pointer variable is holding string data when it is freed. Uninitialised, or used
after free, or read from the wrong offset of a structure.

That accounts for the whole shape of the intermittency: which block is hit
depends on where the heap happens to lie, so it fails in different places on
different runs, with pointers that look like fragments of strings.

**And `patches/emu68/0005` is why any of this is visible.** Before the open-bus
recovery, the first such free hung the machine with 3200 nested exceptions and
no evidence. Now it survives, reports, and names the caller.

**The strongest single lead is the benign case.** Every *successful* run makes
exactly one of these, always the same: `0xc0000058` from `tlsf_freevec+0x11e`.
Same address, same site, every time. That is not random corruption — it looks
like one field read before it is initialised. Being deterministic, it is far
easier to chase than the destructive form, and it may be the same defect.

## The 68-byte context removes the deterministic bad free (2026-08-06)

Measured with the marker rather than the rate, which is what the earlier
judgement got wrong. Six runs each on the minimal boot:

| | runs containing `0xc0000058` | occurrences |
|---|---|---|
| 66-byte frame | **4 of 6** | 4 |
| 68-byte frame | **0 of 6** | 0 |

The deterministic bad pointer reaching `tlsf_freevec+0x11e` — present in every
successful run under the 66-byte scheme, always the same address, always the
same site — **is gone entirely** with the 68-byte context.

**The mechanism fits what was already established.** The 66-byte scheme balances
only while entry and exit are paired; when they are not, the restored frame is
shifted by two bytes, and a register block read two bytes off yields registers
holding halves of adjacent values. That is exactly the signature seen all day —
`"ket\0"` and `"ket\x0e"`, the same string at two offsets two bytes apart, and
pointers whose contents are text.

**And it corrects an earlier judgement in this issue.** The 68-byte backend was
dismissed on 3-of-8 against 4-of-8 icons. At a ~40% base rate, n=8 cannot
resolve anything; that was the rate protocol being violated one hour after it
was written down. A marker going 4/6 to 0/6 answers in six runs what the rate
could not answer in sixteen.

Two things it does **not** do, and both matter:

- **The icon rate does not visibly improve** — 3 of 6 against 4 of 6, noise.
- **Bad pointers still reach the allocator**, at other addresses:
  `0xc2a80053`, `0x44f40000`, `0x6d74616e` (`"mtan"`), `0xf3000201`. So the
  frame is one source of them, not the only one.

`frame_bad` from the validator is 0 across all six, so with `AROS_STACKSIZE` at
40960 no frame lands outside its task's stack either.

## Other frame formats: a real defect, but latent (2026-08-06)

Emu68 generates more than one exception frame size. `M68k_Exception.c` builds
format 0 (8 bytes), and format 2 (12 bytes) for exactly three vectors —
`VECTOR_DIVIDE_BY_ZERO`, `VECTOR_CHK`, `VECTOR_TRAPcc` — with formats 3 and 4
also implemented. Its `RTE` validates the format nibble and accepts 0 and 2,
raising `VECTOR_FORMAT_ERROR` otherwise (`M68k_LINE4.c:1672`).

**`EMU68_SAVE_FRAME` assumes 8 bytes always.** It copies the format word
verbatim and does `lea.l %sp@(8),%sp`. Given a format-2 frame it would save 8
of 12 bytes while keeping a format word that says twelve, so the restore pushes
8 and the `RTE` pops 12 — four bytes of drift, the same class of defect as the
66-versus-68 one in a different size.

**Measured, and it does not happen.** A check in `EMU68_SAVE_FRAME` reports any
frame whose format nibble is not 0; three runs, **zero reports**. Divide by
zero, `CHK` and `TRAPcc` are error conditions and none of them reaches a task
switch in this boot.

So it is latent, not the source of the remaining bad pointers. The check stays —
it costs two instructions on a path that already touches that word, is silent
unless something changes, and the day this port executes a `CHK` it will say so
rather than corrupting a stack quietly.

A good hypothesis, closed by measurement rather than left hanging.

## The remaining bad pointers: several sites, one signature (2026-08-06)

`patches/aros/0010` validates the pointer in `tlsf_freevec` against the pool's
**area list** and reports the caller. The first version of that check used
`mh_Lower`/`mh_Upper` and was wrong — those describe only the MemHeader the pool
started from, and TLSF adds areas beyond it, so it reported four *legitimate*
frees per boot as bad and declined them. Corrected, it reports nothing: no bad
pointer reaches `tlsf_freevec` from outside the heap.

With the guest PC on every open-bus access, the remaining events name several
independent sites:

```
  8x  strcmp+0x0
  6x  PC fffffd5a (already lost)
  3x  tlsf_freevec+0x104
  2x  tlsf_freevec+0x214
  2x  do_render_with_gc+0x2d4
  1x  do_render_with_gc+0x1b6
```

The allocator, string comparison, and graphics rendering — **independent
consumers**, which means general memory corruption rather than one bad caller.
The `tlsf_freevec` ones now pass the area check, so the pointer is in the heap
and the *block header* is corrupt.

**And the addresses carry a signature.** Five of the eight `strcmp` pointers are
`0x8a2c0000`, `0x6c5c0000`, `0xb51c0000`, `0x974c0000`, `0x41940000` — the low
word is zero in each. That is `(16-bit value) << 16`: a longword read **two
bytes past** where the pointer is, taking the pointer's low half as its high
half and a zero word after it.

Two bytes off, again. The 68-byte context removed one source of that; **there is
another**, and it is the next thing to find. The signature is specific enough to
recognise: pointers of the form `0xNNNN0000`, and text fragments at two offsets
two bytes apart.

## Dead 66-byte code was being linked into every image (2026-08-06)

`arch/m68k-all/kernel/kernel_cpu.c` carries three things: the `cpu_Exception`
trampoline, and `m68k_SwitchTail`/`m68k_DispatchFrame` — the 66-byte scheduler
policy, including the literal `for (i = 0; i < 66; i++)` frame copy.

This port replaces the scheduler in `rom/exec` (`arch/m68k-emu68/exec/`), and
`%build_archspecific` overrides by writing the same object path, so
`switch.o`/`dispatch.o` there are ours. But `kernel_cpu.c` belongs to the
**kernel** module, which the exec-side override does not reach. Both 66-byte
functions were therefore linked into every image, with no caller.

**That is not merely wasteful.** Reading `for (i = 0; i < 66; i++)` in a linked
object sent this investigation down a blind alley: the 66-byte copy looked live
and was a plausible second source of two-byte drift, and it took disassembling
`__Dispatch_this` (`lea %a5@(68)`) and `m68k_VoluntarySwitch` to establish that
neither is reached.

Fixed by giving the kernel module its own `kernel_cpu.c` for this target, with
`cpu_Exception` and nothing else. `cpu_Exception` is genuinely live —
`context.c` installs it as the return address of a shifted frame when a task has
`TF_EXCEPT`. Confirmed: `m68k_DispatchFrame` and `m68k_SwitchTail` are gone from
the image, `cpu_Exception` remains, and 4 runs behave as before.

The rule this earns: **an override that replaces a policy must replace it in
every module that carries a copy**, or the old one ships alongside the new one
and reads as current to whoever looks next.

## Correction: the "symbol-set entry" attribution does not hold (2026-08-07)

The section below concluded, from one sample, that the caller was
`set_call_devfuncs` walking a module symbol set and calling a garbage entry.
**It is wrong, and the method that produced it is the reason.**

The return address was taken as the longword at the guest's A7. That is only
the return address for a leaf call with nothing else pushed; otherwise A7 points
at something else entirely. Across more samples the value is frequently `0` —
not an address at all. One sample happened to land on something that resolved
inside a module, and I read a mechanism into it.

Tested directly rather than argued: `set_call_devfuncs` was instrumented to dump
the set it walks, the module rebuilt with it (the *reference distribution*
supplies every module, so ours had to be installed over it), and three runs
produced **zero** dumps. The function is never called — a `.mui` class is a
library and uses a different entry point.

So the caller of the bad pointer is still unknown. What survives from that
section is only what was measured: the guest PC at the fault, which names the
victim.

**Two things worth keeping from the attempt.** A7 is unreliable as a return
address and should not be reported as one. And a fact that had gone unnoticed
all day: the kernel ELF is built here, but **every module on the card comes from
the reference distribution** — `patches/aros/*` touching module code has no
effect on what boots unless the module is rebuilt and installed over the
reference tree.

## Superseded: the caller, named (2026-08-06)

`patches/emu68/0005` now also reads the guest's A7 — which lives in an ARM
register while the JIT runs, so it is read from there rather than from the
stale saved context — and the longword at the top of that stack, which for a
leaf call is the return address. That names the **caller**, where the guest PC
only names the victim.

One failing run, six events, all the same:

```
open bus read: guest a3e7fd78  m68kPC a3e7fd78  ret 0053abc2
```

The PC is already lost. The return address is not, and it resolves:

```
0x0053abc2 = IconListview.mui seg 0x53a818 + 0x13aa
0x13aa     -> set_call_devfuncs+0x40   (spans 0x136a..0x1400)
```

`set_call_devfuncs` is from `compiler/libinit/libinit.c` — the generated module
startup glue linked into every module — and what it does is **walk a symbol set
and call each function pointer in it**.

So the mechanism is: a module is loaded, its init walks its symbol set, calls an
entry, and **the entry is garbage**. The PC lands in nowhere and everything after
that is consequence.

Symbol sets are arrays of function pointers the linker places in dedicated
sections. They depend entirely on the ELF loader handling sections and
relocations correctly — which is where a real defect was already found and fixed
today (`patches/aros/0009`, uninitialised buffer served as file contents). That
one is fixed; whether the sets are still being built wrong is the next question,
and it is a much narrower one than "the boot is intermittent".

**Why this took all day to reach**, worth recording as method rather than
excuse: the failure destroyed its own evidence. Each layer of instrumentation
made the next one possible — open-bus recovery so the machine survives, the
guest PC so the victim has a name, the guest A7 so the caller does. None of
those could have been written first.

## The distribution is built here now (2026-08-07)

The finding that ended 2026-08-06 was that **the kernel ELF was built here and
every module on the card came from a foreign reference tree** — libraries, Zune
classes, the commands in `C:`. Any patch touching module code changed nothing
that booted, and said nothing about it. That was discovered by instrumenting
`libinit.c` and getting no output at all: the module carrying the change was
never on the card.

The cause was in this repository's own `arch/m68k-emu68/mmakefile.src`:

```
#MM- AROS-emu68-m68k : kernel-link-emu68-m68k
```

`AROS-<target>` — the whole-distribution metatarget — was declared as depending
on the link and nothing else. `make AROS-emu68-m68k` was therefore an expensive
way to rebuild the same ELF, and the distribution tree never gained `S/`,
`Fonts/`, `Storage/`, `Utilities/`, `Tools/` or `AROS.boot`.

Fixed by declaring what the other targets declare, shaped after
`arch/m68k-amiga` minus the Amiga packaging and demos:

```
#MM- AROS-emu68-m68k : kernel-link-emu68-m68k software-emu68-m68k
#MM- software-emu68-m68k : general-setup boot workbench-emu68-m68k
#MM- workbench-emu68-m68k : workbench-complete workbench
```

It builds clean — 24k lines of log, zero errors, including external ports
(zstd, bzip2, freetype2, openurl) that had never been built for this target.
The result is 361 MB against the reference's 364 MB, and **a card made from it
boots**: 2 of 6 to icons, the same band as everything else, so changing the
source of every module regressed nothing.

`scripts/build-aros.sh full` builds it; `make-sdcard.sh` now defaults to it.

**First attempt was three libraries short, and said nothing about it.** The tree
came out with 59 libraries against the reference's 62, missing
`posixc.library`, `stdcio.library` and `openurl.library` — they hang off the
generic `AROS` metatarget (`#MM- AROS : compiler-posixc`) rather than off
`AROS-<target>`, so `workbench-complete workbench` never reaches them. Naming
them explicitly fixes it, and the `Libs` listing now matches the reference
exactly, 61 `.library` files on the card and in the tree.

Worth keeping for the shape of it: **24k lines of build log, zero errors, and
three files short.** The symptom surfaced 350 MB later as a boot stopping while
waiting for `posixc.library`. A build that succeeds is not a build that is
complete, and on this target the difference is invisible until something tries
to run.

**What this unblocks.** Every patch in `patches/aros/` that touches a library, a
Zune class or a command now reaches what boots. Most of the day's instrumentation
could not have worked, and one attempt at it silently did not.

## A candidate for the remaining corruption, from upstream (2026-08-07)

Auditing 359 upstream commits since our pin turned up `b553067c52`, and it
describes this issue's open blocker in its own words:

> A short free leaves the allocator's chunk boundaries out of step with the real
> allocations, so the damage lands on an unrelated free much later, **as a block
> that belongs to no MemHeader or one that overlaps its neighbour**.

`AllocBitMap()` reserves room for the extra plane pointers a HIDD bitmap needs,
and each of its cleanup paths frees only `sizeof(struct BitMap)` — 32 bytes short
on m68k. Verified present in our tree at `rom/graphics/allocbitmap.c:480`, `:515`
and `:534`. `freebitmap.c` has the sizes right, so it fires only when creating a
bitmap *fails*.

That is the same shape as "The remaining bad pointers: several sites, one
signature": damage far from its cause, in consumers with nothing in common.

**Not established: that those failure paths run during our boot.** Plausible on a
machine with no chipset and a display path that has been marginal throughout, but
plausible is not measured. Testable the ordinary way — take it alone, three runs,
see whether the bad-pointer reports stop.

Three other defects were verified present in our tree at the same time, including
`workbench/c/iprefs/main.c` running its first prefs pass *before* `Detach()` —
which is the shape of the boot going quiet after IPrefs, recorded here more than
once. The full ledger, with what to take and what to skip, is **ISSUE-0015**.

## The parked branch is now empty of unincorporated work (2026-08-07)

`codex-2026-08-05` was audited item by item against `main`. It is a park, not a
proposal — two commits over the merge base `2329b18`, while `main` has moved a
long way past it. **One thing in it was missing from `main`**, and it is now
`patches/aros/0011`.

**`rom/filesys/fat/date.c` — the directory dates were exchanged in host order,
and we had already fixed this once and then lost it.** That correction matters
more than the fix. The history:

- `1772a01` (2026-08-06) added `0008-fat-convert-on-disk-fields-little-endian`,
  covering `date.c`, `direntry.c` and `ops.c`.
- `c169cd9`, later the same day — *"keep only the write half, the read
  substitution breaks the boot"* — deleted that patch and wrote today's `0008`
  in its place. **`date.c` went with it as collateral.** What the 13-of-13
  measurement condemned was the read-side `FIRST_FILE_CLUSTER` substitution;
  `date.c` was in the same patch file and was cut with it.
- `c169cd9`'s own message says so: *"Converting them was measured too, and is
  not what broke the boot."* It knew, and cut anyway.

So this is not codex work being adopted. It is our own work being restored after
a retreat took more ground than it needed to. **A patch file is the unit of
revert, so anything bundled into one shares its verdict** — which is the same
attribution failure `1772a01` had already caught once, in the other direction,
when MUI tracing bundled with a FAT change made the FAT change look like a
regression.

`0008` stopping where it does is still right on its own terms: the date and time
words are *symmetrically* unconverted, so AROS round-trips them among itself and
the boot never noticed. Nothing else that reads the card can. Every timestamp we
write is byte-swapped on disk, and every timestamp written by anything else reads
back here as a nonsense date.

It is not a boot blocker and is not promoted as one. The failure is graceful by
construction: `ConvertFATDate` range-checks the decoded fields and substitutes
01-01-1978 when they are impossible, which a swapped date almost always is. The
symptom is uniformly wrong file dates, not a hang.

The conversion goes *inside* `ConvertFATDate` and `ConvertDOSDate` rather than at
their call sites, and that placement is what makes it complete. Every producer
and consumer of these fields in `rom/filesys/fat` goes through those two —
`direntry.c:570,683`, `volume.c:506`, `ops.c:1179`. The other mentions of
`write_date`, `create_date` and `last_access_date` are field-to-field copies
(`Rename`, `ops.c:601-606`) or local temporaries holding the on-disk value
between a read and a write-back (`direntry.c:568-578`, `ops.c:1177-1183`). None
interprets the value, so the symmetry AROS relied on survives. Verified to build:
`make kernel-fs-fat` relinks `L/fat-handler`.

This does **not** answer the standing oddity that AROS stamps files with the real
host wall-clock time although the Pi has no RTC and `readbattclock.c` returns 0
for this architecture. That is about the *value*, not the byte order, and stays
open.

**Everything else on that branch is accounted for, and none of it should be
taken:**

| what | why not |
|---|---|
| `direntry.c` read side (`FIRST_FILE_CLUSTER` substitution) | excluded by measurement — it is the change that took the boot to 13 of 13 failures; see "The raw cluster comparison is accidentally load-bearing" |
| MUI, Wanderer, Shell, IPrefs — 12 files | pure `bug()` tracing and `#define DEBUG 1`, no logic |
| `arch/m68k-all/{signal_fast,wait,switch}.S`, `kernel_cpu.c` | removes the `#ifndef __EMU68__` guards, i.e. restores the Paula `INTENA` writes — superseded by the `arch/m68k-emu68/exec/` backend and the IPL decision |
| `platform.c`, `system_timer.c` EXTER arm/ack | the Paula-shadow protocol that was abandoned for IPL |
| `interrupt_controller.c` +18, `PLATFORM_TRACE_BRINGUP 1` | debug scaffolding (IRQ62 counter, Arasan registers) |
| `patches/emu68/0002-offer-zorro3-rom-board`, `0003-trim-standalone-module-list` | moot: `src/boards/emu68rom.c` only enters `BASE_FILES` inside the PiStorm branch of `CMakeLists.txt:266-267`, so the standalone variant never compiles it |
| `emu68-submodule-drift.patch`, both docs, ISSUE-0004 and ISSUE-0008 | already on `main`; the two issues are closed and in `consolidated/history/` |

The 4-line difference in `patches/emu68/0001` between the branches is hunk
offsets, not content.

**The branch is deleted**, locally and on `origin`, and deliberately *not* kept
as a tag. It was scaffolding from the start — a place to put a working tree that
the repository could not represent, so that a rebuild would not destroy it
silently. That job is done. This is not the `legacy` branch, which is an archive
of a real code line and is tagged `legacy-2026-08-03` because it is meant to be
retrievable; nothing about codex was meant to be kept.

Earlier sections of this issue and the closed ISSUE-0008 and ISSUE-0009 still
name the branch. Those references are historical and stay as written — they
record where something was at the time, and the table above records where each
piece of it ended up.

## Where this work lives, 2026-08-06

Not all of the day's findings are on `main`, and the split is deliberate.

**On `main`:** the IPL delivery path (`patches/emu68/0002`, `0003`), the
`SYSHandler` re-entry guard (`0004`), the open-bus recovery (`0005`), the task
stack raised to 40960 (`patches/aros/0007`), the FAT byte-order fix
(`patches/aros/0008`), and the harness with all of its probes.

**On `experiment/frame-68`, and not promoted:** the 68-byte Exec backend with
its frame validator, and `patches/aros/0009`, the ELF-loader read tracing. The
backend is more correct than the 66-byte scheme — self-contained, where that one
balances only while entry and exit are paired — but it **does not change the
boot rate**, and the validator that made it worth running has already produced
its finding (ISSUE-0014). The tracing is diagnostic and marked for removal.

Anything quoting a `[EMU68-FRAME]` or `[LSREAD]` line in this issue was measured
on that branch, not on `main`.

> **Overtaken by events, 2026-08-07.** Both were promoted afterwards: the 68-byte
> backend is on `main` in `aros/arch/m68k-emu68/exec/` — with the INTENA writes
> stripped, the per-task tracing removed, and a `kernel_cpu.c` that keeps the
> dead 66-byte pair out of the ELF — and the ELF-loader fix is `patches/aros/0009`
> there, in a clean form the branch's version does not have. Audited 2026-08-07:
> `main` is a strict superset of that branch on every shared file. The only thing
> left on it is `0008-debug-trace-elf-loader-reads.patch`, the LSREAD instrument
> itself, which is scaffolding that has already produced its finding — the
> verification it produced is recorded above, under "Confirmed: the ELF loader
> serves uninitialised memory as file contents".

## The Zorro III board is out of the build (2026-08-06)

`patches/emu68/0002` offered the Z3 ROM board to a standalone guest and `0003`
narrowed what it carried. Both are removed from the series. The reasoning, in
the order it was established:

- **This port never asked for it.** No `expansion.library`, no autoconfig sweep,
  zero occurrences of `expansion`/`zorro`/`autoconfig` in any serial log. The
  FDT arrives in `A6` from Emu68's initramfs loader (`boot/entry.S`), and the SD
  card is driven by our own `soc/sdcard`. Nothing consumes the board.
- **The fork history agrees.** `JJDSNT/Emu68 feature/host-irq-abi` is exactly
  three commits over upstream `9b4379a`, all dated 2026-08-01. The first
  bring-up had no Zorro at all; the board came later, and the reason for it —
  reusing Emu68's `brcm-sdhc.device` — never happened, because this port has its
  own driver.
- **Offered, it stops a fault and starts answering.** With the board present the
  64 KB window at `0xe80000` no longer faults: an access there is absorbed,
  advances `board_idx`, and can configure a board. One run caught it doing
  exactly that — `Mapping ZIII Emu68 ROM board at address 00000000`, followed by
  ~74 KB of module images copied over guest memory from `0x1000`. Removing the
  board restores the fault, which is what a wild pointer should meet.

**The measurement does not support a stronger claim than that.** Zorro on: 8
`icons` in 24 runs (33%). Zorro out: 5 in 10 (50%). Fisher exact, two-sided,
p = 0.45. The removal is justified by surface area, not by a change in the
failure rate, and the failure rate is unchanged as far as anyone can tell.

Two things found on the way, both worth keeping:

- **`z3_disable` is unreachable on a stock build.** `parse_cmdline()` reads the
  token inside `#ifdef PISTORM_ANY_MODEL` (`start.c:667`), so the off switch for
  a board that `patches/emu68/0002` had made reachable was itself not. The first
  A/B run against it therefore measured nothing; those eight runs are counted
  above as ordinary Zorro-on samples. Same shape as ISSUE-0004.
- **Removing a patch orphans the files only it touched.** `setup.sh --reset`
  clears `skip-worktree` from the *current* series file list, so files that only
  the removed patch touched (`CMakeLists.txt`, `src/boards/emu68rom.c`) kept the
  bit and their old contents — invisible to `git status` at both levels, and
  enough to make `--verify` report a state that matched neither pristine nor
  applied. Clear the bit across the whole index before reasoning about it.

The two defects this issue originally proved are in a different state than the
paragraph below claims, and the correction matters. The FAT endian change is
**not** a working fix: promoted to a patch and measured, it failed three runs of
three, against two of two for its absence. Only its write half is worth keeping,
untested so far. The 66-versus-68 frame question is genuinely open — the port
reached the desktop before the 68-byte backend existed, and that backend is now
parked on `codex-2026-08-05`.

# Problem

Emu68 exception/RTE frames contain SR (2), PC (4), and format/vector (2).
The optimized `m68k-all` scheduler persisted only 66 bytes (PC, SR and 15
registers). Emu68 requires 68 bytes. Reconstructing an incomplete frame can
produce random return PCs such as `0xffffffa0` or zero A6.

Separately, fat-handler wrote directory cluster, size and date fields without
little-endian conversion. Generated bitmap font indexes such as `arial.font`
then appeared as 738852864 bytes instead of 2604 bytes. Wanderer reproducibly
stopped in `OpenDiskFont("arial.font", 11)` during its first redraw.

# What was done

- Added an Emu68-only 68-byte Switch/Dispatch/PrepareContext backend. Target
  symbols are selected in the final ELF; target behavior is no longer added to
  `m68k-all` through new defines.
- Fixed FAT date, cluster and file-size conversions with `AROS_WORD2LE`,
  `AROS_LONG2LE` and matching read conversions.
- Repaired only the disposable `/tmp/wanderer-trace.img` font entries to make
  the redraw test usable. The distributed image has not been silently patched.
- A/B tested the scheduler from before the 23–27 July fast-path changes. A
  coherent conventional backend (including generic Wait/Signal) switches
  contexts without PC corruption but stops after SD initialization, so it is
  not a viable rollback.
- Found build-system traps: new arch objects are not always dependencies of
  the aggregate kobj; removed source files can leave wildcard-selected orphan
  objects; the Emu68 ELF must be removed to force relink. Always verify symbols
  and object sizes in the final ELF before QEMU.

# IRQ path through kernel.resource

Physical BCM283x interrupts do not follow the ordinary Amiga Paula dispatch
path on this target. The intended chain is:

`ARM peripheral -> Emu68 ARMPending -> m68k level-6/EXTER ->`
`Platform_Autovector() -> ARM interrupt-controller Dispatch() ->`
`krnRunIRQHandlers() -> driver handler -> Signal()/message reply`.

Drivers such as the BCM283x system timer register with
`KrnAddIRQHandler()`. On `m68k-emu68`, `kernel.resource` therefore owns the
handler lists and invokes them, but Emu68 and `platform.c` provide the bridge
that gets execution there. `KrnCli()`/`KrnSti()` are not the physical IRQ gate
on this port; Exec nesting and the Emu68/Paula `INTENA` shadow matter instead.

Bridge protocol details:

- Emu68 records `ARMPending` whenever an ARM interrupt arrives.
- It raises m68k level 6 only if the guest `INTENA` shadow has both `INTEN`
  and `EXTER` set.
- `Platform_Init()` must arm that gate once with `INTENA SETCLR|INTEN|EXTER`.
- The pending state is cleared only when the guest acknowledges EXTER through
  `INTREQ`; `Platform_Autovector()` dispatches first and acknowledges last.
- The ARM controller dispatch maps pending bits to the IRQ numbers used by
  `KrnAddIRQHandler()` and calls `krnRunIRQHandlers()`.

This produces three distinct failure classes to test, without immediately
blaming message ports: no level-6 entry means the Emu68/INTENA bridge is
closed; level-6 entry without the expected handler means controller decoding
or masking is wrong; handler execution without task wakeup points to
Signal/message ownership or scheduler state.

# Unenabled sources were being dispatched (found and fixed 2026-08-05)

`intc_dispatch()` read `ARMIRQ_PEND`, `GPUIRQ_PEND0` and `GPUIRQ_PEND1` and
dispatched every bit it found, without masking against the enable registers.

The comment at the top of `interrupt_controller.c` already described this trap
for the ARM bank's mirror bits and guarded against it by scanning only eight
bits. The same trap exists one level down for the real bit. The SD driver writes
`SDHCI_SIGNAL_ENABLE` during initialisation (`sdcard_bcm2708init.c:50`), the
Arasan controller starts asserting GPU IRQ 62, and `GPUIRQ_PEND1` bit 30 goes
high. Nothing on this port registers a handler for 62 — the driver polls
`INT_STATUS` — so `krnRunIRQHandlers()` found nobody, the source was never
acknowledged, and level 6 re-entered continuously.

Serial evidence, with `PLATFORM_TRACE_BRINGUP` on: after
`[SDBus00] MMC0: [256MB Capacity]`, `[intc] IRQ62 pending` repeated without
bound while `[sd] INT_STATUS` stayed at `0x00000001` (command complete, never
cleared) and `[exter] LEVEL6 entry` kept counting.

The fix masks each bank with its enable register before dispatch, which is what
the file's own design already stated: `intc_init()` starts fully masked and
drivers unmask their own IRQ through `KrnAddIRQHandler()`. What a driver has not
unmasked is not ours to acknowledge.

Measured result: the storm stops — `[intc] IRQ62 pending` goes from unbounded to
nine events and ceases. The boot still does not progress past SD initialisation,
so this was a real defect but not the whole story.

# One upstream fix had never been imported (found and imported 2026-08-05)

`/home/jaime/AROS`, branch `feature/m68k-emu68-baremetal`, is where this port was
originally developed. It was re-examined for work that never reached this
repository, on the theory that something in exec or the scheduler had been left
behind. The comparison was made file by file, against the branch tree rather
than against the patch series:

- `arch/m68k-all` — every file the branch changes is changed identically by
  `patches/aros/0002`. Including `preserveall.S` and `preserveall_install.c`.
- `arch/m68k-emu68` — the branch has no file this tree lacks. `kernel/cause.c`,
  `kernel/schedule.c`, `kernel/getsystemattr.c`, `kernel/kernel_debug.c` and
  `kernel/kernel_arch.h` are byte-identical. Our tree has *more*:
  `kernel/context.c` and the whole `exec/` backend.
- `rom/dos/newcliproc.c` — the branch carries two fixes; `patches/aros/0004`
  carries both.

Exactly one substantive commit was missing: `d3baf6ed82`, *"keep the system
timer compare in the future"*. It has never existed in this repository at any
commit. `systimer_heartbeat()` here read `CLO` and wrote `CLO + interval`
without checking the target was still ahead of the counter when the write
landed. The BCM283x comparator matches on **equality** against a free-running
1 MHz counter, so a target the counter has already passed does not fire late —
it never fires again until the 32-bit counter wraps, 71 minutes on. The
heartbeat stops, and with it every `Wait()` timeout, every `Delay()` and the
scheduler quantum, wherever the boot happened to be. Silently.

`arch/aarch64-native/kernel/platform_bcm2708.c` writes it once and is fine: on
real silicon that window is nanoseconds. Under Emu68 it covers a JIT translation
and whatever the serial console is doing, which can exceed a whole 20 ms period.

Imported as `systimer_arm()`: advance from the compare that just fired rather
than from `CLO`, so a late interrupt does not stretch the period; re-read `CLO`
after the write and retry with a fresh margin if the target is already behind;
signed comparison so it stays correct across the wrap; bounded retries.
`emu68_platform_late` counts the retries.

The commit that introduced it upstream says plainly that it did not fix the
intermittency it was written to chase — six boots either side, four reached
Workbench with and without it. It is imported because the race is real, not
because it is expected to be the fix.

Measured here after import: the heartbeat is sustained. `[systimer] IRQ tick`
and `[exter] LEVEL6 entry` both reach `0x1000` — about 82 seconds of ticks — and
`[intc] IRQ62 pending` stops by `0x100`. The boot still produces nothing after
`[SDBus00] MMC0: [256MB Capacity]`.

That is worth stating precisely, because it changes the shape of the problem:
the machine is **not** dead after SD initialisation. Interrupts are delivered,
decoded, and their handlers run, for minutes. Whatever has stopped is in
software above the interrupt path, not in the interrupt path.

# The card poisoned itself (root cause of the regression, 2026-08-05)

The stall "right after SD initialisation" was never a stall there. It was the
absence of tracing: `patches/aros/0006`, which turns on the existing `D()` output
in `dos`, `lddemon` and the Shell, had been deleted from the working tree with no
note. Restored, the same build shows the boot mounting `SDCARD0P0:`, doing its
assigns, running `AROSMonDrvs` and reaching `IPrefs` — a thousand lines further
on than the last thing it used to print.

`IPrefs` then hangs, in the same place on every run, with the same addresses.
Tracing it down: `FontPrefs_Handler`, on `ENV:Sys/font.prefs`, at
`OpenDiskFont("ttcourier.font")`.

The reason was on the card:

```
ARIAL~1  FON  738852864  1996-15-28   arial.font
TTCOUR~1 FON  738852864  1996-15-28   ttcourier.font
```

738852864 is `0x2C0A0000`. The correct size, 2604, is `0x00000A2C` — the same
four bytes reversed. The date reads month 15. Six entries were affected:
`arial.font`, `ttcourier.font`, `xen.font`, `stop.font`, `fixed.font` and
`fontcache`. The cluster fields are byte-swapped too, so the data is
unrecoverable, not merely mis-sized.

These files do not ship — they are **generated during boot** and exist in no
distribution tree. So the sequence that produced the regression is:

1. A fresh card boots to Wanderer, repeatedly. This is what was seen on
   2026-08-03.
2. Some boot generates the font indexes and writes them through the unfixed
   fat-handler, which stores size, cluster and date big-endian.
3. Every boot after that reads a 738 MB font index and never returns from
   `OpenDiskFont`.

The card poisons itself, once, permanently, on a boot that itself looks fine.
That is why "it worked 100% of the time and then stopped" without anybody
changing the code that broke, and why every artifact combination tried on
2026-08-05 failed identically — they all shared the one poisoned `sd.img`.

`mdel` cannot remove the entries: it walks the bogus 738 MB chain and hangs, the
same way AROS does. The card was regenerated with `scripts/make-sdcard.sh
--dist`, from the full distribution tree at
`/home/jaime/aros-build-emu68-m68k/bin/emu68-m68k/AROS` — the lean
`kernel-link-<target>` build produces only the ELF and cannot make a card.

With a clean card the boot runs the whole of `S:Startup-Sequence` to its last
line, `If EXISTS "WANDERER:Wanderer"`, starts Wanderer, and gets as far as
loading `IconVolumeList.mui` — Wanderer's own icon class.

# What is under it

With the font blocker gone, a second and different failure is exposed, and it is
the one this issue was originally opened for. After `IconVolumeList.mui` loads:

```
[JIT] opcode 207f at 000023f8 not implemented
    A6 = 0x00002340   A7 = 0x004dae90
    PC = 0x000023f8   SR = T0|..|IPM0|..Z..
```

`0x207f` is not an instruction — mode 7 register 7 is an invalid destination.
The RAM dump around `0x23f8` shows ROM pointers (`0x3460eb38`, `0x34607f00`),
so the PC has landed in a table of function pointers, in user mode. `A6` is
`0x2340`, which is no library base. This is a `jsr` through a corrupt `A6`,
which is exactly the "corrupt PC/A6" this issue was opened about.

The frame-size mismatch was checked first and is **not** the cause. In the linked
ROM, `rom/exec/exec/arch/dispatch.o` uses `lea %a5@(68),%a2`, and `switch.o`
references `emu68_SwitchTail`. `m68k_DispatchFrame` and `m68k_SwitchTail` — the
66-byte pair in `arch/m68k-all/kernel/kernel_cpu.c` — are referenced by nothing
in 1306 linked objects. They come along only because `kernel_cpu.o` also defines
`cpu_Exception`. The 66 is dead code and debt to clean up, not the defect.

The 68 itself is internally consistent. `arch/m68k-emu68/exec/dispatch.S` lays
the frame out as PC(0), SR(4), format(6), registers(8..67), and `context.c`
reads the format word at offset 6 to match — that is the 68010+ format-0 frame a
68040 pushes on interrupt, where 66 is the 68000 frame with no format word. But
consistency between two files written together is not evidence that the format
word is needed: **the port reached the desktop before this backend existed**,
through the 66-byte `m68k-all` path.

`emu68_CheckTaskFrame()` never printed `[EMU68-FRAME] BAD` in any run, which
looks like support for the 68-byte layout and is weaker than it looks: the
validator only runs on the slow dispatch path, which handled **five** context
switches in a whole boot. Everything else goes through the inline fast path in
`dispatch.S`, which validates nothing.

# Where it actually stops now

Wanderer does not hang. It runs, loads `muimaster.library`, `muiscreen.library`,
`Zune/IconDrawerList.mui` and `Zune/IconVolumeList.mui` — its own icon classes —
and then:

```
[Shell] returned 0 (0): WANDERER:Wanderer
```

It exits, cleanly, code 0. The machine stays up and goes idle: `Decorator`,
`PUBSCREEN handler`, `Workbench Handler`, `IPrefs`, `ConClip`,
`Intuition menu handler`, `console.device` all sitting in `TS_WAIT`, with
`input.device` and `SDCARD0P0` still being woken on every tick. Nothing is
stuck. The display simply never changes: the framebuffer stays on the Emu68
logo, `#787878`.

The graphics driver is not obviously at fault — with `DEBUG` restored in
`emu68gfx_init.c` it reports:

```
[emu68gfx] Init: flags 0x0000003f fb 0x3c100000 640x480 pitch 1280
[emu68gfx] Init: AddDisplayDriver() => 0
[emu68gfx] Init: display driver registered
```

So the question is why Wanderer gives up rather than opening its screen. The
Wanderer on the card carries no tracing: `workbench/system/Wanderer/main.c` has
`#define DEBUG 1` in the tree, but the binary on the card predates it. A traced
Wanderer has been built and not yet installed — that is the next measurement.

# The screen was being swallowed by Emu68 (found and removed 2026-08-05)

The Emu68 submodule was never audited against its own series — ISSUE-0008 looked
only at `external/aros`. Doing it the same way (pinned `9b4379a5` plus
`patches/emu68/` in a scratch worktree, `diff -rq`) found one drifted file,
`src/aarch64/vectors.c`, carrying an open-bus guard that exists in no patch:

```c
/* write side */
if (far >= 0x01000000 && !is_system_memory(far, size))
    return 1;

/* read side */
if (far >= 0x01000000 && !is_system_memory(far, size)) {
    if (value) *value = UINT64_MAX;
    ...
}
```

`is_system_memory()` walks `sys_memory`, which the boot log gives as
`0x00000000-0x347fffff`. The framebuffer is at `0x3c100000` — **outside it**.
So every guest write that reached the fault handler for the framebuffer was
silently discarded. The machine booted, Wanderer ran, and nothing could ever
appear.

Restoring `vectors.c` to what the series produces and rebuilding Emu68:

```
[clean1] top colours: #989898:297558 #e8ece8:7956 #000000:1608
```

`#989898` is the Workbench screen, against `#787878` for the Emu68 logo. The
screendump shows the Workbench screen with its title bar — *"Wanderer 832.62M
graphics mem 832.62M other mem"* — and the mouse pointer.

Two things follow, and neither should be overstated.

**The desktop is empty.** No volume icons. That is the symptom this issue was
opened for, now reached again rather than solved.

**It is intermittent: one run in three.** `clean2` and `clean3`, identical
configuration on an idle machine, stopped at the logo. So removing the guard
restored the *possibility* of the screen, not its reliability. The intermittency
was always the real subject of this issue and is now, again, the only thing in
the way.

The configuration that reached it: the 2026-08-03 ELF from
`/home/jaime/aros-build-emu68-m68k`, a card regenerated from that same
distribution, and Emu68 built from the clean series. The removed guard is parked
as `AI_context/codex-2026-08-05/emu68-submodule-drift.patch`.

The current `out/build/aros` ELF did **not** reach the screen in two tries under
the same clean Emu68, so there is a second regression on the AROS side to find,
separately from the intermittency.

# Why Wanderer exits: no Workbench screen

Traced with `DEBUG` on in `workbench/system/Wanderer/{main,wanderer}.c`, rebuilt
and installed onto the card the same way IPrefs was:

```
[Wanderer] main: Handing control over to Zune ..
[Wanderer] Wanderer__MUIM_Application_Execute: Creating 'Workbench' Window..
[Wanderer] Wanderer__MUIM_Wanderer_CreateDrawerWindow: Couldn't lock screen!
[Wanderer] main: Returned from Zune's control ..
```

`CreateDrawerWindow` calls `LockPubScreen(NULL)` and gets NULL, so it disposes
itself and Zune's loop ends. `LockPubScreen(NULL)` resolves to
`IntuitionBase->WorkBench`, and when that is NULL it calls `OpenWorkBench()`
itself and retries — so the real statement is **`OpenWorkBench()` fails**.

## The screen size was wrong, and that is fixed

With `DEBUG` on in `rom/intuition/openworkbench.c`:

```
[OpenWorkbench] Requested size: 800x600, depth: 4, ModeID: 0xFFFFFFFF
[OpenWorkbench] Invalid ModeID given
[OpenWorkbench] Size: 800x600 ... ModeID 0xFFFFFFFF     (no 800x600 mode exists)
[OpenWorkbench] Size: 640x480, depth: 4, ModeID 0x00100000
[OpenWorkbench] Maximum size: 16384x16384
[OpenWorkbench] Corrected size: 800x600 4bpp
```

The 800x600 is intuition's compiled-in `ScreenModePrefs` default — there is no
`screenmode.prefs` on the card. `openworkbench.c` bounds the requested size by
`DTAG_DIMS`, whose maximum comes from the display driver's `aHidd_Sync_HMax` /
`VMax`. `emu68gfx_hiddclass.c` filled `HDisp`/`VDisp` from the real framebuffer
at runtime but left `HMax`/`VMax` at a hardcoded 16384, so the clamp did nothing
and intuition asked for an 800x600 screen on a 640x480 framebuffer.

Fixed by reporting the real size in `HMax`/`VMax`: this driver owns one linear
framebuffer handed over by the firmware and cannot raster anything larger.
Verified in two runs — `Maximum size: 640x480`, `Corrected size: 640x480 4bpp`.

## And it is still not enough

`OpenScreen` still fails, so `Couldn't lock screen!` still follows. The next
datum needed is why, and `DEBUG_OPENSCREEN` cannot supply it: it is defined as
`;` in `rom/intuition/intuition_intern.h:1231`, unconditionally, so the
`DEBUG_OpenScreen` switch at the top of `openscreen.c` controls nothing.
Explicit `D(bug())` calls at that function's failure exits are needed instead —
`DEBUG 1` is already set in the file.

Worth suspecting, in order: depth 4 against a driver that publishes only a
16-bit truecolor pixfmt with no CLUT; and the mixture of build trees described
below.

# Two build trees are being mixed

The card is generated from `/home/jaime/aros-build-emu68-m68k` — the tree that
produced the media which reached Wanderer on 2026-08-03 — while the ROM, IPrefs
and Wanderer now come from `out/build/aros`. A new intuition against old
`Classes`, `Devs` and `Libs` is exactly the kind of mixture that can break the
display path, and it is not currently controlled for. Either build a full
distribution in `out/build/aros` and generate the card from it, or keep every
binary from the old tree. Do not keep straddling both.

# A/B on the Emu68 Exec backend

Prompted by the observation that the port reached the desktop before that backend
existed. Disabling `arch/m68k-emu68/exec/mmakefile.src` and clearing the stale
objects makes the 66-byte `m68k-all` path link again — verified in the object,
`lea %a5@(66),%a2`, and `switch.o` then references `m68k_SwitchTail`.

| build | lines | JIT faults | Wanderer |
|---|---|---|---|
| Codex backend | 4430 | 55 | crashed: `0x207f` at `0x23f8`, `A6=0x2340` |
| Codex backend | 4548 | 0 | `returned 0` |
| Codex backend | 4484 | 0 | `returned 0` |
| m68k-all, 66-byte | 4363 | 0 | still running at cutoff |

One run without the backend is not a result. It is enough to say the question is
open and worth settling, which it was not before.

# The reference build no longer exists

Every artifact that had been treated as a known-good oracle was rebuilt or
overwritten after the runs that reached Wanderer:

| artifact | mtime |
|---|---|
| `/home/jaime/Emu68/build/Emu68.raw.img` | 2026-08-01 16:48 |
| `/home/jaime/aros-build-emu68-m68k/bin/emu68-m68k/AROS/aros-emu68-m68k.elf` | 2026-08-03 23:29 |
| `/home/jaime/aros-build-emu68-m68k/sd.img` | 2026-08-04 19:57 |

Five configurations were run on 2026-08-05, one at a time on an idle machine.
All five stop at the same place — immediately after
`[SDBus00] MMC0: [256MB Capacity]` — in four different ways:

| Emu68 | ELF | SD image | display | outcome |
|---|---|---|---|---|
| current | current | current | none | IRQ62 storm |
| current | 08-03 | current | none | CPU exception, vector 0x28 |
| 08-01 | 08-03 | current | none | silence |
| 08-01 | 08-03 | 08-04 | none | `Undefined LineC` |
| 08-01 | 08-03 | 08-04 | gtk | logo, no progress |

The last row is the user's own command, which reported the desktop on every
attempt on 2026-08-03. It no longer reaches it. `-display gtk` versus
`-display none` was the one untested variable left over from that session; it is
now tested and it is not the difference.

There is therefore no artifact left to bisect against, and the four distinct
failure modes from nearly identical inputs say the machine is fragile at this
point rather than broken in one specific way.

# Current hypothesis and test state

The fast backend remains the only variant that has reached Wanderer. Its
Emu68-specific 68-byte frame handling is present in the tested ELF.
Target-local Paula INTENA close/reopen around voluntary Switch/Dispatch did
not change the reproducible stop after SD initialization, so it is not a
confirmed fix and should be removed if further evidence does not support it.

A bounded diagnostic in `emu68_DispatchFrame()` prints `[EMU68-IDLE]` when
`core_Dispatch()` returns no runnable task. After
`[SDBus00] MMC0: [256MB Capacity]`, no such marker appeared. The machine is
therefore not simply reaching the scheduler's idle/STOP path: a task is still
running or blocked inside an I/O path, or interrupt delivery stops before a
new scheduling decision. The next discriminating observation is whether
level-6 entries and the timer/SD IRQ handlers continue after this point.

`platform.c` already contains bounded `[exter] LEVEL6 entry` traces and boot
reads of `INTENAR`; verify whether their trace backend is active before adding
new diagnostics. Instrument in order: direct level-6 entry, interrupt
controller pending/IRQ number, system-timer and SD handler, then the resulting
Signal/message wakeup. Keep all output bounded to avoid changing timing.

The legacy Bellatrix `HARNESS_MSGPORT_OWNER_FIX` rewrote `mp_SigTask` in the
Musashi harness. It may describe a symptom but is not evidence that native
AROS should change port ownership; do not transplant it without identifying a
specific transferred port.

# Plan to get the desktop back

The goal is the Workbench screen with its icons, reached repeatedly.

1. ~~Rebuild from the committed state.~~ Done differently and better: the
   unreviewed working state is parked on branch `codex-2026-08-05`, and `main`
   now carries only what has been measured — the `intc_dispatch()` enable mask
   and the `systimer_arm()` import. **Caveat:** switching branches does not
   touch a submodule's working tree, so `external/aros` still physically carries
   all nineteen drifted files and a build from `main` still compiles them.
2. Find why Wanderer exits instead of opening its screen. Install the traced
   Wanderer on the card, the same way the traced IPrefs was installed. This is
   the only thing between here and the goal.
3. Settle the Exec-backend question with three runs per side, not one.
4. Convert the FAT fix to `patches/aros/`, then revert the rest of the submodule
   drift, so a build from `main` matches what `main` says it is. Without this,
   step 3's result is not attributable.
5. Measure to the discipline this issue has already paid for three times: serial
   runs, idle machine, minimum three per configuration, `screendump` and
   dominant colour rather than a glance at the window.

# What is left

- Establish whether the committed baseline reaches the desktop.
- Find what runs — or fails to run — between SD initialisation and the next
  thing that would print. The interrupt path is now cleared as the cause: it
  keeps delivering for minutes after the last output. Follow `dosboot` instead,
  which is the next thing to speak, and instrument the mount of `SDCARD0P0:`.
- Determine why the boot still stalls after SD initialisation now that the
  unenabled-source storm is gone and the heartbeat is sustained. `[EMU68-IDLE]`, `[EMU68-TASK]` and
  `[EMU68-FRAME]` are all silent, so `emu68_DispatchFrame()` is not being
  reached: `arch/m68k-emu68/exec/dispatch.S` handles the common case inline and
  only falls back to it when the ready list is empty, the task is not `TS_READY`,
  `TF_DISPATCH_SPECIAL` is set, or `AFF_FPU` is present. Confirm which branch is
  being taken before instrumenting anything else.
- If level 6 stops, inspect the INTENA shadow, EXTER acknowledgement and Emu68
  RTE re-delivery behavior. If it continues, follow the decoded timer/SD IRQ
  through `krnRunIRQHandlers()` and the task wakeup.
- If it reaches icons, remove all Wanderer/MUI diagnostics and validate at
  least five fresh QEMU boots.
- If it still fails, instrument bounded Wait/Signal transitions and identify
  the exact task/port rather than applying the legacy ownership workaround.
- Rebuild a clean SD image with the FAT fix, then verify no directory fields
  are byte-swapped after repeated boots.

# Acceptance criteria

- [ ] Wanderer displays volume icons in at least five consecutive QEMU boots
- [ ] No CPU exception, corrupt PC/A6, or startup-sequence stall
- [ ] Fresh FAT image retains valid cluster/size/date fields after boots
- [ ] Temporary serial/UI diagnostics removed
- [ ] Emu68-specific scheduler logic remains under `arch/m68k-emu68`

# Execution log

- 2026-08-04 — localized redraw stall to OpenDiskFont and proved FAT endian
  corruption from raw directory bytes.
- 2026-08-04 — proved 66-byte versus 68-byte exception-frame mismatch.
- 2026-08-04 — conventional scheduler A/B rejected after reproducible stop
  following `[SDBus00] MMC0: [256MB Capacity]`.
- 2026-08-04 — resumed fast target-specific backend and INTENA coherence test.
- 2026-08-04 — target-local INTENA close/reopen did not move the SD stop.
- 2026-08-04 — bounded dispatcher diagnostic produced no `[EMU68-IDLE]`
  after SD initialization; ordinary scheduler idle is ruled out.
- 2026-08-04 — documented the Emu68 EXTER/level-6 to `kernel.resource` IRQ
  chain and the ordered diagnostic decision tree.
- 2026-08-05 — answered the first open question: level-6/EXTER interrupts do
  continue after SD initialisation. They continue because GPU IRQ 62 is never
  acknowledged.
- 2026-08-05 — found and fixed the unenabled-source dispatch in
  `intc_dispatch()`; the IRQ62 storm stops, the boot does not advance.
- 2026-08-05 — established that no known-good artifact survives: five
  configurations, including the exact command that succeeded on 2026-08-03, all
  stop after SD initialisation.
- 2026-08-05 — `-display gtk` versus `-display none` ruled out as the cause of
  the 2026-08-03 intermittency.
- 2026-08-05 — opened ISSUE-0008: four files inside `external/aros` carry edits
  belonging to no patch, and two Emu68 patch files are corrupt, so the state
  being measured cannot be rebuilt from the repository. Later the same day the
  count turned out to be nineteen, not four.
- 2026-08-05 — restored `patches/aros/0006`. The boot was never stopping after
  SD initialisation; it was running a thousand lines further with nothing to
  print. Deleting that patch cost a day.
- 2026-08-05 — found the root cause of the regression: the card generates its
  font indexes at boot and an earlier boot wrote them through the unfixed
  fat-handler, byte-swapping size, cluster and date. Six entries affected.
  `OpenDiskFont` then never returns. Card regenerated.
- 2026-08-05 — with a clean card the boot runs the whole of `S:Startup-Sequence`
  and starts Wanderer, which loads its icon classes and exits 0. The system
  stays up and idle; the framebuffer never leaves the Emu68 logo.
- 2026-08-05 — A/B on the Emu68 Exec backend: one run with the 66-byte
  `m68k-all` path, no JIT faults, Wanderer still running at cutoff. Not a
  result yet; the question is now open where before it was assumed settled.
- 2026-08-05 — parked the unreviewed working state on branch
  `codex-2026-08-05`; `main` keeps only measured changes.
- 2026-08-05 — audited the Emu68 submodule against its series for the first
  time. One drifted file, an open-bus guard swallowing every write above 16 MB
  that is not system memory -- including the framebuffer at 0x3c100000.
  Removed; the Workbench screen appears. Empty, and in one run of three.
- 2026-08-05 — aligned `external/aros` to the reference fork verifiably: of the
  78 files it changes, 53 of the 57 under `arch/m68k-emu68` match byte for byte
  and the other 21 are reproduced by `patches/aros/`. Drift went from nineteen
  files to zero.
- 2026-08-05 — `setup.sh --verify` reports `all series applied` for both
  submodules for the first time; `build-aros.sh` runs without being bypassed.
  ISSUE-0008 closed.
- 2026-08-05 — **the desktop is back, with icons.** RAM Disk and Aros drawn,
  title `Wanderer 832.96M graphics mem`, from this repository's own build.
  Wanderer needs roughly eight minutes under QEMU; a run cut at 200 s shows an
  empty screen still loading `Zune/IconListview.mui` and reads as failure.
- 2026-08-05 — the FAT endian change was promoted to `patches/aros/0007` and
  measured: three runs, three failures. Reverted: two runs, two desktops. It is
  a regression. Its read half converts a second time on a path that already
  converts; only the write half is worth taking, and it is still untested.
- 2026-08-05 — protocol note that cost a day to learn: the unfixed handler
  poisons the card, so one run contaminates the next. Regenerate the card before
  every single run of any A/B that touches the FAT path.
- 2026-08-05 — compared `/home/jaime/AROS` (`feature/m68k-emu68-baremetal`)
  against this tree file by file. One substantive commit had never been
  imported: `d3baf6ed82`, the system-timer compare race. Imported.
- 2026-08-05 — three serial runs with the imported fix, one at a time on an idle
  machine. All three stop after `[SDBus00] MMC0: [256MB Capacity]`; dominant
  colour `#787878` (Emu68 logo) in each. The heartbeat now runs to `0x1000`
  ticks, so the interrupt path is ruled out as the thing that stops.
- 2026-08-06 — the intermittency measured for the first time, with the harness
  from ISSUE-0011: 4 of 10 reach the icons. The number matters less than the
  shape it revealed — screen-open is nearly deterministic at 39-48 s, the failing
  runs split into two distinct populations, and the stalled ones stay stalled
  through 900 s rather than finishing late. The card is ruled out mechanically:
  regenerated before every run, with a distinct image hash per record.
- 2026-08-06 — six runs with the serial kept, to ask *where* rather than *how
  often*. Five stalls, five different stopping points, all inside the library
  loader and all after Wanderer had loaded its MUI classes. No library is
  implicated: the successful run loads all of them. One stall is followed by
  Emu68 mapping the Zorro III ROM board, which happens in no other run.
  The harness now keeps the serial log unconditionally — the first ten runs
  measured the rate and discarded every byte of it, so this question needed the
  series run again.
- 2026-08-06 — removed the Zorro III board from the build (patches 0002 and
  0003) after establishing that nothing in this port consumes it and that
  offering it turns a faulting window into one that answers. Ten runs after:
  5/10 against 8/24 before, p = 0.45 — no demonstrable change in the
  intermittency. Kept for the surface area, not for the rate.
- 2026-08-06 — added ARM PC sampling to the harness and got an answer in one
  series: 3 of 4 stalls are an Emu68 ARM stack overflow, parked at
  curr_el_spx_sync+0 in a two-instruction exception loop, with 0xdff01e (the
  Paula INTREQR shadow) as the guest address in flight in two of them. The
  fourth is a separate population, executing inside the JIT arena with a healthy
  stack. Every future stall now carries its own register dump in stall.txt.
- 2026-08-06 — SP sampling added. Normal operation uses 704-1568 bytes of ARM
  stack, flat; the failure jumps to 524416 between two samples. Collapse, not
  leak. The `logo` population shows the same signature, so the two failure
  classes are probably one defect. Stack-window capture added to stall.txt; the
  next series returned 4/4 icons and caught nothing, which is its own reminder
  about variance.
- 2026-08-06 — PSTATE added to the probe. Normal execution runs with ARM
  interrupts enabled; the all-masked value appears only after the collapse. Also
  settled the 66-vs-68 frame question from the arbitration code: Emu68 pushes an
  8-byte format-0 frame, so 68 is right. That series returned 0 icons in 3,
  against 4 in 4 two series earlier — the variance is wide and no single series
  should be read as a rate.
- 2026-08-06 — added a re-entry guard to SYSHandler and caught the first fault
  in 3 of 3 stalls: a guest write to a wild address (once literally the bytes
  of the string "debug"), emulated through the unmapped part of the linear
  alias, faulting inside the handler. Two defects, now separable: the guest
  going wild (AROS side, this issue) and Emu68 recursing instead of reporting a
  bus error (a corrected open-bus guard).
- 2026-08-06 — open-bus guard (patches/emu68/0005), asked of the MMU rather
  than a region table. ARM stack use at a stall drops from 524416 bytes to
  720-1584, and every stall now leaves the guest executing inside the JIT arena
  instead of frozen on the exception vector. Rate unchanged at 3/8, as expected
  for a survivability fix. Remaining: one bounded re-entry still happens, and
  the reported addresses are alias-shaped, so the guard is catching the second
  fault rather than preventing the first.
- 2026-08-06 — read the parked 68-byte backend on codex-2026-08-05. It saves
  and restores all eight bytes including the format/vector word and is
  self-contained: pushes 8, pops 8, with no reliance on a leftover on the
  supervisor stack. That is strictly more robust than the 66-byte scheme, which
  balances only while entry and exit are paired.
- 2026-08-06 — replaced the predictive open-bus guard with a recovering one, and
  the reports now carry guest addresses: "alnu", "ket\0", "ket\x0e", "ga..".
  The guest is dereferencing string data as pointers, and the same string at
  two offsets two bytes apart -- the signature of a longword read misaligned by
  two. Strongest corroboration yet for 66-versus-68, and still corroboration
  rather than proof.
- 2026-08-06 — the harness now reads guest memory at the m68k PC whenever the
  guest's trap handler reports an exception. First use corrected a reading of
  mine: the vector-11 F-line exceptions are not an FPU gap, they are a wild PC
  landing on data whose bytes happen to decode as F-line. Every failing run in
  a series of eight reported an exception; every successful run reported none.
  The `logo` signature is deterministic -- same PC 0x0088520a and identical
  registers across three runs -- and is the better one to chase.
- 2026-08-06 — 0xc0000058 is read in 3 of 3 logo failures and 0 of 5 successes,
  at the same point each time, and is the first wrong thing in that chain. The
  F-line vector turns out to be a consequence of open bus reading as all ones
  when the PC lands in unmapped memory, not a clue about the cause. 0x0088520a
  stays the target: mapped RAM, deterministic, identical registers, and an A6
  of 0x2b58 that cannot be a library base.
- 2026-08-06 — correction: the "other qemu-system process" that blocked the last
  run of the chase series was a deliberate one of the user's, and it affected
  only that run. The other five are clean and the two wild PCs captured in them
  stand. A commit message from that moment says the series may be contaminated;
  it is wrong and this is the correction.
- 2026-08-06 — symbolised the guest's user stack against our own m68k ELF
  (base 0x34600000, byte-swap the monitor's little-endian words first). The
  crash is inside InternalLoadSeg_ELF calling Dos_7_Read, with the module's ELF
  header on the stack. Corrects two earlier readings: A6=0x2b58 is a saved size,
  not a corrupt library base, and 0x0088520a is in the loader's own segment
  allocation region. Points at the disk read path, i.e. ISSUE-0009.
- 2026-08-07 — audited `codex-2026-08-05` against `main` item by item. One piece
  was missing from `main`: the FAT date/time byte order in
  `rom/filesys/fat/date.c`, now `patches/aros/0011`. Everything else on that
  branch is diagnostics, the read-side FAT change that measurement rejected, the
  Paula path that IPL superseded, or already on `main`. The branch owes `main`
  nothing further. The date fix is correctness on disk, not a boot fix, and is
  not claimed to change the rate.
- 2026-08-07 — correction, and the user caught it: `date.c` had **not** merely
  never been carried, it was on `main` on 2026-08-06 in `1772a01` and was deleted
  by `c169cd9` when that commit cut `0008` back to its write half. The read-side
  substitution was the measured regression; `date.c` shared its patch file and
  shared its fate. Restoring it is undoing an over-broad revert, not importing
  someone else's work. The general lesson is filed above: a patch file is the
  unit of revert, so bundling is how a good change inherits a bad verdict.
- 2026-08-07 — audited `experiment/frame-68` the same way. `main` is a strict
  superset of it on every shared file: its FAT patch is the pre-cut version now
  covered by `0008` plus `0011`, its ELF-loader patch is the tracing-carrying
  draft of `main`'s `0009`, its stack patch uses 64 KB where `main` uses the
  measured 40960, and its open-bus patch predates the guest-PC reporting that
  named `tlsf_freevec`. Its `ISSUE-0007` has no line `main`'s lacks. One finding
  was owed and is now carried: the ELF-loader fix was verified there to take the
  reads-past-the-fill from 2 per boot to 0. Those three runs all failed to reach
  icons and say nothing about the rate — the build carried ~2000 lines of tracing
  per boot, the 68-byte backend and a 64 KB stack.
- 2026-08-07 — `codex-2026-08-05` deleted, local and `origin`, and not tagged.
  It was scaffolding for a working tree the repository could not represent, not
  an archive of a code line; `legacy` is the latter and keeps its tag. The
  audit above is what survives it.
