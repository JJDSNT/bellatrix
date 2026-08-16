---
id: ISSUE-0013
title: "Port dma.resource, and with it the option of a DMA SD card path"
status: backlog
priority: medium
type: feature
owner: unassigned
created_at: 2026-08-06
updated_at: 2026-08-06
tags:
  - sdcard
  - dma
  - performance
  - emu68
blockers:
related_files:
  - external/aros/arch/arm-native/soc/broadcom/2708/dma/dma_init.c
  - external/aros/arch/arm-native/soc/broadcom/2708/dma/dma.conf
  - external/aros/arch/arm-native/soc/broadcom/2708/sdcard/sdcard_sdhost_bus.c
  - external/aros/arch/arm-native/soc/broadcom/2708/include/hardware/bcm2708_dma.h
  - aros/arch/m68k-native/soc/sdcard/sdcard_bcm2708bus.c
  - aros/arch/m68k-native/soc/sdcard/mmakefile.src
---

# Summary

This port has no `dma.resource`, which is why its SD card backend is the Arasan
controller in pure PIO. That choice is recorded as deliberate in
`soc/sdcard/mmakefile.src`, and it was the right call for bring-up. It also
means every 512-byte block is 128 32-bit MMIO reads executed as **JITted m68k
code**, one page-fault-shaped access at a time.

`arch/arm-native` already has both halves — a small `dma.resource` and an SDHOST
backend that uses it. This issue is to port them, and to be honest about what
that is worth before doing it.

# Problem

## What exists on the other CPU

`arch/arm-native/soc/broadcom/2708/dma/` is small: `dma_init.c` is ~250 lines
and the whole public interface is three calls
(`arch/arm-native/soc/broadcom/2708/dma/dma.conf`):

```
int  DMAAllocChannel(unsigned int flags)
void DMAFreeChannel(int channel)
int  DMAWaitChannel(int channel, unsigned int timeout_us)
```

Completion is **waited on with a timeout, not signalled by an interrupt**, which
matters here: it keeps this out of the unresolved interrupt-delivery question in
ISSUE-0004/ISSUE-0010.

`sdcard_sdhost_bus.c` is the consumer, and it has already solved the things that
are easy to get wrong: a 32-byte-aligned control block, a bounce buffer for
misaligned caller buffers, DREQ pacing against the controller, and the address
conversion at line 69:

```c
return BCM2708_DMA_BUS_ADDR((ULONG)(IPTR)KrnVirtualToPhysical(virt));
```

## The one question that does not port

**What is a bus address, seen from an m68k guest under Emu68?**

The DMA engine is real silicon and consumes real bus addresses. On arm-native,
`KrnVirtualToPhysical()` plus a fixed alias gets there because the CPU running
the driver is the ARM the address space belongs to. Here the driver runs as m68k
code in an address space Emu68 constructs, and whether a guest address relates
to the ARM physical address by anything as simple as an offset is exactly what
has to be established first.

There is a hint that it might be simple: the SD backend already reaches the
peripherals directly at the base it discovers from the FDT
(`KATTR_PeripheralBase`), so at least the peripheral window is where the ARM
would put it. That is a hint, not an answer, and it must not be assumed for RAM.

**This is the same question ISSUE-0012 hits** for the hardware cursor's pixel
buffer, where the arm-native reference sidesteps it by asking the firmware for
GPU memory through the mailbox rather than translating an address it already
has. Whichever of the two moves first should answer it once, for both.

Cache coherency is the second half of the same problem — `CachePreDMA`/
`CachePostDMA` exist in `rom/exec`, and what they have to mean under Emu68's JIT
is its own question.

## Why this is not obviously worth doing yet

**Nothing measured says throughput is the problem.** The port's open failure is
a *hang*: four runs in ten reach an empty Workbench and sit there, one of them
for a full 900 s without a pixel changing (ISSUE-0007). DMA does not fix a hang.

**Nobody has measured how much of the boot is card I/O.** The boot reaches the
icons in about 53 s. What fraction of that is the SD path is unknown, and it is
cheap to find out compared with writing a driver. That measurement is the
precondition for this issue being worth its cost — if the answer is 5%, the
best possible outcome here is 2.5 s.

# Goal

`dma.resource` available on `m68k-emu68`, and a measured answer to whether a DMA
SD path is worth having.

# What is left

1. **Measure first.** What fraction of the ~53 s boot is spent in the SD path.
   `scripts/boot-timing.py` gives the whole-boot number; the split needs
   something narrower.
2. **Establish the address story.** Guest m68k address → ARM physical → VC bus
   address, under Emu68, for a buffer AROS allocated. Write it down before any
   descriptor is built; a DMA engine handed a wrong address does not fail
   politely.
3. **Port `dma.resource`** — three entry points, no interrupt dependency.
4. **Then, and only then, the SDHOST backend.** Note this is a *different
   controller*, not just a different transfer method: today's backend is Arasan
   precisely because SDHOST refuses to run without a DMA channel. Switching
   controllers changes more than the data path.

# Decisions taken

None. Recorded so the framing is not lost: the current PIO/Arasan choice is
deliberate and correct until measured otherwise, not an omission to be tidied
up.

# Acceptance criteria

- [ ] The fraction of boot time attributable to the SD path is measured and
      recorded here
- [ ] The guest→bus address translation under Emu68 is written down and verified
- [ ] `dma.resource` builds and allocates a channel on `m68k-emu68`
- [ ] A DMA transfer moves correct bytes — verified against the same read done
      in PIO, not merely "it did not hang"
- [ ] Boot time measured before and after, ≥3 runs each per `CLAUDE.md`

# Notes

**This one is testable under QEMU**, unlike ISSUE-0012. `qemu-system-aarch64`
8.2.2 carries both `bcm2835-dma` (with per-channel devices) and
`bcm2835-sdhost`, so the whole path can be brought up locally before any
hardware is involved. That is a real advantage and part of why this is worth
recording as its own issue rather than folding into a performance wish-list.

The endianness distinction in `sdcard_bcm2708bus.c` — registers are values and
must be swapped, the data FIFO is a byte stream and must not be — does not go
away with DMA. It moves: the descriptor fields are little-endian values
(`AROS_LONG2LE(SDHOST_SDDATA_DMA_ADDR)` in the reference) while the payload the
engine moves is untouched bytes. See ISSUE-0009 for why that distinction is
currently load-bearing.

# Execution log

- 2026-08-06 — opened, out of the question "should we have dma.resource?" while
  looking at why `LoadSeg` is where the intermittent stall lands. It is not the
  answer to that stall — a hang is not slowness — but it is a real gap, and the
  code to fill it already exists in this tree for the other CPU.
