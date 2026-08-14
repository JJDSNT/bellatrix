---
id: ISSUE-0019
title: "Port the BCM2708 USB OTG host controller and Poseidon from arm-native"
status: doing
priority: high
type: feature
owner: unassigned
created_at: 2026-08-14
updated_at: 2026-08-14
tags:
  - usb
  - dma
  - cache
  - emu68
  - poseidon
blockers:
related_files:
  - external/aros/arch/arm-native/soc/broadcom/2708/usb/usb2otg/
  - external/aros/arch/arm-native/soc/broadcom/2708/usb/poseidon/usbromstartup.c
  - external/aros/rom/usb/poseidon/
  - external/aros/arch/m68k-all/exec/cachecleare.c
  - patches/aros/0002-m68k-all-support-an-m68k-that-is-not-an-amiga.patch
  - aros/arch/m68k-emu68/soc/
  - aros/arch/m68k-emu68/kernel/getsystemattr.c
  - aros/arch/m68k-emu68/platform/bcm283x/interrupt_controller.c
---

# Summary

Give this port USB, by building `arch/arm-native`'s `usb2otg.device` — the
Synopsys DesignWare USB 2.0 OTG host controller driver, the same silicon the
Pi 3B has — for `m68k-emu68`, and putting the Poseidon stack behind it.

The port itself is smaller than it looks. The driver is ~9200 lines but it is
**already written for a big-endian host** and already uses the two kernel
interfaces this port implements. The work is almost entirely in one place that
has nothing to do with USB: **this port cannot do DMA correctly yet**, and USB
would be its first consumer.

# What is already in our favour

Checked in the tree on 2026-08-14, not assumed:

| What the driver needs | State on this port |
|---|---|
| Little-endian MMIO from a big-endian CPU | **Already handled.** `usb2otg_intern.h:67-93` defines `rd32le`/`wr32le` over `AROS_LE2LONG`/`AROS_LONG2LE`, because arm-native supports armeb. Same idiom our `soc/mbox` uses. |
| An interrupt | **Already works.** `KrnAddIRQHandler(IRQ_VC_USB, ...)`, and `IRQ_VC_USB` is GPU IRQ 9 — bank 0, bit 9. Our controller dispatches by `irq >> 5` and already carries GPU IRQ 62 for the SD card. |
| The peripheral window | **Already implemented.** The driver reads `ARM_PERIIOBASE`, which comes from `KrnGetSystemAttr(KATTR_PeripheralBase)`; `kernel/getsystemattr.c` answers it from `platform_periiobase`, taken from `/soc`'s first `ranges` entry. |
| `openfirmware.resource` | **Optional.** `DTEnabled()` returns TRUE when `OpenResource` gives NULL, and probes the core as before. Only BCM2711, where the OTG core is disabled in the DT, needs it. |
| Emulation to test against | **QEMU has it.** `qom-list /machine/soc/peripherals` on `raspi3b` under QEMU 8.2.2 lists a `dwc2` object (and `mphi`). Unlike ISSUE-0012, this is not gated on real hardware. |

The driver has also clearly been hardened on a real Pi rather than only written:
bounce buffers with their own 64-byte-aligned allocation because `AllocPooled`
ignores member alignment, split-transaction channels that get retired when they
cannot be revived, an IVAC tripwire mentioned by name in a comment. That is a
good thing to inherit and a reason to port it rather than write one.

# The blocker: `CacheClearE()` does nothing to the data cache here

This is the whole of the difficulty and it is **our own defect**, not upstream's.

`patches/aros/0002` gives `arch/m68k-all/exec/cachecleare.c` an `__EMU68__`
early return:

```c
#ifdef __EMU68__
    /*
     * Emu68 recognizes the 68040 CINVA IC opcode and uses it to invalidate
     * translated m68k code. There is no physical m68k data cache to manage,
     * so invalidate the complete translated instruction cache even when the
     * caller supplied a smaller range.
     */
    __asm__ volatile(".word 0xf498" ::: "memory");
    return;
#endif
```

`0xf498` decodes as `CINVA IC`: `0xf498 & 0xff20 == 0xf400` selects CINV,
`& 0x18 == 0x18` is scope "all", and `& 0x40 == 0` means **instruction cache
only**. The comment is accurate about what it does. The sentence *"There is no
physical m68k data cache to manage"* is the part that is wrong — there is no
m68k data cache, but there is a very real **ARM** data cache, and every guest
load and store goes through it because the JIT emits ordinary AArch64 loads and
stores.

The USB driver's correctness rests entirely on that primitive:

```
usb2otg_schedule.c:464   CacheClearE(buffer, xfer_size, CACRF_ClearD);
usb2otg_schedule.c:749   CacheClearE(buffer, xfer_size, CACRF_InvalidateD);
usb2otg_schedule.c:892   CacheClearE(completed_buf, inval_len, CACRF_InvalidateD);
```

Clean before the controller reads a buffer; invalidate before the CPU reads what
the controller wrote. On this port both are no-ops, so the DWC2 engine would
read stale lines out of DRAM and the CPU would read stale lines out of its own
cache. The failure mode is silent data corruption on transfers, which is the
worst kind to debug and the kind this project has just spent ten days on.

**Nothing has caught this because nothing on this port does DMA.** The SD card
driver is PIO through the Arasan FIFO — MMIO, uncached, no coherency question.
That is also why ISSUE-0013 (a DMA SD path) is a separate future item. USB is
the first real DMA consumer, and it walks straight into it.

## The fix exists in Emu68 already

Emu68 translates the 68040 cache instructions into genuine AArch64 maintenance
(`external/emu68/src/M68k_LINEF.c`, the `CINV` and `CPUSH` branches):

| m68k | Emu68 emits |
|---|---|
| scope line (`& 0x18 == 0x08`) | `dsb sy`, `bic` to the line boundary, `dc civac`, `dsb sy` |
| scope page (`== 0x10`) | the same `dc civac` in a loop across 4 KB |
| scope all (`== 0x18`) | a call to `clear_entire_dcache`, the full CLIDR/CSSELR set/way walk |

and `opcode & 0x40` is what selects the data cache. So the encodings this port
needs already work:

| Wanted | m68k instruction | Opcode |
|---|---|---|
| clean+invalidate one line | `CPUSHL DC,(An)` | `0xf468 + An` |
| clean+invalidate a page | `CPUSHP DC,(An)` | `0xf470 + An` |
| clean+invalidate everything | `CPUSHA DC` | `0xf478` |
| invalidate one line | `CINVL DC,(An)` | `0xf448 + An` |

Neither branch checks privilege, so this does not have to run in supervisor
mode.

So the prerequisite is not "teach Emu68 cache maintenance". It is "stop our own
patch from throwing the range away", which is a rewrite of one function.

# Plan

Each step ends somewhere that builds and boots.

1. **Fix `CacheClearE` for Emu68 first, on its own, before any USB code.**
   Honour `CACRF_ClearD`/`CACRF_InvalidateD` by walking the range with
   `CPUSHL DC,(An)`, keep the existing `CINVA IC` for `CACRF_ClearI`, and fall
   back to the "all" scope above some range where a line loop stops being worth
   it. `CachePreDMA`/`CachePostDMA` want the same treatment; on this port their
   address translation is the identity, since the guest's addresses are physical.

   **This is worth doing whether or not USB happens**, and it should be verified
   on its own terms rather than through USB — a wrong cache primitive and a
   wrong USB driver produce the same symptom, and debugging both at once is how
   the last investigation went.

2. **Verify the bus-address assumption.** The driver programs DMA addresses as
   `0xc0000000 | (ULONG)buffer` (`usb2otg_schedule.c:754`), the VideoCore alias
   of ARM physical memory. That is right if and only if a guest address is an
   ARM physical address. Emu68 maps the advertised DRAM 1:1 and our heap runs
   `0x02000000-0x345fffff`, comfortably inside the alias window — but this is
   the assumption that quietly breaks everything if it is wrong, so measure it
   rather than reason about it. Note we do *not* inherit arm-native's
   complication here: it has kernel static arrays at `0xf8xxxxxx` virtual that
   the alias cannot reach, and this port has no VA/PA split for the guest.

3. **Give m68k the CPU abstraction the driver needs.** Checked on 2026-08-14
   and it is the one real portability gap -- see "How ARM-specific the driver
   actually is" below. `arch/m68k-all` has no `include/asm/` at all, and the
   driver's MMIO accessors call `dmb()` on every register access.

4. **Build `usb2otg.device` for the target.** A new `soc/usb/usb2otg/` under
   `aros/arch/m68k-emu68/`, mirroring the arm-native mmakefile
   (`modtype=device`, `moduledir=Devs/USBHardware`). The 9200 lines of driver
   source are **not** copied: `config/make.cfg.in:346` sets `VPATH` to the
   source directory and `rule_compile_multi` adds `vpath %.c` of its own, so the
   mmakefile can name the arm-native directory and compile in place. One copy of
   the source, no divergence from upstream.

5. **Bring in Poseidon and the romstartup.** `rom/usb/poseidon` plus the
   classes actually wanted (hid, massstorage) and a `usbromstartup` equivalent
   to `arch/arm-native/soc/broadcom/2708/usb/poseidon/`.

5. **Test in QEMU with `-device usb-kbd` and `-device usb-storage`**, then on
   real hardware, and expect the two to disagree — see the risks.

# Risks, in the order they are likely to bite

**The full build is on the path, and it was broken — now ISSUE-0020.** `usb2otg`
lands in `Devs/USBHardware` on the SD card, not in the kernel ELF, so this needs
`build-aros.sh full`, and that failed in freetype. Reproduced at the current pin
on 2026-08-14 and diagnosed: freetype 2.14.3's sources were being compiled
against **2.14.1's headers**, still installed in the sysroot from the August 3
build, because the 2.14.3 tarball's headers carry March mtimes and `%copy_includes`
is a plain make prerequisite. Older-but-newer-version loses to newer-but-older-file.

The guess recorded here first — "an AROS-against-freetype-2.14.3 mismatch",
i.e. somebody else's source bug — was wrong, and cheaply so: the compiler's
`note: declared here` named a path under `AROS/Developer/include` rather than
under `Ports/`, which settled it. Worth remembering as a habit rather than as a
fact about freetype.

Unblocked by deleting the stale installed headers. See ISSUE-0020 for the
durable fix, which is not yet done.

**QEMU will not exercise most of the driver.** QEMU attaches USB devices
directly to the dwc2 root port. A real Pi 3B has a LAN9514 hub soldered between
the OTG core and every socket, so on hardware every transfer is a split
transaction through a hub. That is what `usb2otg_hub.c` and the split-channel
logic in `usb2otg_schedule.c` are for, and QEMU will exercise essentially none
of it. **A pass in QEMU is evidence the driver initialises and talks, not that
it works.** Do not let a green QEMU run stand in for a Pi.

**QEMU's dwc2 model is partial.** It is enough for a keyboard and mass storage
and is not a reference implementation. A disagreement between QEMU and hardware
is as likely to be QEMU's as ours, and neither should be assumed.

**Cache maintenance is easy to get subtly right.** A range walk that is off by a
line, or that rounds the wrong way, produces corruption that is rare and
load-dependent — indistinguishable at first from a driver bug. This is the
argument for step 1 standing alone with its own test.

# Acceptance criteria

- [ ] `CacheClearE`/`CachePreDMA`/`CachePostDMA` honour their range and the
      D-cache flags on Emu68, verified independently of USB.
- [ ] A guest address is demonstrated equal to its ARM physical address over the
      heap range, rather than assumed.
- [ ] `build-aros.sh full` completes, or its failure is a filed issue of its own.
- [ ] `usb2otg.device` builds for `m68k-emu68` and initialises, with the driver
      source unmodified or with every modification justified in a patch.
- [ ] A USB keyboard enumerates under QEMU.
- [ ] A USB keyboard enumerates on a real Pi 3B, through the LAN9514 hub.
- [ ] Mass storage reads a file.
- [ ] The boot-to-icons rate is unchanged with USB present — measured, against
      the twelve-run baseline in
      `AI_context/consolidated/history/ISSUE-0017.md`.

# Notes

The last criterion is not a formality. This adds an interrupt source, a second
DMA master and a large body of new code to a boot that has only just become
reliable, and the honest expectation is that it will cost something. Better to
find out by measuring than by noticing.

`arch/m68k-amiga` also has USB (`usb/denebusb`), which is evidence Poseidon
builds for m68k at all — a different controller, but the stack above it is the
same one.

# Execution log

## 2026-08-14 — steps 1 to 4 done; the driver builds for m68k

**Step 1, `CacheClearE`** — done (`90020ee`). One function was enough: on 68040
`CachePreDMA_40` and `CachePostDMA_40` both end in `jsr -0x282(%a6)`, which is
this vector, and that also settled the semantics -- both pass `CACRF_ClearD` for
the *post*-DMA direction, so clean-only would have been wrong there.

The implementation changed once during the work, for a reason worth keeping.
The first version stepped the range in 16-byte lines with `CPUSHL`. Emu68 ends
the translation block after every cache instruction -- *"Cache is context
synchronizing. Break up here!"* -- so **under a JIT the unit of cost is the
instruction, not the cache line**, and a line loop costs one translation break
per 16 bytes. `CPUSHP` is one instruction, one break, and Emu68 loops the page
internally with the host's real line size, which also sidesteps the guest being
unable to read `CTR_EL0`. A typical USB transfer went from 32 instructions to 1.

**Step 2, the bus-address assumption** — done, and it needed no new code. Two
independent proofs:

- *Static.* `external/emu68/src/aarch64/start.c:1365` is
  `mmu_map(mb_Base, mb_Base, size, MMU_ACCESS | MMU_ISHARE | MMU_ATTR_CACHED, 0)`
  and the signature is `mmu_map(phys, virt, ...)`, so the guest's DRAM is
  identity-mapped. `MMU_ATTR_CACHED` is also why step 1 matters.
- *Runtime, on every boot.* `soc/sdcard/sdcard_bcm2708init.c:75` allocates its
  mailbox buffer with `AllocMem` -- heap, above `0x02000000` -- and hands the
  pointer to the VideoCore, which DMAs at a physical address. Line 99 checks the
  address comes back echoed and line 120 reads a result the VideoCore wrote into
  that buffer. The card only powers on because this works, and we boot from it.

**Steps 3 and 4, the CPU layer and the build** — done (`407e787`). The driver
compiles to `usb2otg.device`, ELF 32-bit MSB Motorola m68k, 62 KB, in
`Devs/USBHardware`. No source is copied: `rule_compile_multi` resolves absolute
basenames against their own directory and adds a `vpath`, so the mmakefile names
the arm-native files where they live.

Four gaps, all found by building rather than by reading: `<asm/cpu.h>` (added
for this target), `cpumask_t` (patch 0012 -- m68k was the only architecture not
defining it), `VCPOWER_*` (moved from soc/sdcard's private header to
`<hardware/videocore.h>`), and the 27 raw ARM instructions (patch 0013).

Also established: **Emu68 does no VideoCore power management.** Its only mailbox
tags are `0x00010005`/`0x00010006` (memory), `0x00030002`/`4`/`7` (clock
queries), `0x00038002`/`0x00038030` (clock set) and `0x00040003` (framebuffer).
Powering the OTG core up is the guest's job, exactly as it already is for the SD
card.

## What is next

Step 5, Poseidon. Nothing loads `usb2otg.device` yet -- it is a file on the card
with no stack above it -- so the next thing that can be *observed* is
`rom/usb/poseidon` plus a `usbromstartup` equivalent. Building is not the same
as running, and none of the above has been run.
