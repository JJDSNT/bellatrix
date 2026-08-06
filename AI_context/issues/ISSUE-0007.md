---
id: ISSUE-0007
title: "Intermittent Emu68 boot loses task context before Wanderer icons"
status: doing
priority: critical
type: bug
owner: agent
created_at: 2026-08-04
updated_at: 2026-08-06
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

**Status 2026-08-06: measured, and it is sharper than expected.** Ten runs with
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
