# AROS/Emu68 m68k

This target produces an ELF32, big-endian m68k image for the native Emu68
initramfs loader. Emu68 owns the Raspberry Pi bare-metal environment; AROS
starts only after Emu68 has initialized the machine and loaded the ELF.

Configure and build with:

```sh
../AROS/configure --target=emu68-m68k
make AROS-emu68-m68k
```

The output is:

```text
bin/emu68-m68k/AROS/aros-emu68-m68k.elf
```

Select it in the Raspberry Pi `config.txt` alongside `Emu68.img`:

```text
initramfs aros-emu68-m68k.elf
```

The bootstrap translates Emu68's register ABI into `Emu68BootContext`, validates
the flattened device tree, records its first 32-bit memory range and preserves
the framebuffer and `/chosen/bootargs` information. The ELF carries relocatable
core, OOP/HIDD, timer and graphics residents. It initializes a TLSF memory
header from the FDT RAM range, creates `SysBase`, runs the SINGLETASK and
COLDSTART resident levels, enables multitasking and enters the Exec scheduler.

## m68k Exec ABI

The classic m68k Exec ABI documents `Permit()`, `ObtainSemaphore()`,
`ReleaseSemaphore()` and `ObtainSemaphoreShared()` as preserving
`D0-D1/A0-A1`. Their C implementations use the normal AROS register-clobber
convention, so an m68k bootstrap must install preserving vector wrappers after
`krnPrepareExecBase()` has completed.

Historically this adaptation lived in the Amiga board bootstrap even though it
contains no Amiga chipset knowledge. The wrappers and their installer now live
in `arch/m68k-all/exec`; the Emu68 bootstrap explicitly installs them after
creating `SysBase`. The helper is inert for other m68k targets until their own
bootstrap calls it. This provides a path for Atari, Macintosh or other m68k
targets to adopt the same ABI fix independently, without changing their
hardware boundary or boot sequence.

The target also links `libamiga.a`. Despite its historical name, the code used
here is the compiler `alib` compatibility layer, providing ABI-level helpers
such as `StrDup()` and `GetDataStreamFromFormat()`. Linking it does not import
CIA, Paula, Gayle, custom-chip or other Amiga hardware dependencies.

## Emu68 framebuffer HIDD

`emu68gfx.hidd` is the AROS-side adapter for the linear framebuffer handed to
the m68k ELF by Emu68. Emu68 remains responsible for initializing the physical
display and passes the address, pitch, width and height in the entry ABI. The
driver advertises one fixed RGB565 little-endian mode matching that contract.

Displayable AROS bitmaps remain ordinary managed chunky bitmaps. The driver
declares `aHidd_Gfx_FrameBufferType = vHidd_FrameBuffer_Direct` and hands
`graphics.library` a `CLID_Hidd_ChunkyBM` bitmap bound to the real framebuffer
memory; the generic `Display`/`BitMap` classes then handle `Show()` and
`UpdateRect()` themselves, re-linking a screen's classic `BitMap` to that
framebuffer bitmap whenever it becomes visible. This keeps Raspberry Pi
mailbox, VideoCore and other bare-metal details outside AROS while preserving
the normal graphics HIDD and `graphics.library` boundary - and it avoids
reimplementing bookkeeping the generic code already does correctly.

### Bring-up bugs

Three bugs blocked the boot screen before it rendered correctly:

- **Swapped colors.** The pixel format is declared `RGB16_LE`, but m68k is
  big-endian while the framebuffer is not. Without
  `aHidd_PixFmt_SwapPixelBytes`, `graphics.library`'s color-to-pixel
  conversion skips the byte swap it applies for every other big-endian
  target's little-endian formats, so every color came out with its bytes
  swapped (a solid pink/garbled screen).

- **Nothing ever updated past the first screen.** An earlier version of this
  driver implemented its own `Show()`/`UpdateRect()` instead of declaring
  `aHidd_Gfx_FrameBufferType`. Without that attribute, `graphics.library`
  never re-links a screen's classic `BitMap` to the real framebuffer object
  when a new screen is shown, so drawing landed on an offscreen buffer that
  was never displayed (solid gray, nothing updating - only the very first
  screen's initial paint ever reached VRAM).

- **Silent hang creating a second screen's bitmap.** `HIDD_Display_CreateObject`
  has a generic shortcut: a displayable bitmap with no class of its own
  inherits the class of the existing framebuffer bitmap. Since this driver
  has no hardware cursor, that framebuffer bitmap is wrapped by
  `graphics.library` in a `CursorFB` proxy object. Inheriting *that* class and
  instantiating it via a plain `OOP_NewObject()` - instead of through its own
  `create_cursorfb()` constructor - produced a `CursorFB` instance whose
  "real bitmap" pointer was never set. Any attribute `Get()` on it (starting
  with `Width`) then hung forwarding to that null reference. The fix is to
  always name the bitmap class explicitly (`CLID_Hidd_ChunkyBM`) in
  `CreateObject()` rather than relying on the inherit-from-framebuffer
  shortcut - the same pattern `vc4gfx` (Raspberry Pi's native framebuffer
  HIDD) already uses.

Emu68 exposes a unified memory domain rather than separate Amiga chip and fast
RAM. The system memory header consequently satisfies both `MEMF_CHIP` and
`MEMF_FAST`. This is needed by generic graphics compatibility paths such as
`AllocSpriteDataA()`, which still request `MEMF_CHIP`; it does not imply the
presence of an Amiga chipset. AArch64 native targets solve the same legacy
requirement by removing `MEMF_CHIP` in their platform allocator.

## Real platform timer and interrupt controller (`platform/`)

This port drives **physical peripherals through a virtual interrupt-delivery
bridge**. The registers are real BCM283x silicon, discovered from the real
board FDT and programmed directly over MMIO; there is no synthetic timer
device on either side. Interrupt *delivery*, however, is not native: the ARM
exception still belongs to Emu68, which translates any physical IRQ into the
m68k EXTER channel. Both halves are described below.

Emu68 hands AROS the real Raspberry Pi FDT (patched only so that `/soc`'s `ranges` point
at the guest-accessible virtual window Emu68 already mapped for its own
peripheral access, in `src/raspi/start_rpi64.c:map_peripheral_ranges()` -
this remap already carries `MMU_ALLOW_EL0`, so the guest gets the same real
register access the host has, no Emu68 changes required); `arch/m68k-emu68/
platform/fdt.c` walks that FDT post-heap, and `platform.c` matches
`compatible` strings under `/soc` against a small static driver table -
`platform/bcm283x/system_timer.c` (`compatible = "brcm,bcm2835-system-timer"`)
and `platform/bcm283x/interrupt_controller.c`
(`compatible = "brcm,bcm2836-armctrl-ic"`) - translating each match's `reg`
through `/soc`'s `ranges` to get a real, guest-writable MMIO address. This is
the same shape `arch/aarch64-native/kernel/platform_bcm2708.c` uses to drive
the identical silicon on bare ARM hardware. Two things differ structurally,
and neither is cosmetic.

### Byte order

BCM283x registers are little-endian. On `aarch64-native` the CPU is too, so a
plain dereference works. Here the guest is **big-endian m68k** and Emu68 maps
the peripheral block straight through without swapping, so every 32-bit MMIO
access has to convert explicitly (`AROS_LE2LONG`/`AROS_LONG2LE`).

This is worth stating plainly because the failure mode is deceptive: reads
*and* writes are reversed, so a write followed by a read-back agrees with
itself. A driver that programs `SYSTIMER_C3` and reads the same value back can
still be handing the hardware a completely different number - which is exactly
what happened here, and why the compare never matched.

### Interrupt delivery

> **Needs one Emu68 change.** Upstream emulates the `INTENA`/`INTREQ`
> registers only on PiStorm variants, so on a standalone build the arm and
> acknowledge below land in ordinary RAM and no interrupt is ever delivered.
> Extending that emulation to non-PiStorm builds is what makes this work; see
> "The fix" under Current status, and `doc/host-interrupts.md`.

Dispatch arrives over the m68k level-6 autovector - Emu68's "EXTER" channel
for a real physical IRQ - rather than an ARM64 vector table entry, so the
right driver has to be found and armed at runtime instead of being link-time
fixed. That much is a straightforward remap.

The part that is not: Emu68's core-0 IRQ fast path
(`src/aarch64/vectors.c`, `curr_el_spx_irq`) drives that channel off an
INTENA/INTREQ shadow it maintains for the guest, and the guest has to speak
that protocol:

- The fast path always records its internal `ARMPending` flag, but only
  raises the m68k level-6 line when the shadow has **both** `INTEN` and
  `EXTER` set. The channel must be armed once at startup, or a physical IRQ
  arrives and is silently dropped.
- It clears `ARMPending` (and drops level 6) only on a guest write to
  `INTREQ` with the SET/CLR bit clear and `EXTER` set. Every level-6 entry
  must acknowledge **the bridge** in addition to the peripheral that fired -
  the same thing `arch/m68k-amiga/kernel/amiga_irq.c`'s `PAULA_IRQ_ACK` does
  after running a server chain.

So this is not "the same path as `aarch64-native` entered through a different
vector". There is an extra layer with its own state and its own acknowledge
contract. `platform.c` implements both halves.

This is also the contract Emu68's own guest-side drivers rely on:
`gic400.library` (Pi 4 GIC-400 support, used by `genet.device`) installs
itself with `AddIntServer(INTB_EXTER, ...)` and never touches the custom-chip
registers at all - it depends on the OS owning level 6 and doing the
acknowledge.

`platform.c` installs one shared level-6 trampoline that hands off to
whichever interrupt controller driver was discovered; that driver decodes
`ARMIRQ_PEND`/`GPUIRQ_PEND0`/`GPUIRQ_PEND1` (same registers, same offsets as
`hardware/bcm2708.h`) to find which real source fired and calls
`krnRunIRQHandlers()`. `arch/m68k-emu68/kernel/kernel_arch.h` wires
`ictl_enable_irq()`/`ictl_disable_irq()` to that same driver, so
`KrnAddIRQHandler()` unmasks real hardware exactly as it does on
`aarch64-native`/`arm-native`.

The system timer driver acknowledges its IRQ and translates it into Exec's
standard `INTB_VERTB` heartbeat. The generic `timer.device` consumes that
heartbeat, so it contains no Emu68, Raspberry Pi, CIA, Paula, or custom-chip
knowledge. Its period is derived from `SysBase->VBlankFrequency`.

The bootstrap probe validates both synchronous `TR_GETSYSTIME` and an
asynchronous 40 ms `TR_ADDREQUEST`. During the latter, the probe task blocks,
the scheduler returns to the bootstrap task, and the platform timer wakes the
probe through the normal Exec device path. A second `TR_GETSYSTIME` also checks
that at least the requested 40 ms elapsed on the device clock.

The scheduler/timer stress probe then:

- queues simultaneous 40 ms and 80 ms requests and verifies their completion
  order;
- cancels a pending five-second request with `AbortIO()` and checks for
  `IOERR_ABORTED`;
- runs two CPU-bound tasks at the same priority while the probe task repeatedly
  blocks;
- submits 120 consecutive one-second requests and requires both worker tasks to
  make progress during every wait.

This two-minute soak exercises asynchronous interrupt completion, ready/wait
task lists, timer request queues, preemption, and equal-priority round-robin.

## QEMU validation

Emu68 itself remains the bare-metal owner. QEMU emulates the Raspberry Pi that
runs Emu68; it does not load the m68k ELF directly.

Build the firmware from an unmodified upstream checkout - this port targets
stock Emu68, so validating against a patched one proves nothing:

```sh
git clone https://github.com/michalsc/Emu68.git
cd Emu68 && git submodule update --init --recursive
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-gnu.cmake \
      -DTARGET=raspi64
cmake --build build -j$(nproc)
gunzip -c build/Emu68.img.gz > build/Emu68.raw.img
```

The build also downloads Raspberry Pi DTBs into `build/firmware/`, so
`bcm2710-rpi-3-b.dtb` comes from the same tree. (The toolchain file pins
GCC 14; with GCC 13 installed, copy it and adjust the two compiler lines.)

### Boot media

`dosboot` needs something to mount. Build an SD image with an MBR partition
table and a single FAT32 partition holding the distribution tree that
`make` leaves in `bin/emu68-m68k/AROS`:

```sh
SD=sd.img
DIST=bin/emu68-m68k/AROS
truncate -s 256M $SD
sfdisk $SD <<'EOF'
label: dos
unit: sectors
start=2048, type=c, bootable
EOF
mformat -i $SD@@1M -F -v AROS -T 522240 ::
mcopy -i $SD@@1M -s -Q $DIST/{C,S,Libs,Devs,L,Classes,Fonts,System,Prefs,Storage,Utilities,Tools,Locale} ::
mcopy -i $SD@@1M $DIST/AROS.boot ::
```

`Locale` has to be there: `S:Startup-Sequence` does
`Assign "LOCALE:" "SYS:Locale"`, and without it the boot console opens with
`Can't find SYS:Locale` (`workbench/c/Assign.c:597`) and every later
`LOCALE:`-relative assign is built on sand.

Do **not** just copy the whole tree. `Developer` alone is 291 MB of SDK and
nothing in the boot path reads it; a 512 MB card carrying it currently stalls
the boot between `AROSMonDrvs` and `preparing console`, which is an open
problem of its own and not worth walking into while bringing something else
up.

`@@1M` is the mtools offset to the partition at LBA 2048; `type=c` is FAT32
LBA, which is what `partition.library`'s MBR handler reports as the
`0x46415402` (`FAT\2`) DOSType that `fat-handler` claims. The unit AROS ends
up with is `SDCARD0P0:`.

Pass it to QEMU with `-sd sd.img`. QEMU's `raspi3b` wires the file to the same
Arasan controller `soc/sdcard` drives, so this is the one piece of the boot
path that is not a stand-in for real hardware.

Then run:

```sh
qemu-system-aarch64 \
  -M raspi3b \
  -kernel /path/to/Emu68.raw.img \
  -dtb /path/to/bcm2710-rpi-3-b.dtb \
  -initrd bin/emu68-m68k/AROS/aros-emu68-m68k.elf \
  -drive file=sd.img,if=sd,format=raw \
  -append "nocomposition" \
  -serial stdio -display gtk -no-reboot \
  -monitor unix:/tmp/emu68-monitor.sock,server,nowait
```

`-display gtk` puts the framebuffer in a window and the serial log keeps
coming out on the terminal; close with `Ctrl-A` `X` in the terminal. Use
`-display none` when only the log matters -- `screendump` on the monitor
socket still works headless, which is how the desktop below was captured.

`nocomposition` is still **required** to see anything, and it is not
`emu68gfx`'s fault -- which is what this file used to guess. Re-measured on
2026-08-22 with `fbgfx` as the boot driver and `vcgfx` installed from
`DEVS:Monitors`, two runs of the same build screendumped at the same point:

| boot argument      | dominant colour        |
|--------------------|------------------------|
| `nocomposition`    | 92.0% `999999` (Workbench grey), 4.3% white, 1.9% title-bar blue |
| none               | 89.6% black, the rest boot-splash leftovers |

So the software compositor still swallows the display with a completely
different pair of drivers, and whatever it expects is missing from all three
of ours rather than from the one that was suspected. With the flag, the
Workbench screen comes up with `RAM Disk` and the boot volume on it.

`nomonitors` is a different flag and must **not** be used here: it sets
`BF_NO_DISPLAY_DRIVERS`, which stops `AROSMonDrvs` running what is in
`DEVS:Monitors` -- and that is now where the VideoCore driver lives. A boot
with it would fall back to the kickstart framebuffer driver and never take the
hardware.

Reaching that desktop needed two things beyond the display driver itself,
neither of them obvious from the symptom:

- `task.resource` in the ROM, without which `stdc.library` cannot initialise
  and `Compositor`, `FixFonts`, `IPrefs` and `Wanderer` all die reporting a
  library they cannot open.
- the `CLI_SYSTEM` fix in `rom/dos/newcliproc.c`, without which the first
  `Execute()` that `AROSMonDrvs` makes never gets its startup packet replied
  and `__dos_Boot()` deadlocks before opening the console.

Connect to the monitor with:

```sh
nc -U /tmp/emu68-monitor.sock
```

### Kernel arguments (`-append`)

QEMU writes `-append` into the DTB as `/chosen/bootargs`, `boot.c` passes it
through as `KRN_CmdLine`, and `PrepareExecBase` (`rom/exec/prepareexecbase.c`)
parses `sysdebug=` out of it into `SysBase->ex_DebugFlags`. So AROS's runtime
debug flags work here with no rebuild:

```sh
qemu-system-aarch64 -M raspi3b \
  -kernel /path/to/Emu68.raw.img \
  -dtb /path/to/bcm2710-rpi-3-b.dtb \
  -initrd bin/emu68-m68k/AROS/aros-emu68-m68k.elf \
  -append "sysdebug=InitCode" \
  -serial stdio -no-reboot
```

`InitCode` is the most useful one during bring-up. It turns on two separate
printouts:

- `rom/kernel/prepareexecbase.c` dumps the whole resident list once it is
  built, as `addr: pri flags version name`. The flags column is the init
  class - `01` `RTF_COLDSTART`, `02` `RTF_SINGLETASK`, `04` `RTF_AFTERDOS`,
  `80` `RTF_AUTOINIT` (hence the `81` on most modules).
- `rom/exec/initcode.c` narrates each pass: `enter InitCode(0x01, 0)`, one
  `calling InitResident (pri flags "name")` per module, then `leave`.

```text
Resident modules (addr: pri flags version name):
+ 34605b20:  127 02   4 "kernel.resource"
+ 3460f53c:  120 01  51 "exec.library"
+ 3460fefc:   50 81  41 "timer.device"
+ 34629cda:    9 81  45 "emu68gfx.hidd"
+ 3468044e:  -50 01  41 "dosboot.resource"
+ 3467dcf4: -120 00  50 "dos.library"
+ 34692160: -121 04  41 "DOSBoot cleanup"
```

Flags are comma-separated (`sysdebug=InitCode,InitResident`); the full list
of names is `ExecFlagNames` in `rom/exec/exec_flags.c`. Note that the
`Resident modules` dump comes from the kernel and the `InitCode:` lines come
from exec, so seeing one without the other means the flag was applied later
than the printout you are missing.

To pinpoint a module that hangs during init rather than just knowing which
ones ran, put `#define DEBUG 1` above `#include <aros/debug.h>` in that
module's source and rebuild - its `D(bug(...))` calls reach the same serial
channel. That is how `dosboot.resource` was confirmed to be sitting in its
retry loop rather than stuck.

> **Instrumentation currently left switched on -- revert before this branch
> is finished.** Seven files outside this target carry a `#define DEBUG 1`
> that does not belong there:
>
> - `rom/dos/boot.c`, `cliinit.c`, `shell_helper.c`, `systemtaglist.c` --
>   the mount, the `SYS:` assign, the `LoadSeg()` off the card, the Shell
>   startup.
> - `rom/dos/newcliproc.c` -- the CliInit packet and the flags it computes.
> - `rom/lddemon/lddemon.c` -- every disk library load and the result of its
>   `InitResident()`. This is the only place that separates "file not found"
>   from "loaded, and `InitResident()` returned NULL", which is what found
>   the missing `task.resource`.
> - `workbench/c/Shell/Shell.c` -- each Startup-Sequence line and its return
>   code.
>
> They went in as two commits of their own, both titled `TEMPORARY`, so
> `git revert` of those two is the whole cleanup -- no hand editing.
> `arch/m68k-emu68/hidd/emu68gfx/emu68gfx_init.c` also has `DEBUG 1`, but
> that one is ours and just needs the flag flipped back.

Read memory as **bytes** (`xp /4bx`), not words. The word view renders the
byte order in a way that is easy to misread on a little-endian peripheral,
which is exactly how the byte-order bug above stayed hidden for a while.

Useful probes:

```text
xp /4bx 0x400        bootstrap stage marker (ASCII, e.g. "E005")
xp /4bx 0x3F003000   SYSTIMER CS    (compare-match status)
xp /4bx 0x3F003004   SYSTIMER CLO   (free-running microsecond counter)
xp /4bx 0x3F003018   SYSTIMER C3    (compare target)
xp /4bx 0x3F00B210   GPUIRQ_ENBL0   (bit 3 == System Timer channel 3)
xp /4bx 0x3F00B204   GPUIRQ_PEND0
```

Note these are **host physical** addresses. The guest sees the same registers
through Emu68's peripheral window at `0xf2000000` (so `systimer_base` reads
`0xf2003000` inside AROS). Symbols from the m68k ELF cannot be poked this way
- the kernel is relocated at load, so `nm` addresses are link-time only.

Use `screendump /tmp/aros.ppm` in the monitor to capture the framebuffer
console.

### Current status

Built for `raspi64`. Everything below runs against clean upstream Emu68
(`michalsc/Emu68`) **except interrupt delivery**, which needs the one Emu68
change described under "The fix" - stock upstream has no path for a host IRQ
to reach the m68k at all.

Working:

- FDT discovery and `/soc` address translation - both nodes resolve to the
  real guest-accessible window.
- System Timer programming. With the byte-order fix, `CLO` reads as a sane
  microsecond counter (advancing ~2.7 ms between adjacent traces instead of
  jumping by billions), `C3` is programmed to `CLO + interval`, and the
  compare **matches**.
- Interrupt controller. `GPUIRQ_ENBL0` bit 3 is unmasked and, once the
  compare latches, `GPUIRQ_PEND0` bit 3 reads pending.

Interrupt delivery: **working**, over ordinary Amiga custom-chip writes. This
needs one change on the Emu68 side - see below.

#### What was missing

Emu68's core-0 IRQ fast path (`vectors.c:148`-`171`) does two things on every
physical IRQ: it sets its internal `ARMPending` flag **unconditionally**, and
it sets `INTF.ARM = 6` - which is what makes the execution loop raise m68k
level 6 - **only** when `(INT_shadow.INTENA & 0x6000) == 0x6000`, i.e. when
the shadow has both `INTEN` and `EXTER`.

Reading Emu68's own `INT_shadow` out of physical memory while the timer is
running shows exactly that state:

```text
0x34c620b0:  00 00   00 00   01
             INTENA  INTREQ  ARMPending
```

That was the state before arming: `ARMPending = 1` proves the whole hardware
path works - the compare matched, the interrupt controller routed to core 0,
ARM IRQs are unmasked, and the fast path ran. It skipped the `INTF.ARM` store
only because `INTENA` was zero.

So the single missing step is arming that shadow. Nothing else is broken.

#### Why stock Emu68 cannot arm it

Arming the shadow means writing `INTENA`, and on a standalone build there is
no way to do that from the guest:

| Fact | Location |
|---|---|
| All `INTENA`/`INTREQ` MMIO emulation sits inside `#ifdef PISTORM_ANY_MODEL` | `vectors.c:314`-`723` |
| A standalone build is `VARIANT=none`, so that macro is undefined | `CMakeLists.txt:245` |
| The non-PiStorm `SYSWriteValToAddr` special-cases only `0xdeadbeef`; everything else writes through to a linear alias | `vectors.c:726` |
| Every writer of `INT_shadow.INTENA` is inside that ifdef | `vectors.c:377,380,660,663,666` |
| `INTF.IPL` is dead - `GetIPLLevel()` returns 0 | `ExecutionLoop.c:296` |
| The whole `MOVEC` write surface can only *clear* `INTF.ARM`, never set it | - |

So writing `0xE000` to `0xdff09a` on a stock build lands in ordinary RAM and
reads back unchanged. This also answers the regression question for any fix:
nothing can break, because nothing works today.

#### The fix: emulate the two registers on non-PiStorm builds

The Pi has no Paula, but the *protocol* Emu68 already implements for PiStorm
is exactly the one needed - a level-6 "EXTER" channel with an arm bit and an
acknowledge. Rather than invent a second mechanism, the non-PiStorm branch
gains the same four register cases:

| Emu68 change | File |
|---|---|
| Map a faulting page at `0x00dff000` so accesses trap | `src/aarch64/start.c` |
| `INTENA`/`INTREQ` writes update `INT_shadow` and gate `INTF.ARM` | `src/aarch64/vectors.c` |
| `INTENAR`/`INTREQR` reads are *served from* the shadow | `src/aarch64/vectors.c` |

Reads are served rather than snooped, so `INTENAR` reflects what Emu68 is
actually holding - which is what makes the arming verifiable from the guest.

This lives on `feature/host-irq-abi` in `github.com/JJDSNT/Emu68`
(commit `4218d21`, on top of upstream `9b4379a5c5`). **Stock upstream
delivers no interrupts to this port** until that change is merged;
`doc/host-interrupts.md` in this directory is the write-up for upstream,
including the alternative (a `MOVEC` control register) and why this option
was preferred.

#### Guest side

Nothing Emu68-specific. `platform/platform.c` writes the two custom-chip
registers exactly as `arch/m68k-amiga/kernel/amiga_irq.c` does against real
Paula:

| Step | Mechanism |
|---|---|
| Arm | `*0xdff09a = INTF_SETCLR \| INTF_INTEN \| INTF_EXTER` |
| Deliver | fast path sets `INTF.ARM` -> loop raises level 6 -> autovector at `VBR + 0x78` |
| Acknowledge | `*0xdff09c = INTF_EXTER` (SET/CLR clear), once per entry |

Arming happens *before* device discovery: Emu68's fast path masks ARM IRQs on
return and nothing re-enables them, so an IRQ arriving while the shadow is
still clear records `ARMPending`, skips `INTF.ARM`, and leaves the CPU deaf
for good.

The acknowledge is mandatory on every entry and is not optional politeness:
`INTF.ARM` is **not** cleared when the exception is taken
(`ExecutionLoop.c:387`-`436` pushes the frame and loads PC without touching
`INTF`), so the line is level-sensitive. Acknowledge after dispatch has
drained every source, the same ordering `arch/m68k-amiga` uses for a server
chain.

#### Verified

`boot/selftest.c` (a ROMTag at `rt_Pri -49`, `EMU68_SELFTEST`, off by
default) exercises the whole path from above:

```text
[AROS/Emu68] platform timer: tick 1 / 2 / 3
[AROS/Emu68] timer.device clock is advancing
[AROS/Emu68] timer.device woke the task
[AROS/Emu68] timer.device elapsed time is coherent
[AROS/Emu68] simultaneous timer requests completed in order
[AROS/Emu68] AbortIO cancelled timer request
[AROS/Emu68] timer/scheduler soak completed successfully
```

The soak is the load-bearing part: 120 consecutive one-second requests, each
checking that both spinning workers advanced. A lost interrupt hangs it, a
broken acknowledge storms it, and a scheduler that does not preempt starves a
worker. It runs 120/120.

Corroborated independently by `dosboot.resource`'s own retry loop, whose
`bootDelay()` goes through `timer.device`: 19 retries in 57 seconds, 3.00 s
each, which is exactly the 150 ticks it asks for.

One thing earlier revisions of this document got wrong: **there is no byte
swap on Emu68's own structures.** Emu68 is built `elf64-bigaarch64` - the ARM
runs big-endian, sharing the guest's byte order. Swapping is needed only for
the genuinely little-endian BCM peripherals.

Real Raspberry Pi 3 hardware validation remains outstanding.

#### To revisit: `INTF.IPL` instead of the shadow

What we have works, but it is an Amiga-shaped channel bolted onto a machine
with no Amiga chipset. Every host interrupt, whatever it is, arrives as level 6
because EXTER is the only door, and getting through that door means Emu68
maintaining an `INT_shadow` and the guest programming `INTENA`/`INTREQ` to
open it. PiStorm needs that indirection because it is snooping a real Paula on
a real bus. We are inventing a Paula in order to talk to ourselves.

There is a more direct path already in the CPU model. `struct M68KState` has
an `INTF` union (`Emu68 include/M68k.h:168`) whose `IPL` field is the m68k
interrupt priority level, and the execution loop already consumes it
(`src/ExecutionLoop.c:341`):

```c
if (ctx->INTF.ARM)      level = 6;      /* our door */
else if (ctx->INTF.PPC) level = 2;

#if defined(PISTORM)                    /* PiStorm32 */
    if (ctx->INTF.IPL > level) level = ctx->INTF.IPL;
#else                                   /* PiStorm classic */
    if (ctx->INTF.IPL) { ... GetIPLLevel(); ... }
#endif
```

The PiStorm32 branch is the shape worth copying: one comparison, and the level
comes straight from a field the IRQ fast path could set. A host interrupt would
then arrive at whatever level suits it rather than being funnelled into EXTER,
and neither Emu68 nor the guest would need `INT_shadow` for it at all.

Two things this note is careful about, because both were got wrong along the
way. First, the expensive `GetIPLLevel()` GPIO read belongs to *classic*
PiStorm, which has to sample real IPL pins; it is not the cost of using
`INTF.IPL`, and an earlier reading of this code wrongly concluded that driving
IPL would burden the JIT's hot loop. Second, the consumer is compiled out for
a standalone build -- neither branch above is reachable without
`PISTORM`/`PISTORM_CLASSIC` -- so this is not a free switch: it needs a
non-PiStorm branch of its own, in the same spirit as the interrupt-register
work already done. (This once also cited the autoconfig work; that has since
been removed -- see AI_context/issues/ISSUE-0016.md.)

`doc/host-interrupts.md` records the two options that were on the table when
the EXTER route was chosen. This is a third, and it is the one that stops
pretending there is a Paula.

### Superseded diagnosis

An earlier revision of this document attributed the dead timer to a gap in
QEMU's emulation of the legacy BCM2835 system timer, on the grounds that the
compare-match status bit never set while `CLO` visibly ran past the target.
That was wrong, and is recorded here so the reasoning is not repeated: the
driver was writing byte-reversed values, so the target the hardware actually
held was ~512 seconds in the future while `CLO` was still in the hundreds of
milliseconds. QEMU emulates this peripheral correctly. The same bug would
have failed identically on real hardware.

## Adding a ROMTag: `.aros.romtag` in `boot/emu68.ld`

A port-local module can join the normal AROS startup by dropping a
`struct Resident` into the image - `krnRomTagScanner()` sweeps
`__aros_resident_start` .. `__aros_resident_end` looking for
`RTC_MATCHWORD`, so no `.conf` file or genmodule wrapper is needed. But on
this target the tag **must** go in the `.aros.romtag` section:

```c
static const struct Resident my_romtag
    __attribute__((section(".aros.romtag"), used)) =
{
    RTC_MATCHWORD, (struct Resident *)&my_romtag, (APTR)(&my_romtag + 1),
    RTF_COLDSTART, 41, NT_UNKNOWN, -49,
    "myname", "my id string\r\n", (APTR)my_init
};
```

The reason is a trap. The scanner is a linear sweep, and every time it finds
a ROMTag it jumps straight to that tag's `rt_EndSkip` - which is a symbol
belonging to the module, intended to skip past that module's own body. But
`boot/emu68.ld` gathers **all** `.text` from every object before **all**
`.rodata`, so a module whose `rt_EndSkip` resolves into the `.rodata` block
ends up claiming everything the linker happened to place in between.
`dosboot.resource` is the worst case: its `rt_EndSkip` points at
`db_Cleanup` (the `addromtag db_Cleanup` in `rom/dosboot/dosboot.conf`),
roughly 70 KB further on. A ROMTag landing inside that span is skipped in
complete silence - no warning, no error, the module simply never
initializes.

`.aros.romtag` is placed immediately after `.text.boot` and ahead of every
module's `.text`, which is the one position that cannot be inside another
tag's skip range. Verify placement with:

```sh
m68k-aros-nm -n bin/emu68-m68k/AROS/aros-emu68-m68k.elf | grep -i romtag
```

Your tag should appear near address 0, before `Timer_ROMTag`.

`rt_Pri` chooses when it runs within its init class - the list is sorted
descending. `timer.device` is 50 and `dosboot.resource` is -50, so `-49` is
the last slot before dosboot, with the entire system already up. That is
where `boot/selftest.c` registers itself (`EMU68_SELFTEST`, off by default).

That -50 boundary matters more than it looks: `InitCode(RTF_COLDSTART, 0)`
never returns. `dosboot_Init()` either hands over to dos.library or loops
forever retrying for boot media, which is why `arch/aarch64-native` treats a
return from it as fatal (`krnPanic("System Boot Failed!")`). Anything that
needs to run at COLDSTART has to be a resident with `rt_Pri > -50`; code
placed after the `InitCode()` call in `coldstart_user()` is unreachable.

## Debug console (0xdeadbeef)

Emu68 leaves the guest physical address `0xdeadbeef` deliberately unmapped
(`src/aarch64/start.c`). A one-byte guest write there faults into Emu68 itself,
which forwards the byte to its own host-side `kprintf()`
(`src/aarch64/vectors.c`), reaching the real UART that QEMU exposes through
`-serial`. This gives `bug()`/`kprintf()` real scrollback text output,
independent of framebuffer state, instead of the single-value bootstrap marker
below.

`krnPutC()` (`arch/m68k-emu68/kernel/kernel_debug.c`) and the framebuffer
console's `emu68_console_putc()` (`arch/m68k-emu68/boot/console.c`) both write
through this channel now, so existing boot-progress messages show up on serial
as-is.

TODO: now that this channel exists, revisit whether the `emu68_set_stage()`
single-marker mechanism below is still needed, or whether boot-stage tracking
should move entirely to text messages over this channel.
