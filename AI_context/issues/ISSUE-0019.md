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
  - aros/arch/m68k-native/soc/
  - aros/arch/m68k-native/kernel/getsystemattr.c
  - aros/arch/m68k-native/platform/bcm283x/interrupt_controller.c
  - aros/arch/m68k-emu68/boot/mmakefile.src
  - run.sh
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

Unblocked by deleting the stale installed headers. ISSUE-0020 diagnosed that and
is closed (`consolidated/history/`); the durable fix, which is not yet done, is
ISSUE-0025.

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

## Runtime investigation — 2026-08-14

The earlier statement that nothing loads `usb2otg.device` is obsolete. Patch
0014 starts `PsdStackLoader` and calls `AddUSBHardware usb2otg.device` from
`Startup-Sequence`; the full distribution and SD image contain all of:

- `Devs/USBHardware/usb2otg.device`;
- `Libs/poseidon.library`;
- `Classes/USB/hid.class` and `bootmouse.class`;
- `C:PsdStackLoader`, `C:AddUSBHardware`, and `C:AddUSBClasses`.

QEMU's `raspi3b` machine already owns a DWC2 controller. Attaching
`-device usb-mouse` succeeds at the QEMU level but does not make the AROS
cursor move. A diagnostic `Startup-Sequence` captured the actual guest result:

```text
starting AddUSBHardware
Adding hardware usb2otg.device, unit 0...failed!
finished AddUSBHardware
```

No `[USB2OTG] HS OTG Core Release` or driver-initialised message reached the
serial log. The failure is therefore below Poseidon's hardware registration:
`psdAddHardware()` cannot open/initialise the device. The module itself is a
valid ELF32 big-endian m68k relocatable, contains its ROMTag and Init entry, and
has no undefined symbols.

## ARM-native versus Emu68 wiring

The low-level Bellatrix wiring already follows the native ARM path:

- Emu68 maps the Raspberry Pi `/soc` peripheral ranges below 4 GiB and rewrites
  the FDT `ranges` values before passing the copied tree to the m68k ELF in A6.
- The m68k bootstrap publishes that FDT as `KRN_OpenFirmwareTree`.
- `platform.c` derives `platform_periiobase` from the rewritten `/soc/ranges`;
  `KrnGetSystemAttr(KATTR_PeripheralBase)` exposes it to disk modules. The SD
  and mailbox drivers already operate successfully through this path.
- The m68k BCM283x interrupt-controller driver uses the native logical IRQ
  numbering and makes `KrnAddIRQHandler(IRQ_VC_USB, ...)` unmask GPU IRQ 9.
- Emu68 has no competing USB stack or DWC2 owner. Powering the block remains
  the guest driver's responsibility through `mbox.resource`.

The important architectural difference is startup. ARM-native packages the
DWC2 driver and runs a Raspberry Pi `usbromstartup` resident during
`COLDSTART`; that resident opens Poseidon, registers the controller and begins
enumeration while the required kernel resources are resident. Bellatrix keeps
the driver on disk and attempts registration much later from
`Startup-Sequence`.

### Emu68 itself is not a USB-driver reference

A source-tree search on 2026-08-14 found no DWC2, OTG, xHCI, LAN9514 or generic
USB controller implementation in Emu68's runtime sources, headers, examples or
board code. The only relevant textual occurrence is the CM4 preparation guide,
which describes Raspberry Pi firmware `usbboot` exposing eMMC to the host; it
is not USB support supplied to the m68k guest. Matches under bundled Capstone
are instruction names such as `PADDUSB`, not USB code.

Consequently Emu68 is the platform-enablement reference for address mappings,
the rewritten FDT and translated cache-maintenance instructions, but not for
DWC2 sequencing or a guest USB ABI. For those, the correct implementation
reference remains AROS `arm-native`/BCM2708, adapted only at the CPU/platform
boundary and registered through the normal modular Poseidon path.

### The Emu68 xHCI driver is a valuable integration reference

The separate [`rondoval/emu68-xhci-driver`](https://github.com/rondoval/emu68-xhci-driver)
project is not part of the Emu68 core tree and targets the BCM2711/VL805 xHCI
controllers on Pi 4/CM4, not the BCM2837 DWC2 on Pi 3. It therefore cannot
supply our controller reset/register sequence. It is nevertheless strong
evidence for the surrounding architecture because it is a real classic
Poseidon HCD designed specifically for Emu68.

Transferable patterns found in its source on 2026-08-14:

- It is a normal loadable `xhci.device`, installed under
  `DEVS:USBHardware`, rather than a USB stack embedded in Emu68. This supports
  Bellatrix's modular `usb2otg.device`/installation-owned Poseidon boundary.
- Its onboard unit discovers MMIO and IRQ from `devicetree.resource`, checks
  `status`, and obtains a guest-visible virtual base instead of hard-coding the
  Pi model. The API differs from AROS's `openfirmware.resource`, but the role is
  exactly the one intended for our resident OpenFirmware service.
- It wraps transfer buffers with `CachePreDMA()`/`CachePostDMA()`, maintains a
  pool limited to Emu68/Pi DRAM, tests DMA reachability, and uses aligned bounce
  buffers for unreachable or unsafe inbound buffers. This independently
  validates treating correct cache/DMA semantics as a platform prerequisite,
  not as a DWC2-specific workaround.
- It permits direct inbound DMA only for whole cache-line-aligned ranges,
  because invalidating partial boundary lines could discard neighbouring CPU
  data. Bellatrix must audit the inherited DWC2 bounce/direct-buffer decisions
  against the same rule after enumeration works.
- Its interrupt server acknowledges and gates hardware, signals a dedicated
  unit task, and lets that task process event rings and timeouts. It also uses
  `timer.device` for watchdog/delay work. This supports bounded, scheduled
  waits and minimal ISR work rather than long MMIO spin loops.
- Controller attach is staged with error unwinding. A failed register mapping,
  controller registration, task start or interrupt setup returns an error and
  releases acquired state. This directly supports patch 0022's change to stop
  ignoring DWC2 reset-wait failures.

Non-transferable details:

- xHCI controller state, rings, doorbells, halt/reset protocol and PCIe/MSI
  wiring do not apply to DWC2.
- Its DMA addresses are direct Emu68 RAM addresses. BCM2837 DWC2 uses the
  VideoCore bus alias (`0xc0000000 | physical`), so its address-programming
  convention must not be copied.
- Pi 4's onboard xHCI and VL805 topology does not validate Pi 3's LAN9514 hub
  or DWC2 split transactions.

The practical use is therefore as a checklist and design precedent for the
Emu68/Exec/Poseidon boundary, while AROS arm-native remains the authority for
BCM2708 DWC2 register sequencing.

## Role of `openfirmware.resource` in the Emu68 port

Emu68 already does the hard part needed by `openfirmware.resource`: it copies
the firmware FDT, rewrites the parent addresses in `/soc/ranges` to describe
the guest-accessible peripheral mappings, and passes the resulting tree to the
m68k ELF in A6. The bootstrap preserves that pointer in the boot tags as
`KRN_OpenFirmwareTree`.

What is missing is the public AROS interface over that tree.
`openfirmware.resource` is not currently listed in the Emu68 ELF's
`CORERESIDENTS`, so later drivers cannot assume that
`OpenResource("openfirmware.resource")` succeeds. `usb2otg.device` explicitly
allows for this: when the resource is absent it performs a blind probe instead
of validating `brcm,bcm2708-usb`.

### Advantages

- **One public hardware-description API.** Drivers can use the same
  `OF_FindNodeByCompatible()` and property accessors as ARM-native instead of
  adding private FDT parsers or Pi-model conditionals to each module.
- **Safe probing.** A driver can distinguish an enabled DWC2 on BCM283x from a
  disabled or unsuitable OTG block on BCM2711, and from hardware that has no
  such controller at all, before touching MMIO.
- **Model independence.** Compatible strings, `status`, `reg`, interrupts,
  clocks, DMA ranges and board-specific properties come from the tree rather
  than being inferred from CPU architecture or fixed addresses.
- **Correct use of Emu68's rewritten view.** Drivers see the addresses Emu68
  made accessible to the m68k guest, not the original physical addresses from
  the firmware tree.
- **Parity with native ports.** Shared SoC drivers can retain their existing
  OpenFirmware-based hardware checks, reducing Bellatrix-only patches and
  making upstreaming more realistic.
- **A scalable boundary.** Future GPIO, network, PCIe, audio or other platform
  modules can discover hardware without growing `platform.c` into a registry
  of every peripheral in the machine.

### Intended responsibility

`openfirmware.resource` should be the public, read-only hardware-description
service available to normal residents and loadable drivers. It should expose
the FDT that Emu68 has already adjusted for the guest; it should not remap
hardware, own devices, or duplicate driver policy.

It does **not** replace the small parser in `platform.c`. That parser runs at
the earliest bootstrap stage, before ordinary residents are available, and is
needed to establish the interrupt controller, system timer and
`platform_periiobase`. Once Exec and resources exist, consumers should prefer
`openfirmware.resource` rather than adding more late-driver queries to the
bootstrap parser.

### Integration requirements

1. Add `openfirmware_resource` to the Emu68 ELF residents and its corresponding
   mmake dependencies.
2. Place it after the kernel/Exec foundation it requires and before normal
   disk modules can be opened.
3. Verify that it consumes the `KRN_OpenFirmwareTree` boot tag and publishes
   the same rewritten FDT that `platform.c` used.
4. Keep the DWC2 `DTEnabled()` validation active; do not replace it with an
   `__mc68000__` exception.
5. Treat absence of the resource as a platform bring-up error rather than
   silently relying on a blind MMIO probe.
6. Add a boot diagnostic that confirms the resource exists and resolves
   `brcm,bcm2708-usb` with an enabled status before Poseidon registers the
   controller.

## Decision

Use ARM-native as the reference for BCM283x wiring, but preserve the modular
AROS m68k installation model:

1. Keep fundamental platform services in the Emu68 ELF: kernel/Exec,
   `openfirmware.resource`, interrupt controller, timer, mailbox, DMA/cache
   primitives and the SD boot path.
2. Keep `usb2otg.device` as a replaceable disk module under
   `Devs/USBHardware`, analogous to an Amiga USB host-controller driver.
3. Keep `poseidon.library`, `PsdStackLoader`, USB classes and preferences owned
   by the AROS m68k installation. Do not embed a second Poseidon stack in the
   Bellatrix ELF.
4. Register the Bellatrix controller through the normal Poseidon startup path.
   Make registration idempotent so an installation with saved hardware
   preferences does not add a duplicate controller.
5. Keep HID, keyboard, mouse and mass-storage classes on disk and load/scan
   them through the standard installation mechanisms.
6. Do not make boot depend on USB. Bellatrix already boots from SD, so there is
   no need to copy ARM-native's early USB-mass-storage `COLDSTART` policy.
7. Keep the real Device Tree validation. The experimental
   `#ifdef __mc68000__` bypass is diagnostic only and must not be the final
   implementation.
8. Keep `-device usb-mouse` in graphical QEMU runs; it supplies the emulated
   device but does not replace guest-side controller/stack initialization.

This boundary is preferred both for elegance and compatibility. Whether a
module was loaded from disk or linked into the ELF has negligible steady-state
USB performance once its code is resident; DMA cache maintenance, IRQ latency,
worker scheduling and DWC2 transaction handling dominate. Keeping the generic
stack on disk avoids coupling an installation to the ELF's Poseidon version
without sacrificing meaningful runtime performance.

## Next validation

- Add and validate `openfirmware.resource` as an Emu68 resident.
- Make the modular driver reach its DWC2 core-release log when opened from
  Poseidon.
- Confirm that `psdAddHardware()` succeeds and QEMU's USB mouse enumerates.
- Confirm that `bootmouse.class` produces input events and moves the cursor.
- Repeat with `usb-kbd`, then smoke-test on a real Pi 3B because QEMU bypasses
  the board's LAN9514 hub and cannot validate split transactions.

## Current bring-up state — 2026-08-14

- `openfirmware.resource` is now linked into the Emu68 ELF and the bootstrap
  replaces the raw FDT boot-tag value with the parsed OpenFirmware root before
  `COLDSTART`. The ELF builds, links and boots with the resource resident.
- The first modular-startup defect was the device name: Poseidon was given only
  `usb2otg.device`, although the module lives in `DEVS:USBHardware`. Supplying
  `DEVS:USBHardware/usb2otg.device` reaches device Init and OpenUnit.
- QEMU reports DWC2 core release `OT2.94a`, architecture 2 (internal DMA), and
  the driver reaches its first core soft reset.
- The current stop is inside that reset sequence after DMA/global interrupts
  are disabled. A paused QEMU monitor read of physical `GRSTCTL` before guest
  initialisation returned `0x80000000` (`AHBIDLE` set). Patch 0022 records
  `GRSTCTL` immediately around `CSFTRST` and rejects a reset timeout instead of
  ignoring it.
- Builds remain incremental: apply only the new numbered patch, build
  `kernel-usb-usb2otg-emu68`, recreate the SD image, and boot it. Do not run
  `setup.sh --reset`, `build-aros.sh clean`, or an unqualified full build for
  this investigation.

### Reset and timer result

A local graphical run supplied the decisive patch-0022 trace:

```text
[USB2OTG] Init: Core soft reset, GRSTCTL=80000000
[USB2OTG] Init: CSFTRST written, GRSTCTL=80000000
[USB2OTG] Init: Core reset complete, GRSTCTL=80000000
```

This excludes a DWC2 MMIO-endianness problem at this boundary. Core identity
(`OT2.94a`), hardware configuration, `AHBIDLE`, and the self-clearing
`CSFTRST` bit all have the expected values. The next operation was the first
`usb2otg_delay(1000)`, where execution stopped.

Patch 0019 had reused `USB2OTGBase->hd_TimerReq`. That request and its reply
port are created during device Init, so the port's `mp_SigTask` belongs to the
Init task. `OpenUnit` is later called from Poseidon's `AddUSBHardware` task;
its synchronous `DoIO()` therefore waits on a reply signal owned by a different
task. This is a cross-task timer-request ownership bug, not a slow DWC2 reset.

Patch 0023 changes only the two OpenUnit settle delays. Each call creates a
task-local message port and timer request, opens `UNIT_MICROHZ`, performs the
delay, and releases them. Allocation, open, and I/O failures abort unit attach
through the existing patch-0022 failure path. This follows the task-local timer
pattern already used elsewhere in the driver and by the Emu68 xHCI reference.

The targeted build has completed and the relinked module is present in the SD
image. Note that this generated target currently needs two incremental `make
kernel-usb-usb2otg-emu68` passes after a source edit: the first rebuilt
`usb2otg_core.o`, while the second noticed that newer object and relinked
`usb2otg.device`. Always verify the module timestamp or diagnostic strings
before recreating `sd.img`.

Runtime validation of patch 0023 succeeded in the user-provided `usblog.txt`.
After `Core reset complete`, OpenUnit configured the host clock, flushed both
FIFO classes, halted all eight channels, reset the connected port and returned
successfully. Poseidon then completed control transfers without a logged error
or timeout, assigned addresses 2 and 3, read the device/configuration/string
descriptors, selected configuration 1 and interface alternate setting 1, and
read a 52-byte HID report descriptor from device 3. It finally queued:

```text
[USB2OTG] INT-Q: dev=2 ep=1 len=2 interval=255 next=386
[USB2OTG] INT-Q: dev=3 ep=1 len=4 interval=10 next=257
[AROS/Emu68] BootUI: STARTING WANDERER...
```

Device 3's HID report-descriptor request and four-byte, 10 ms interrupt IN
endpoint are consistent with QEMU's USB mouse. This establishes controller
attach, enumeration, control DMA and HID interrupt scheduling. It does not yet
establish delivery of completed interrupt reports into `bootmouse.class` or
visible cursor movement; that remains the next GUI acceptance check.

### HID partial result and performance diagnostic

GUI testing shows that mouse button presses reach AROS but relative motion does
not move the cursor. Poseidon repeatedly resubmits the four-byte interrupt pipe,
so the endpoint is alive. Button success proves at least byte zero traverses the
controller, bounce copy-back, Poseidon and `bootmouse.class`; it does not prove
that displacement bytes one and two contain the expected values.

`bootmouse.class` treats a boot-mouse report as `[buttons, signed X, signed Y,
wheel]` and writes bytes 1/2 directly to `InputEvent.ie_X/ie_Y`. Patch 0024
therefore adds one focused completion diagnostic:

```text
[USB2OTG:HID] dev=3 ep=1 actual=4 data=BB XX YY WW
```

It prints only non-zero successful interrupt reports. Movement producing
non-zero `XX`/`YY` means DWC2 DMA is correct and moves the investigation above
the HCD into `bootmouse.class`/`input.device`; zero displacement there means
the loss remains in the controller/bounce path or QEMU device selection.

The same patch restores `DEBUG=0` in `usb2otg_device.c` and
`usb2otg_core.c`. Those temporary bring-up switches logged every BeginIO and
interrupt resubmission and materially perturbed performance, so performance
must be reassessed with the focused build before attributing the slowdown to
USB IRQ or scheduling overhead. The relinked module was verified to contain
`USB2OTG:HID` and no `BeginIO: IOReq`/`INT-Q` strings, then copied into the new
SD image.

### Performance lesson from the m68k-amiga Deneb driver

The Amiga target does provide a strong performance clue, although its
controller is an ISP1760/EHCI rather than DWC2. `denebusb` deliberately leaves
the controller's SOF interrupt disabled:

```c
unit->hu_IntMask = IINTF_ATL_DONE | IINTF_INT_DONE | IINTF_ISO_DONE;
                    /* | IINTF_SOF intentionally omitted */
```

It enables only transfer-done, port-change and frame-counter-rollover events,
sets the EHCI interrupt threshold to eight microframes, and programs periodic
PTD descriptors with their own microframe/SOF-active masks. Thus hardware
enforces interrupt endpoint intervals and the CPU is interrupted for useful
completion/state events, not every one-millisecond USB frame.

This cannot be copied register-for-register: ISP1760 has a descriptor/PTD
scheduler, while BCM2837 DWC2 host channels expose only limited frame-parity
scheduling and the inherited driver currently promotes queued interrupt work
from a 1 kHz SOF ISR. The architectural lesson is directly applicable:
Bellatrix should not pay one emulated IRQ per USB frame merely to discover that
a 10 ms mouse or 255 ms hub deadline is not due.

The first low-risk scheduler reduction is patch 0028. `handle_SOF()` already
tracked whether it promoted an interrupt request, but ignored that result and
caused `PendingInt` on every 1 kHz SOF. It now wakes pending work only after an
actual promotion. Frame-sensitive split delay and stall recovery remain in the
SOF handler unchanged. This removes up to 1,000 needless scheduler/worker
wakeups per second before attempting the more invasive dynamic SOF masking.

Patch 0027 removes the temporary HCD, bootmouse and generic HID movement logs.
Those traces proved the full input path, but serial formatting on every report
would distort both the new BootUI timer and subjective redraw measurements.

The preferred DWC2 adaptation is a deadline-driven periodic scheduler. Mask
SOF while no near-term frame-sensitive work requires it; use a task-local
timer/softint to wake at the earliest interrupt-pipe deadline, then arm the
channel outside the SOF boundary as required by the existing Pi 3 erratum
comments. Retain SOF only for short windows where split completion or exact
microframe sequencing actually needs it. This follows Deneb's event-driven
policy without pretending DWC2 has Deneb's PTD hardware.

### Future I/O-core direction

Longer term, Bellatrix should be able to place USB and other I/O workers on a
dedicated core. The current driver already creates a `USB2OTG Worker` task and
assigns it an explicit CPU0 affinity, so the ownership boundary exists and a
future port-wide I/O scheduler can move it deliberately.

Do not make that migration part of the present USB bring-up or SOF-performance
fix. IRQ routing, MMIO serialization, Exec list ownership, cache/DMA ordering
and completion delivery must first be correct on CPU0. The immediate scheduler
change should keep all state and worker execution on CPU0 while separating
deadline wakeups from DWC2 SOF interrupts. Later, affinity can become platform
policy rather than being hard-coded in the device.

### Bootmouse-to-input instrumentation

Patch 0025 instruments the next boundary above Poseidon. For movement-only
reports, `bootmouse.class` now records the signed coordinates, event code,
relative-mouse qualifier and original four report bytes immediately before
calling `IND_WRITEEVENT`:

```text
[BOOTMOUSE] submit x=-1 y=0 code=00ff qual=8000 raw=00 ff 00 00
```

It also logs a non-zero result from the synchronous `DoIO()` as
`IND_WRITEEVENT failed`. Button-only and idle reports remain silent. This will
distinguish report parsing/sign-extension from rejection inside `input.device`
without restoring the high-volume HCD diagnostics.

The class target required the same two incremental make passes as the HCD:
the first rebuilt its object and the second relinked the installed
`Classes/USB/bootmouse.class`. The relinked artifact was verified to contain
both diagnostic strings and copied into `sd.img`.

The absence of that trace in the next GUI run was itself diagnostic:
`bootmouse.class` was not the active binding. `Startup-Sequence` runs
`AddUSBClasses`, which adds every class under `SYS:Classes/USB`, and Poseidon's
AfterDOS logic explicitly documents `hid.class` overruling bootmouse and
bootkeyboard. Its ROM late-startup follows the same policy: add `hid.class`
first and add the boot classes only if that fails. The report-descriptor traffic
already observed was another indication that the generic parser owned the
interface.

Patch 0026 therefore instruments the equivalent active boundary in
`hid.class::nFlushEvents()`. Whenever the generic HID parser has accumulated a
relative delta it now logs the signed X/Y, code, qualifier and input command
before `DoIO()`, plus any non-zero input.device result:

```text
[HID:MOUSE] submit x=-1 y=0 code=00ff qual=8000 cmd=10
```

The relinked `hid.class` was verified by its diagnostic strings and copied into
`sd.img`.

### QEMU pointing-device result

Patch 0026 confirmed the complete guest path. A relative report such as
`data=00 ff 00 00` became `x=-1 y=0`, `IECLASS_RAWMOUSE`,
`IEQUALIFIER_RELATIVEMOUSE`, and command 24 (`IND_WRITEEVENT`), with no error
from `input.device`. The pointer moved under the SDL frontend, proving the
end-to-end DWC2, Poseidon, HID, input.device and Intuition path.

The graphical frontend and emulated device do matter for QEMU validation:

- GTK with `usb-mouse` did not deliver sustained relative movement without
  pointer capture, although buttons and wheel reports continued;
- SDL with `usb-mouse` delivered movement, but both axes were reversed between
  physical motion and the guest pointer;
- `usb-tablet` delivered correctly oriented movement through the same guest
  stack and does not require relative pointer capture.

The raw HCD bytes and the `InputEvent` values were identical, so no inversion
belongs in `usb2otg.device` or `hid.class`; doing that would break standards-
compliant physical USB mice. Graphical `run.sh` boots therefore use
`-device usb-tablet` as the stable QEMU test device. Relative `usb-mouse`
remains useful as an explicit diagnostic override, not the default.
