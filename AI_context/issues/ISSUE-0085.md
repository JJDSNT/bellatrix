---
id: ISSUE-0085
title: "Refresh the AROS pin to upstream HEAD, and take the driver work that came with it"
status: doing
priority: medium
type: chore
owner: unassigned
created_at: 2026-08-31
updated_at: 2026-08-31
tags:
  - aros
  - upstream
  - sdcard
  - vcgfx
  - audio
  - kernel
blockers:
related_files:
  - patches/aros/0024-sdcard-report-what-the-pio-data-loop-costs.patch
  - patches/aros/0079-vc4gallium-build-emu68-from-project-tree.patch
  - patches/aros/0091-amigavideo-turn-on-the-driver-s-own-tracing.patch
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_bcm2708bus.c
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_bcm2708init.c
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_intern.h
  - aros/workbench/devs/AHI/Drivers/pwmaudio/rpipwm-hwaccess.c
  - AI_context/issues/ISSUE-0065.md
---

# Summary

The AROS submodule was pinned at `fbea2d8b8d` (2026-08-26). Upstream `master`
is at `3fb8adfd3a` (2026-08-31), **172 commits** ahead.

This port's board drivers are forks of `arch/arm-native/soc/broadcom/2708/`
and of the aarch64 raspi target, so a pin bump is two separate pieces of work:

1. **The bump itself** — re-anchor the patch series. Done below; the series
   applies clean at HEAD.
2. **The drivers** — upstream changed code we hold a *copy* of, so none of it
   arrives with the bump. Each change has to be read and decided on. That
   list is the body of this issue.

The two are independent: the bump is safe without the ports, and the ports can
land one at a time afterwards.

## Where this stands (2026-08-31)

| | |
|---|---|
| Pin bumped, series re-anchored | **done** — `setup.sh --verify` clean, lean build exit 0 |
| 3.1 timer | **done**; boots without crashing, effect not yet measured |
| 3.2 sdcard clock divisor | **done**; card reads fine at the new clock, throughput not measured |
| 3.3 bulk PIO read | **done** — without upstream's byte swap |
| 3.4 IPTR casts | **done** |
| 3.5 pwmaudio jack gate | **rejected**, see below |
| 3.6 per-task CPU usage | **implemented**, both metrics — see below |
| 3.10 `ShutdownA()` | **implemented** — the port had none at all |
| `aros-contrib` pin | **bumped** to `4b9155332`, its two patches apply unchanged |

Nothing below the bump has been booted yet. A clean build is not evidence for
3.1 or 3.2: the first says the clock now advances between ticks, the second
changes the speed the card is clocked at, and only a boot can speak to either.

# Part 1 — the series, re-anchored

Trial-applied against `3fb8adfd3a` in a scratch worktree. Of the 69 numbered
patches, 65 applied untouched (as did the unnumbered
`optional-debug-turn-on-tracing-in-dos-lddemon-and-shell`, which is not part
of the series `setup.sh` globs). The four that did not:

| Patch | Why | Resolution |
|---|---|---|
| `0024-sdcard-report-what-the-pio-data-loop-costs` | upstream restructured the very loop it brackets (`sdcb_IOReadLongs`, below) | re-anchored; the accounting now brackets the whole read/write branch, bulk path included |
| `0079-vc4gallium-build-emu68-from-project-tree` | upstream added an include line to `workbench/libs/mesa/mmakefile.src` inside our hunk's context | re-anchored, **and the new line gated** — see below |
| `0081-bluetooth-do-not-drain-cn-waitreqs-in-place` | upstream carries the same fix | **deleted** |
| `0091-amigavideo-turn-on-the-driver-s-own-tracing` | upstream inserted `OPTIMIZATION_CFLAGS := $(ROM_OPTIMIZATION_CFLAGS)` into our context | re-anchored |

After that: **68 numbered patches, all applying with `git apply` — strict, no
fuzz, which is what `setup.sh` uses** — to upstream HEAD.

## 0081 is upstream now

`1f571f40f0` (Nick Andrews, 2026-08-26) —
*"bluetooth.library: never drain cn_WaitReqs in place in bConnUp"* — is
ISSUE-0062's fix, and a superset of ours: besides routing through
`bDispatchWaiting()`, it releases parked requests from `bConnFinishEnum()`.
The patch is deleted rather than rebased. Nothing to port; the bump delivers
it.

## The gate added to 0079

`4918094696` (*mesa3dgl: honour VC4_RENDER_SCALE only where the HVS can scale
a plane*) added to `workbench/libs/mesa/mmakefile.src`:

```make
USER_INCLUDES += -I$(SRCDIR)/arch/arm-native/soc/broadcom/2708/include
```

`mesa3dgl_support.c` now does `#include <hardware/bcm2708.h>` under
`MESA3DGL_HAVE_COREAPI`, which patch 0079 defines for `emu68` too. That
include path would hand our target the **arm-native** `hardware/bcm2708.h`,
which is written against `ARM_PERIIOBASE`, in preference to the one this port
keeps in `arch/m68k-emu68/include/`. 0079 now gates it to
`-I$(SRCDIR)/arch/m68k-emu68/include` on `emu68`.

Both headers define `BCM2711_PERIIOBASE`, so the render-scale gate reads the
same either way; the hazard is the rest of the header, not this symbol.

# Part 2 — what the bump delivers for free

Shared code, no fork, arrives with the pin. Worth *verifying* rather than
porting — most of it is on the card path, which is this port's boot path.

- `rom/devs/sdcard/sdcard_bus.c`: **`sdcb_CmdError`**. Controller errors were
  read back out of `sdcb_BusStatus`, which the next interrupt overwrites — so
  an error could be erased before the waiting task looked, or a stale one
  inherited by a command that never failed. `WaitCmd()` now reads a field the
  IRQ only ever sets.
- same file: on a controller reset the IRQ now drops `sdcb_RespListener` /
  `sdcb_DataListener` and clears `AF_Bus_DMAActive|AF_Bus_DMABounced`, instead
  of letting `WaitCmd()`'s recovery run `FinishCmd()`/`FinishData()` over a
  wiped controller.
- same file: `sdcUnit->sdcu_Bus->sdcb_BusFlags` is now `|= AF_Bus_MediaPresent`,
  not `=` — it was clobbering the DMA and interrupt-delivery bits the bus had
  already established.
- same file: CMD7 unacknowledged is now a `FALSE` return with a message. It
  used to fall through, skip the transfer clock and bus width, and surface
  much later as a bare command timeout.
- `rom/devs/sdcard/sdcard_ioops.c`: a chunk whose stop was never acknowledged
  no longer counts towards `*act`.
- `rom/exec/etask.h`: two new `IntETask` fields (`iet_LastBusy`,
  `iet_LastUsageStamp`) for the CPU-usage sweep — see the kernel item below.

# Part 3 — the ports, ranked

## 3.1 timer: this port has no clock between ticks (highest value)

`765f360dca` (*aarch64-raspi: read the system timer on demand*) fixes, for the
aarch64 raspi target, something **this port has too** — and worse.

`rom/timer` builds `ticks.c` with `archspecific=yes`. There is no
`arch/m68k-emu68/timer/`, so the build takes the generic one. Confirmed
against the existing build tree:

```
$ head -1 out/build/aros/bin/emu68-m68k/gen/rom/timer/timer/ticks.d
  ... /home/jaime/bellatrix/external/aros/rom/timer/./ticks.c
```

and every other timer source resolves the same way — `timer_init`,
`common_init`, `lowlevel`, `timervblank` are all the generic files. The
generic `rom/timer/ticks.c` is the template:

```c
void EClockUpdate(struct TimerBase *TimerBase)
{
    /* This is called whenever timer.device wants to read EClock value from the hardware */
}
```

It does nothing. `EClockUpdate()` is called under `Disable()` at the top of
`GetSysTime()`, `GetUpTime()`, `ReadEClock()` and every `TR_ADDREQUEST` —
*specifically* so the reading is current — and here it returns without
looking at anything.

What advances the clock instead is `rom/timer/timer_init.c`'s `VBlankInt`,
which adds a fixed `tb_VBlankTime` per interrupt. So the whole system's notion
of time is a **software counter stepping in whole tick intervals** —
`platform/bcm283x/system_timer.c` runs the heartbeat at ~50 Hz, so **~20 ms
granularity**, twice as coarse as the 10 ms upstream just called out as a bug.

`SYSTIMER_CLO` is a free-running 1 MHz counter. `system_timer.c` already reads
it on every single tick (`now = systimer_read(SYSTIMER_CLO)`). The microsecond
truth is right there and `timer.device` never asks for it.

**Why this is worth doing first.** Everything measured or paced on this
machine goes through these calls: `Delay()`, `WaitUntil`, the DOS timeouts,
the sdcard PIO accounting `patches/aros/0024` prints, the frame pacing in
`bprof`, `amigavideo` waiting on the beam. Anything integrating motion over
the interval it measures advances in lurches, worse the faster it runs — and
this branch has been chasing exactly that class of symptom.

**Shape of the port.** Mirror `arch/aarch64-raspi/timer/` — three files:

- `ticks.c` — `EClockUpdate()` reads `SYSTIMER_CLO`, takes the delta against
  the last value consumed and `ADDTIME`s it into `tb_CurrentTime` /
  `tb_Elapsed`. `EClockSet()` stays empty: the counter is read-only and
  free-running.
  - **`AROS_LE2LONG`, unlike upstream.** `arch/aarch64-raspi/timer/ticks.c`
    dereferences the register raw, which is right there only because that CPU
    is little-endian. Emu68 maps the peripheral block straight through — see
    the note above `systimer_read()` in `platform/bcm283x/system_timer.c`.
  - **32-bit, and `CHI` deliberately not read.** Unsigned subtraction is
    correct across the counter's ~71 minute wrap for any two readings less
    than that apart, and every timer.device entry point plus the heartbeat
    calls this, so consecutive readings are milliseconds apart. `CHI` would
    mean 64-bit division, which on m68k is a libgcc call in ROM-resident code
    for no gain. It stops being sound only if nothing asks the time for over
    an hour, which here means the heartbeat has stopped — already the larger
    problem.
- `timer_platform.h` — the generic `struct PlatformTimer` has only
  `tb_TimerIRQNum` and `tb_VBlankTime`; this adds `tbp_periiobase` and
  `tbp_CLO`. `rom/timer/mmakefile.src` already puts
  `-I$(SRCDIR)/arch/$(CPU)-$(ARCH)/timer` ahead of its own directory, so
  **every** `rom/timer` source picks this header up — there is no half-tree
  compiled against one layout and half against another.
- `timer_init.c` — **required, not optional**. The generic one adds
  `tb_VBlankTime` per tick; leaving it in place alongside a real
  `EClockUpdate()` double-counts every interval. Ours keeps the generic file's
  shape and changes one thing: the tick calls `EClockUpdate()`.

**On the address, since it is a fair thing to ask.** Nothing here is a new or
invented address. `tbp_periiobase` comes from
`KrnGetSystemAttr(KATTR_PeripheralBase)`, which `kernel/getsystemattr.c`
documents as *"the supported way"* for a driver in its own module to find the
peripheral window, and which `sdcard.device` already uses in production on
this hardware. It resolves to the same place `platform/bcm283x/system_timer.c`
reads: `platform_periiobase` is the parent base of `/soc`'s first `ranges`
entry, and `soc_translate()` maps a child address to `parent_base + offset`,
so `SYSTIMER_BASE = ARM_PERIIOBASE + 0x3000` **is** that driver's `node->base`
by construction. The `0x04` for `CLO` is register layout, already used and
confirmed there.

The module boundary is why it goes through `KrnGetSystemAttr()` rather than
calling the platform layer: each module is linked with `--localize-symbols`,
so `timer.device` cannot reach `system_timer.c`'s statics.

## 3.2 sdcard: the clock divisor is wrong here, and has been

`aros/arch/m68k-emu68/soc/sdcard/sdcard_bcm2708bus.c` was forked before
upstream corrected `GetClockDiv()`. Ours:

```c
for (__BCMClkDiv = 0; __BCMClkDiv < V300_MAXCLKDIV; __BCMClkDiv++) {
    if ((bus->sdcb_ClockMax / (__BCMClkDiv + 1)) <= speed)
```

Upstream's:

```c
/*
 * The value programmed into CLOCK_CONTROL is not a divisor: SDHCI runs
 * the card at base/(2*N), and N of zero is the special case that passes
 * the base clock straight through.
 */
if (speed >= bus->sdcb_ClockMax)
    return 0;

for (__BCMClkDiv = 1; __BCMClkDiv < (V300_MAXCLKDIV / 2); __BCMClkDiv++) {
    if ((bus->sdcb_ClockMax / (__BCMClkDiv * 2)) <= speed)
```

The field is not a divisor — the controller runs at `base/(2*N)`. Ours picks
`N` as though it were `base/(N+1)`, so **the card is clocked at about half
what every caller asked for**, identification and transfer alike. Nothing
reports it; it just runs slow.

This is the cheapest throughput change available on the card path, and it is
worth measuring against `patches/aros/0024`'s own PIO accounting, which is
already in the tree for exactly this question.

## 3.3 sdcard: the bulk PIO data-port read — port it, but not its byte order

`e98d741e98` adds `sdcb_IOReadLongs`, an 8-way unrolled bulk read of
`SDHCI_BUFFER`, and `rom/devs/sdcard/sdcard_bus.c` uses it in place of one
indirect call per longword. On a JITted m68k that per-word call overhead is
paid 128 times per 512-byte block, so this should show up plainly in the
`[SDBus%02u] PIO:` line.

**It cannot be copied as written.** Upstream's version applies `AROS_LE2LONG`
to every word it reads off the data port:

```c
dest[i] = AROS_LE2LONG(*port);
```

That is the exact bug ISSUE-0065 was — the data port is a byte stream, not a
register, and swapping it reverses every four bytes of user data while
reporting success. It is invisible on little-endian ARM and fatal here. Our
fork already carries `sdc_is_data_port()` and the long comment explaining it;
the ported `BCMMMIOReadLongs` must read the port **raw**, with no conversion
at all, and the comment should say why it differs from upstream's.

See `bellatrix-pio-fifo-byte-order-trap` — this is the same trap arriving from
the other direction, now embedded in code we would otherwise copy verbatim.

The upstream-side answer is still the one our comment already names: a
separate vtable entry for the data FIFO in `rom/devs/sdcard`, distinct from
the register accessors. `sdcb_IOReadLongs` is *nearly* that entry — it exists
only for `SDHCI_BUFFER` — so upstreaming "the data FIFO accessor does not
byte-swap" is now a small change rather than a redesign, and would also fix
the latent bug in the big-endian `raspi-armeb` target.

## 3.4 sdcard: things to skip, deliberately

- `02e77729de` (MMIO through `IPTR` rather than `ULONG`) — cosmetic on a
  32-bit target, but take it to keep the fork readable against upstream.
- `9356f3d2ad` (controller and clock setup under `DINIT`) — cheap, take it.
- EMMC2 / BCM2711 / BCM2712 controller selection, the `CardPresent()`
  legacy-window fallback, and the `BCMMMIODirectWrite*` accessors: Pi 4 and
  Pi 5 only. This port is Arasan on a Pi 3. **Skip**, and let the fork stay
  narrower than upstream on purpose.
- `SDHCI_INT_ADMA_ERROR` in the interrupt mask: ours is PIO-only and has no
  ADMA to fail. Skip while that holds.

## 3.5 pwmaudio: refuse to load where there is no jack -- REJECTED

`aros/workbench/devs/AHI/Drivers/pwmaudio` is a fork of `RPiPWM`. Upstream
added `pwm_audio_present()`: `DriverInit()` now returns `FALSE` when the
device tree says the board has no 3.5mm jack — an `audio_pins` node with an
empty `brcm,pins` list (Pi 400, Compute Modules, Zero 2 W), or no node at all
(Pi 5/500). A machine that hands over no tree keeps the built-in defaults and
stays playable.

It looks small, self-contained, and a match for how this port already reads
the tree (`platform/fdt.c`). **Do not take it**, at least not as written.

The gate makes the driver's existence depend on resolving
`OF_OpenKey("/__symbols__")` and then the `audio_pins` symbol. Nothing on this
port guarantees that node: `platform/fdt.c` copies whatever tree Emu68 hands
over, and `__symbols__` is only present when the DTB was built with `-@`. It
is nowhere in `fdt.c` or `platform.c`, and no boot here has been shown to find
it.

Today a failed lookup falls through to the built-in defaults --
`pwm_gpio_pin[] = { 40, 45 }`, `pwm_gpio_fsel = 4` (ALT0) -- which are the
values confirmed on real hardware, and which `51a300c` (*the DMA was never
given an address it could reach*) shows were not cheap to arrive at. With the
gate, that same fall-through becomes "this board has no jack" and
`DriverInit()` returns `FALSE`: audio disappears, silently and completely.

The boards upstream is protecting -- Pi 400, the Compute Modules, Zero 2 W,
Pi 5/500 -- are not what this port targets, and the Pi 3 has the jack. So the
change buys nothing here and risks everything the audio bring-up established.

If it is ever wanted, it has to distinguish *the tree says there is no jack*
(node found, `brcm,pins` present and empty) from *the lookup did not resolve*,
and only the first may refuse to load.

## 3.6 kernel: per-task CPU usage — new work, and cheaper here than expected

`3fce84d972` fills in `iet_CpuUsage` on arm, which had always read zero, by
sweeping tasks from the VBlank timer IRQ the way x86 does from the APIC
heartbeat. `TaskTag_CPUUsage` (`rom/task/QueryTaskTagList.c:163`) does nothing
but read that field, so whatever fills it makes `xSysInfo` work — and this
branch already puts `tests/sysinfo/xSysInfo` on the card. The two `IntETask`
fields the sweep needs (`iet_LastBusy`, `iet_LastUsageStamp`) arrive with the
bump.

**No m68k has ever had this.** `iet_CpuUsage` is written by exactly three
arches — `all-pc`, `arm-native` (only since this bump) and `riscv64-opensbi`.
The sibling field `iet_CpuTime` is written by twelve files, all x86, arm,
aarch64, riscv, ppc or unix-hosted. Nothing under `arch/m68k-amiga` or
`arch/m68k-all` touches any of them, and upstream's `m68k_SwitchTail`
(`arch/m68k-all/kernel/kernel_cpu.c:30`) is the same as our
`emu68_SwitchTail`: save FPU/AMMX, set `tc_SPReg`, call `core_Switch()`, no
accounting. So there is nothing to port from a m68k that already had it, and
no m68k reference implementation to validate against. Validation has to be
against known load: one task in a tight loop should read near 100%, two equal
ones near 50% each.

### Why the usual m68k objection does not apply here

The reason every other arch timestamps each context switch is that it has a
CPU register to read — `TSC` on x86, `CNTPCT` on arm. The obvious reading is
that m68k has no such register and the only clock is the BCM system timer
across the peripheral window, which would make the accurate route an uncached
MMIO read on every context switch — in a path (`exec/dispatch.S`) written
tightly enough to decline even one Paula write per dispatch.

That reading is wrong on this target. **Emu68 exposes counters to the guest
through `MOVEC`**, and has since 2021 (`e401068`, Michal Schulz, *"Add new
locations and comments to Emu68 movec registers"*) — stock, not from any patch
of ours:

| Register | Contents |
|---|---|
| `0x0e0` `CNTFRQ` | counter clock speed, Hz |
| `0x0e1`/`0x0e2` `CNTVALLO/HI` | the ARM generic counter, 64-bit |
| `0x0e3`/`0x0e4` `INSNCNTLO/HI` | m68k instruction counter |
| `0x0e5`/`0x0e6` `ARMCNTLO/HI` | ARM instruction counter |

and the read is as cheap as it gets (`src/M68k_LINE4.c:2223`):

```c
case 0x0e1: /* CNTVALLO - lower 32 bits of the counter */
    EMIT(ctx, mrs(tmp, sys_CNTPCT_EL0),
              mov_reg(reg, tmp));
```

`movec CNTVALLO,Dn` JITs to two AArch64 instructions. No MMIO, no bus access,
no trap. That is the same free register read x86 and arm rely on, so the cost
objection to timestamping every switch disappears.

### What that makes possible

Being two instructions, the timestamp can go in the **assembly itself**, which
removes the two holes that otherwise sink the accurate route on this port:

- `Switch` already calls C (`jsr emu68_SwitchTail`, `kernel/context.c:74`), so
  that half was never the problem.
- `m68k_VoluntarySwitch` — the `Wait()` path — jumps straight to
  `__Dispatch_this` and never touches C, and `Dispatch`'s fast path is pure
  assembly. A task that blocks would otherwise keep accruing until some other
  task suffered an involuntary switch, i.e. the numbers would be wrong in the
  common case.

What is left is 64-bit accumulation into `iet_private1`/`iet_private2`, which
in m68k assembly is `add.l`/`addx.l`.

### Constraints on how it is written

- **Use only `0x0e0` and `0x0e1`.** The harness's Musashi backend
  (`external/rigel/external/musashi`, `m68k_in.c:6748`) implements exactly
  those two, deliberately returning *emulated* CPU time rather than wall clock
  so a benchmark "measures the machine being emulated and gives the same
  answer every run". `0x0e2` (`CNTVALHI`) and `0x0e3`/`0x0e4` (`INSNCNT`)
  appear nowhere in it and fall through to `m68ki_exception_illegal()`. A
  kernel that has to run on both backends must stay inside the intersection.
- **So the counter is 32-bit**, with the same wrap reasoning as
  `arch/m68k-emu68/timer/ticks.c`: unsigned subtraction is correct for any two
  readings less than a wrap apart, and context switches are milliseconds
  apart.
- **Do not trust `CNTFRQ` as a rate.** Upstream says so in `4f2d26712b`:
  *"CNTFRQ is firmware-programmed and not always true - QEMU reports 19.2MHz
  while counting at 1MHz"*. It does not matter for this feature — usage is a
  ratio of two quantities in the same units, so the rate cancels — but it
  would matter for anything reporting absolute time.

### The bonus only a JIT can offer

`INSNCNT` counts *guest work done*, immune to how fast the JIT happened to
run. `iet_CpuUsage` wants wall-clock share, so `CNTPCT` is the right source
for it; but an instruction-count metric alongside would say something neither
x86 nor arm can say about their own machines. Emu68-only, and outside the
Musashi intersection above, so it would have to be optional.

### What was built

`arch/m68k-emu68/kernel/cpuusage.c`, filling both numbers.

**One hook, in the assembly.** `exec/dispatch.S` calls it at label `2:`, once
the incoming task is current and before the frame is restored, because every
way onto the CPU passes through there — the involuntary `Switch`, the `Wait()`
path in `m68k_VoluntarySwitch`, and the idle loop's return from
`emu68_DispatchFrame`. One hook where the arm port needs accounting in both
`cpu_Switch` and `cpu_Dispatch`, and it closes both holes named above. Safe at
that point: `%a5` holds the frame and is callee-saved, everything the `jsr` can
clobber is reloaded by the `movem` below it, and the SR is already at level 7,
which the privileged `MOVEC` requires.

The interval between two dispatches belongs entirely to whoever was current
during it, so a single system-wide stamp replaces a per-task "started at" and
no slice has to be opened anywhere.

**`MOVEC` by raw opcode.** The assembler has no name for these registers, so
both words are spelled out: `.short 0x4e7a, 0x00e1`. `0x4e7a` is `MOVEC Rc,Rn`;
the extension word is A/D (1 bit), register number (3), control register (12).

**No 64-bit divide.** The ratio shifts both terms down until the numerator
fits, keeping 16 bits — 0.0015%, far finer than the number means — rather than
pulling libgcc's `__udivdi3` into ROM-resident code.

**Two upstream patches**, because the second metric had nowhere to live and no
way out:

- `0093` — `iet_CpuInsn`, `iet_LastInsn`, `iet_InsnUsage` at the end of
  `struct IntETask`, zero on any target that cannot count instructions.
- `0094` — `TaskTag_InsnUsage`, answered by `QueryTaskTagList` exactly as
  `TaskTag_CPUUsage` is. A number nobody can read is not a measurement.

Validation is against known load, since there is no m68k reference: one task in
a tight loop near 100%, two equal ones near 50% each — and the two metrics
should start to disagree wherever the JIT runs some code more cheaply than
other code, which is the whole reason the second one exists.

## 3.7 Not applicable, recorded so the next bump does not re-read them

- `4f2d26712b`, `bdcca28ad3`, `dd6abb8372`, `1df0747c2b`, `9fa72e5b22`,
  `1697163fba` — ARM generic timer, per-core scheduler quantum, SMP secondary
  bring-up, ARM clock rate. All are about the ARM CPU running AROS natively.
  Here the CPU is m68k under Emu68 and the tick comes from the BCM system
  timer through the IPL bridge. Nothing to take.
- `765f360dca` (*aarch64-raspi: read the system timer on demand*) — replaces a
  no-op `EClockUpdate`, so `GetSysTime()` had tick resolution. **Check whether
  the same is true here** before dismissing it: this port takes `timer.device`
  from `kernel-timer-kobj` and the CIA path, and if its `EClockUpdate` is also
  a no-op, everything built on `GetSysTime()` advances in 10ms lurches. Not
  verified yet — that check is the actionable part of this item.
- `3230580d51`, `4918094696`, `f83ebdc86f`, `983cc6704b` (vcgfx HVS channel
  discovery, V3D 4.2 gallium hidd, `v3d/` moving in beside the other hidds) —
  all HVS5 / BCM2711. Our `vcgfx_hvs.c` (HVS4) is untouched upstream. The one
  piece worth a second look later is the *idea* in `3230580d51` — asking the
  firmware which HVS channel and pixelvalve it is already driving instead of
  assuming — which is the same class of question ISSUE-0083 and the vcgfx
  work on this branch have been answering by hand.

## 3.8 The sweep, restated: nothing else in this range touches our forks

Re-run after the bump, intersecting every `.c`/`.h` upstream changed against
every filename this port holds a copy of. The result is the same list Part 3
already covers — sdcard, vcgfx, vc4gallium, RPiPWM, the aarch64 timer — plus
three that need only a verdict:

- `vc4gallium/vc4_init.c` — a one-word typo fix in a comment (`hidds/v3d` →
  `hidd/v3d`). Nothing.
- `vcgfx/vcgfx_hidd.h` — `vcsd_VC4GalliumLib` renamed `vcsd_GalliumLib`, part
  of the BCM2711 v3d work. Cosmetic here; taking it would only be to keep the
  fork readable, and the surrounding change is not one we take.
- `vcgfx/vcgfx_hvs.h` — three fields (`hvs_Channel`, `hvs_PVOffset`,
  `hvs_PVIrq`) read only by `vcgfx_hvs5.c`, which is HVS5 and which we do not
  fork. Nothing to do.

New upstream files in areas we fork: `arch/aarch64-raspi/timer/ticks.c`
(**taken**, 3.1), the whole of `hidd/v3d/` (BCM2711, skipped),
`rom/task/AddTaskNotifyHook.c` and `RemTaskNotifyHook.c` (shared code, arrives
with the bump), and `arch/aarch64-native/exec/shutdowna.c` — which is worth a
line: this port has **no `ShutdownA()` at all**, so `Shutdown()` falls through
to the generic one. The aarch64 implementation is an SVC into its own
kernel syscall layer, so it is a model rather than something to copy. Out of
scope for a pin bump; noted so it is not mistaken for a regression.

`arch/aarch64-raspi/hidd` and `arch/aarch64-native/hidd`, where our `fbgfx`
came from, are untouched in this range.

## 3.9 A finding, deliberately not acted on: the activity LED pin

`969a1c762e` (*raspi: take the LED wiring from the device tree*) says of the
old code that *"the kernel drove fixed pins that were right for the Pi 2
only"*, and its new comment is specific:

> A Pi 2 puts PWR on GPIO 35, a Pi 400 on 42. The Pi 3B and 4B hand theirs to
> firmware controllers (expgpio, virtgpio) this code cannot reach — those keep
> the state firmware left.

Our `soc/sdcard/sdcard_bcm2708bus.c` `BCMLEDCtrl()` drives GPIO 47 through
`GPSET1`/`GPCLR1` for anything that is not a BCM2835. That is the Pi 2
wiring, and this port targets a Pi 3 — where upstream's new code deliberately
does nothing, because the LED is not on a pin this code can reach.

**Not changed.** It is a GPIO pin number on real hardware, and the standing
rule here is that those are not touched on inference. What is needed first is
an observation on the board: whether the activity light responds to card
traffic at all today, and whether GPIO 47 is doing something else on a Pi 3B
that we would rather not be writing to. Recorded so the question is asked,
not so the number is changed.

## 3.10 ShutdownA(): the port had none, and this machine has something to ask

Turned up by the completeness sweep (3.8) rather than by the diff: upstream
added `arch/aarch64-native/exec/shutdowna.c` in this range, and looking for our
equivalent found that there isn't one. `Shutdown()` fell through to
`rom/exec/shutdowna.c` — run the reset callbacks, return 0, leave the board
exactly as it was.

That is the honest answer for a machine with nothing to ask, and this is not
that machine. Emu68 owns the bare metal of a Raspberry Pi, and the Pi has a
power-management block that both resets and halts it.

`arch/m68k-emu68/exec/shutdowna.c` drives it, taking the sequences from
`arch/aarch64-native/kernel/syscall.c` — the same silicon, from the other side
of the same peripheral window. **No address here is new**: the block base is
the one this port already carries as `GPIO_PADS`
(`ARM_PERIIOBASE + 0x100000`; the pads registers live inside the PM page, at
`+0x2c`), and the register offsets and the `0x5a` password are that driver's,
unchanged.

Two things could not be copied:

- **Byte order.** Those ports dereference these registers directly, which is
  right only because ARM runs little-endian there. Emu68 maps the block
  straight through to a big-endian guest, so every access converts — the same
  rule as `platform/bcm283x/system_timer.c`.
- **What to do when nothing happens.** The arm-side implementations spin on
  `wfe` forever after arming the watchdog. `PM_WDOG` counts in 1/65536 s and
  is given ten, so it expires well under a millisecond; anything past that
  means nothing is listening — an emulator that does not model the PM block,
  most likely. Here the wait is bounded and then it returns with a message,
  because turning "cannot power off" into a hang is worse than reporting it.

Unverifiable by building: whether QEMU models the watchdog at all. If it does
not, the bounded path is what runs, which is why it exists.

# The boot, and what it cost to get there

The first boot after all of Part 3 died before the desktop:

```
[InitResident] timer.device: MakeLibrary 0 ms, calling init @ 0x3463283c
[Kernel:TLSF] free-list corruption at REMOVE_HEADER: ... block=02037180
              size=2739834480 flags=0x1
#  Software Failure!  Task : Exec Bootstrap Task  Error: 0x80000027
```

Symbolised against the ELF (it is loaded at `0x34600000`, so subtract that and
look the offsets up with `nm`), the trace is
`coldstart_user → InitCode → InitResident → Timer_InitLib → set_libinit →
Timer_Init +0x84 → AllocMem → nommu_AllocMem → tlsf_malloc →
tlsf_fail_corruption`.

A diagnostic in `Timer_Init` gave the shape of it exactly:

```
[timer] DIAG base=020370d0 sizeof(TimerBase)=182 &tbp_CLO=02037182 delta=178
[Kernel:TLSF] ... block=02037180 ...
```

The base is 182 bytes by the arch build's reckoning, but the next TLSF block
starts at `+176`. `tbp_CLO` is written at `+178`, inside the neighbouring
block's header, and the very next `AllocMem` walks the free list and finds it.

## The cause was a stale object, and the mechanism is worth knowing

`Timer_InitTable`'s first longword — `LIBBASESIZE`, i.e.
`sizeof(struct TimerBase)` as `timer_start.c` saw it — read `0xae` = 174.
`arch/.../timer_init.c` saw 182. Same header, same struct, `__AROSEXEC_SMP__`
undefined in both (checked by compiling a probe with the arch build's own
flags).

Same tree, same object deleted first, `make kernel-timer-kobj`:

| | `Timer_InitTable` |
|---|---|
| normal | `0x000000ae` = **174** — the *generic* `timer_platform.h` |
| `CCACHE_DISABLE=1` | `0x000000b6` = **182** — ours |

**ccache's direct mode.** `arch/m68k-emu68/timer/timer_platform.h` is a *new*
file that shadows `rom/timer/timer_platform.h` from earlier on the include
path. ccache decides by manifest — the headers the previous compile opened.
The new one is not in it, the ones that are have not changed, so it is a hit,
and the object it returns was compiled against the header ours now displaces.
Deleting the `.o` does not help: the same false hit is served again.

**And the dependency file lies about it.** `timer_start.d` correctly named our
header the whole time, which is why this took as long as it did to see:
`Makedepend` runs gcc directly, without ccache, so the `.d` is right while the
`.o` beside it came from the cache. A `.d` is not evidence that the object was
compiled against what it lists.

This matters well beyond the timer. **This repository's whole mechanism for
carrying its own code is to inject `arch/m68k-emu68` into the AROS tree by
symlink, so shadowing upstream files is the normal way of working here.** Every
new header added to the injected tree can do this again. The way out is one
`CCACHE_RECACHE=1 ./scripts/build-aros.sh`, or `CCACHE_DISABLE=1` for a
localised target.

Worth considering, and not done here because it trades build time for safety:
having `setup.sh` notice when the injected file *set* changes (not its
contents) and make the next build recache.

## What the boot established, and what it did not

With the ccache poisoning cleared, the machine boots. No `Software Failure`,
no TLSF corruption, `timer.device` initialises, `[intc] irq 3 dispatched 8192
times` — the heartbeat fires — and the boot loads the whole desktop stack from
the card: SetPatch, the USB classes, muimaster, coolimages, workbench.library,
`IconVolumeList.mui`, `IconListview.mui`, the datatypes.

**So the four changes do not crash the machine, and the card is readable at
the new clock** (3.2 doubles it; every module above came off the card).

It does not reach the desktop. Activity stops after `png.datatype` finishes
its `InitResident`, and the CPU parks in `emu68_DispatchFrame +0x2a` — Exec's
idle loop — while interrupts keep arriving and the chipset keeps running. The
BootUI sequence gets to `STARTING DOS...` and never emits `STARTING
SERVICES...` or `STARTING WANDERER...`.

**This is not attributed.** Two things are known:

- **It is not the timer.** Neutralising `EClockUpdate()` back to the generic
  template and restoring the fixed-period tick gives *the same stall at the
  same place* — `png.datatype`, to the line. A timing change that made no
  difference to where it stops is not what stops it.
- **The obvious baseline is not a baseline.** `boot-i.log` in the repository
  root predates this work and does reach `STARTING WANDERER`, but its early
  BootUI messages (`retargeted to RGB32 framebuffer`, `display owned by a
  native driver` at 2.1s) do not appear here at all, and its ELF loads at a
  different address. It is a different configuration, so the comparison
  proves nothing either way.

What would settle it is a like-for-like boot of the pin bump alone, with none
of Part 3 applied. That has not been done. Until it is, "the desktop does not
come up" cannot be called a regression from this work — note that `ISSUE-0078`
already records the desktop as untestable on this branch for an unrelated
reason.

Also unverified for want of a boot that gets far enough: the PIO accounting
line from `patches/aros/0024` never appears, so 3.2's effect on card
throughput has not actually been measured, only reasoned about.

# Doing it

The bump itself, from a clean `./scripts/setup.sh --verify`:

```bash
# 1. clear skip-worktree while every patch that set it still exists
git -C external/aros ls-files -v | grep '^S' | sed 's/^S //' \
  | tr '\n' '\0' | xargs -0 git -C external/aros update-index --no-skip-worktree

# 2. drop what the series created and the directories we inject, then go pristine
awk '/^diff --git /{f=$4; sub(/^b\//,"",f); next} /^new file mode /{print f}' \
    patches/aros/[0-9]*.patch | sort -u \
  | (cd external/aros && xargs rm -f)
rm -f external/aros/arch/m68k-emu68 external/aros/contrib \
      external/aros/workbench/devs/AHI/Drivers/hdmiaudio \
      external/aros/workbench/devs/AHI/Drivers/i2saudio \
      external/aros/workbench/devs/AHI/Drivers/pwmaudio
git -C external/aros reset --hard
git -C external/aros clean -fd

# 3. bump
git -C external/aros checkout 3fb8adfd3a06d233a4d635be66370a8206cfb94e

# 4. re-apply and confirm
./scripts/setup.sh
./scripts/setup.sh --verify
```

Step 1 is the one that cannot be skipped: `git reset --hard` silently ignores
`skip-worktree` entries, and patch `0081` is gone, so its file would otherwise
stay marked and stay patched forever (CLAUDE.md, *Deleting a patch needs one
step first*).

Then a lean build (`./scripts/build-aros.sh`) as a smoke test before anything
in Part 3 is attempted, and before a `full`.

## The build trap this bump walked into

The first lean build after the bump died on:

```
m68k-aros-gcc: error: @rom_optimization_cflags@: No such file or directory
```

`31fceee88e` (*Build ROM-resident code for size via ROM_OPTIMIZATION_CFLAGS*)
added a substitution to `configure`. The build tree was configured against the
old one, so `config/make.cfg` never learned the name and the literal `@...@`
reached the compiler as a filename.

Nothing forced the reconfigure, and that was **our** bug, not upstream's.
`build-aros.sh` carried a shortcut for the case where `setup.sh --reset`
rewrites every mtime without changing a byte — it touched `config.status` past
`configure` to avoid rebuilding the world. Its comment claimed an mtime with
identical content was *"the only kind of change a submodule checkout can
produce"*. A pin bump is the counter-example, and the shortcut turned a
genuine configure change into silence.

Fixed in `scripts/build-aros.sh`: the tree now stamps a digest of `configure`
at configure time (`.bellatrix-configure-digest`), and a mismatch forces the
reconfigure before the mtime shortcut is ever reached. A tree with no stamp is
one configured before the check existed — it stays quiet and adopts the digest
at its next configure, which is why **this** bump still needed
`rm -f out/build/aros/mmake.config` by hand.

So, for the next bump: nothing to do. For a tree configured before
2026-08-31: if a build dies on an unsubstituted `@variable@`, remove
`mmake.config` and let it reconfigure.
