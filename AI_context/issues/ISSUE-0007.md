---
id: ISSUE-0007
title: "Intermittent Emu68 boot loses task context before Wanderer icons"
status: doing
priority: critical
type: bug
owner: agent
created_at: 2026-08-04
updated_at: 2026-08-05
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
Emu68 exception. The investigation has proven two independent defects: FAT
directory fields were written in host big endian, and the fast m68k scheduler
saved a 68000-sized frame although Emu68 produces a 68010 format-0 frame.

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
- 2026-08-05 — compared `/home/jaime/AROS` (`feature/m68k-emu68-baremetal`)
  against this tree file by file. One substantive commit had never been
  imported: `d3baf6ed82`, the system-timer compare race. Imported.
- 2026-08-05 — three serial runs with the imported fix, one at a time on an idle
  machine. All three stop after `[SDBus00] MMC0: [256MB Capacity]`; dominant
  colour `#787878` (Emu68 logo) in each. The heartbeat now runs to `0x1000`
  ticks, so the interrupt path is ruled out as the thing that stops.
