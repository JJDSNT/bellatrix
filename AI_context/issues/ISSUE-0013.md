---
id: ISSUE-0013
title: "Port dma.resource, and with it the option of a DMA SD card path"
status: backlog
priority: medium
type: feature
owner: unassigned
created_at: 2026-08-06
updated_at: 2026-08-17
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
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_bcm2708bus.c
  - aros/arch/m68k-emu68/soc/sdcard/mmakefile.src
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

## Correction: the resource is not interrupt-free any more

This issue says the completion is "waited on with a timeout, not signalled by
an interrupt", and uses that to keep it out of the interrupt-delivery question.
**That stopped being true upstream.** Today's `dma_init.c` registers a handler
per channel (`KrnAddIRQHandler(IRQ_DMA0 + channel)`), sleeps on the signal with
a 4 ms timer pulse as a safety net, and exposes `DMACHF_IRQ` for drivers that
want to own the line themselves.

So `dma.resource` is this port's **second interrupt consumer**, and it arrived
before USB did. See ISSUE-0039.

## Why this is not obviously worth doing yet

**Nothing measured says throughput is the problem.** The port's open failure is
a *hang*: four runs in ten reach an empty Workbench and sit there, one of them
for a full 900 s without a pixel changing (ISSUE-0007). DMA does not fix a hang.

**Nobody has measured how much of the boot is card I/O.** The boot reaches the
icons in about 53 s. What fraction of that is the SD path is unknown, and it is
cheap to find out compared with writing a driver. That measurement is the
precondition for this issue being worth its cost — if the answer is 5%, the
best possible outcome here is 2.5 s.

# Measured, 2026-08-17 — and two of the three obstacles are already gone

## The number

`patches/aros/0024` brackets the PIO word loop and nothing else, because that
loop is exactly what a DMA path would replace. One boot to the Wanderer
desktop, under QEMU:

```
[SDBus00] PIO: 7168 KiB in 13110 ms, 14339 blocks
```

| | |
|---|---|
| time inside the PIO word loop | **13.1 s** |
| moved | 7.0 MiB, 14339 blocks of 512 bytes |
| rate | 547 KiB/s |
| per block | 0.91 ms |
| per 32-bit MMIO read | 7.1 µs (128 per block) |

**The first figure taken was 18.1 s and was wrong** -- measured on a host with
other work on it. The block count is identical across both runs (14339), so the
workload is deterministic and the two are exactly comparable: **38% of the
first number was host load**, not card. Recorded because a boot-time figure
from this project has been distorted by host contamination before
(ISSUE-0048), and because the corrected number is the one that decides this
issue.

The whole boot reads about **7 MiB** from the card and then stops — the counter
does not move again once the desktop is up, so that figure is the boot, not a
sample of it.

**Read this as the shape, not the magnitude.** It is QEMU, where every MMIO
read traps into a device model, so 9.8 µs per longword is an emulator artefact
and the fraction it represents does not transfer to hardware. What does
transfer is the structure: 14339 blocks × 128 reads = **1.8 million individual
MMIO accesses per boot, each one executed as JITted m68k code**. That count is
a property of PIO and of the block size, not of the host.

The instrument is now in the ELF, so **the same figure on a real Pi is one boot
away**, and that is the number that actually decides this issue.

## Two prerequisites fell out of the USB work

Neither was noticed here because they were paid somewhere else:

* **"What is a bus address, seen from an m68k guest under Emu68?"** — answered.
  `external/emu68/src/aarch64/start.c:1365` is
  `mmu_map(mb_Base, mb_Base, size, ...)` with the signature `mmu_map(phys,
  virt, ...)`, so the guest's DRAM is identity-mapped; and the SD backend's own
  mailbox buffer proves it at runtime on every boot, since the card only powers
  on because the VideoCore reads and writes a heap pointer AROS handed it. See
  ISSUE-0019's execution log, step 2.
* **Cache coherency** — answered and implemented (`90020ee`). `CacheClearE()`
  honours `CACRF_ClearD`/`CACRF_InvalidateD` a page at a time with `CPUSHP`,
  and on 68040 both `CachePreDMA_40` and `CachePostDMA_40` end in that same
  vector. Measured at the time: the page strategy beat a 16-byte `CPUSHL` loop
  by about eleven seconds of boot, because under a JIT the unit of cost is the
  instruction, not the cache line.

So step 3 — `dma.resource` itself, ~250 lines and three entry points with no
interrupt dependency — is now the cheap part.

## What did not fall, and it is the expensive one

Step 4 stands unchanged and is worth restating because the two prerequisites
falling makes it easy to read this issue as unblocked. **The DMA-capable
backend is SDHOST, a different controller from the Arasan this port uses.**
Arasan was chosen precisely because SDHOST refuses to run without a DMA
channel. Bringing DMA is not adding a transfer method to today's driver; it is
switching controllers, and everything already learned about the Arasan's
timing, its endianness split and its per-boot reset (ISSUE-0029) does not
carry over.

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

## 2026-08-17 — the card moves to SDHOST, and this stops being about speed

**Target configuration, decided by the user:**

```
GPIO 48-53  ->  ALT0  ->  SDHOST  ->  SD card       (needs dma.resource)
GPIO 34-39  ->  ALT3  ->  Arasan  ->  SDIO -> WiFi
```

The reasoning is not a preference and not a performance argument. On the
BCM2837 the Arasan SDHCI is a single controller, and AROS's own WiFi stack for
this hardware is written against it: `sdio.resource` is *"SDIO host bound to
the BCM2835 Arasan SDHCI controller"* (`sdio_init.c:4`) and `bwfm.device`'s
comments name the Arasan card interrupt by name (`bwfm_dev.c:48`). One
controller cannot serve GPIO 34-39 and GPIO 48-53 at once.

So **while the card is on the Arasan, WiFi on this machine is not difficult,
it is impossible.** Moving the card to SDHOST is the only configuration in
which both work, which makes it a property of the machine being built rather
than an optimisation to justify with a benchmark.

That reframes this issue. `dma.resource` is no longer something to adopt if a
measurement says it pays; it is a prerequisite of a configuration that has to
be reached anyway, and the DMA path's throughput is a side effect of getting
there.

**Nothing about WiFi is being built.** The standing freeze holds; this records
where the card has to end up so the SD work is not done twice.

### What already exists in the tree, for arm-native

| piece | where |
|---|---|
| card on SDHOST | `arch/arm-native/soc/broadcom/2708/sdcard/sdcard_sdhost_{init,bus}.c` |
| `dma.resource` | ported to this target on 2026-08-17, see the log below |
| `sdio.resource` | `arch/arm-native/soc/broadcom/2708/sdio/` — 47 KB, Arasan-bound |
| `bwfm.resource` | `arch/arm-native/soc/broadcom/2708/bwfm/` — chip bring-up, firmware |
| `bwfm.device` | `workbench/devs/networks/bwfm/` — SANA-II |

### One thing that turned out not to be a problem

Whether the boot firmware leaves the card's pins pointed at the wrong
controller: it does route them to Arasan by default, and it does not matter.
**Each driver muxes its own pin group.** `sdcard_sdhost_init.c:10-15` documents
GPIO 48-53 and switches them to ALT0 itself; `sdio_init.c:232` routes GPIO
34-39 to ALT3 for the WiFi bus. No `config.txt` change is implied.

## Earlier framing, kept

The original note said: the current PIO/Arasan choice is deliberate and correct
until measured otherwise, not an omission to be tidied up. That was true while
the Arasan was the only controller anyone had asked for. It is superseded by
the decision above, not contradicted by it -- Arasan/PIO remains the right
answer for bring-up, and bring-up is over.

# Acceptance criteria

- [x] The fraction of boot time attributable to the SD path is measured and
      recorded here — under QEMU. **Still wanted on hardware**, where the
      magnitude is meaningful; the instrument is already in the ELF
- [x] The guest→bus address translation under Emu68 is written down and verified
- [x] `dma.resource` builds and allocates a channel on `m68k-emu68` — channel
      8, with `DMACHF_IRQ`, allocated and freed on every boot by
      `arch/m68k-emu68/boot/dma_probe.c`
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
- 2026-08-17 — **`dma.resource` ported and proven to allocate.** Built from
  arm-native's source compiled in place, linked into the ELF, and exercised by
  a probe resident that opens it, takes a channel with `DMACHF_IRQ` and gives
  it back:

  ```
  [DMA:probe] allocated channel 8
  [DMA:probe] released channel 8
  ```

  Channel 8 is a lite engine, which is the pool behaving as designed -- lite
  channels are handed out first so the scarce full engines stay free for
  `DMACHF_TDMODE` users. `DMACHF_IRQ` means it went through
  `KrnAddIRQHandler(IRQ_DMA0 + 8)`, so this also exercises the interrupt path
  ISSUE-0039 changed the same day.
- 2026-08-17 — Measured, at the user's question "dma.resource or something
  else?". 13.1 s inside the PIO word loop for 7 MiB, under QEMU (first
  reading of 18.1 s was host-contaminated; see above). Recorded that the
  address and cache-coherency prerequisites were already answered by the USB
  work, and that the controller switch in step 4 was not.
