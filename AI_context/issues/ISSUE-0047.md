---
title: "Replace the inherited AArch64 DWC2 port with a Bellatrix-owned driver"
status: in-progress
updated_at: 2026-08-26
date: 2026-08-21
components:
  - usb
  - dwc2
  - poseidon
  - m68k-emu68
---

# Decision

Bellatrix will implement its own DWC2 host-controller driver under
`aros/arch/m68k-emu68/soc/usb/dwc2emu68`. The replacement now enumerates devices
and passes the QEMU smoke tests, so `usb2otg.device` and the patches which made
the arm-native implementation usable on m68k have been removed from the pack.

Only after the replacement works will redundant arm-native USB patches be
removed. Poseidon and the generic AROS USB stack are not being replaced.

# Why

The inherited driver intertwines the Poseidon ABI, controller scheduling,
platform DMA/cache rules, recovery and substantial work in interrupt context.
Repeated fixes did not remove the permanent 1 kHz SOF interrupt cost and have
made correctness difficult to reason about. The Bellatrix driver instead uses:

- a unit task as the sole owner of request and controller state;
- a minimal interrupt top half which only acknowledges/masks and signals;
- an explicit emu68 MMIO, DMA alias, cache and barrier boundary;
- event-driven SOF enabling rather than an unconditional 1 kHz guest IRQ;
- bounded task-context timeouts and recovery.

The emu68 xHCI driver is an architectural reference for ownership and interrupt
separation, not a hardware implementation source. The controller remains the
Pi 3/QEMU Synopsys DWC2.

# State, corrected 2026-08-24

This section previously claimed the Bellatrix driver was the distribution's
active host controller. It was not. `mmakefile.src` listed the five inherited
`usb2otg_*` sources in `FILES` and renamed them at compile time with
`-Dusb2otg=dwc2emu68`; the `dwc2emu68_*` sources sat beside them and were
never compiled. The issue had run ahead of the code, and the mmakefile's own
comment said so.

## Two drivers, both ours, compared before either is deleted

The plan is now explicit, and it is the shape `soc/sdcard` already uses for
its two backends:

1. adopt the legacy engine as our own code, out of the patch series -- done;
2. port it by rewriting, gradually;
3. keep both building the whole time;
4. compare them running, and only then keep one.

The rename was what stopped step 3 from being real: one module cannot be two
drivers, and `-Dusb2otg=dwc2emu68` made the inherited engine build under the
rewrite's name. They are now separate:

| | `soc/usb/usb2otg` | `soc/usb/dwc2emu68` |
|---|---|---|
| what | the AROS DWC2 engine, adopted | the Bellatrix rewrite |
| source | 394 KB, 7 files | 62 KB, 8 files |
| module | `usb2otg.device`, 62 KB | `dwc2emu68.device`, 26 KB |
| metatarget | `kernel-usb-arosotg` | `kernel-usb-dwc2emu68` |

`kernel-usb-arosotg` and not `kernel-usb-usb2otg`: arm-native already defines
that name for the original of this code, and mmake matches metatargets across
the whole tree -- asking for it here built arm-native's copy and died on a
`yield` instruction the m68k assembler does not have. `build-aros.sh` builds
both, because building only one is how the other stops being code.

## How far the port actually is

Measured from the command dispatch, not from intent:

| command | `usb2otg` | `dwc2emu68` |
|---|---|---|
| `UHCMD_CONTROLXFER` | `cmdControlXFer` | implemented, root hub and devices |
| `UHCMD_INTXFER` | `cmdIntXFer` | implemented, root hub and devices |
| `UHCMD_BULKXFER` | `cmdBulkXFer` | implemented 2026-08-24 |
| `UHCMD_ISOXFER` | queues, never completes | `IOERR_NOCMD` |

**The iso row is a correction.** An earlier version of this section said the
adopted engine had full coverage and the rewrite did not. It does not:
`usb2otg_intern.h` says so in as many words --

    /* ISO is intentionally absent: cmdIsoXFer queues but nothing drains. */

-- and `cmdIsoXFer` does exactly that: `AddTail` to `hu_IsoXFerQueue`, return
`RC_DONTREPLY`, with the `Cause()` that would drain the queue commented out.
The request is never answered. That is worse than refusing it, because a
caller waits forever instead of getting an error.

So with bulk done, the rewrite's coverage is equal or better on every row.

**Corrected 2026-08-26: that sentence is true per command and misleading as a
summary, and it argued for the wrong decision for two days.** A table indexed
by command answers "does it know how to do this kind of transfer" and hides
"how many can it do at once, and can it reach the device at all". Both hidden
answers were bad:

| | `usb2otg` | `dwc2emu68` (before 2026-08-26) |
|---|---|---|
| hardware channels | `hu_Channel[8]`, all used | `#define DWC2_CHANNEL 0`, one |
| transfers in flight | one per channel | one, `active_request` |
| channel interrupt mask | all eight | `HAINTMSK = 1UL << 0` |
| split transactions | 27 `HCSPLT` + 44 `UHFF_SPLITTRANS` uses | `HCSPLT` written to 0, always |

The split row is the one that decides anything. Every USB port on a Raspberry
Pi 3 is behind the LAN9514, a high-speed hub, and a low- or full-speed device
behind a high-speed hub is only reachable through split transactions. Writing
`HCSPLT = 0` is not "less complete" -- it is a driver that cannot talk to a
keyboard on this board. The 2026-08-24 validation never noticed because QEMU's
`usb-tablet` attaches to the root port with no hub in between.

Isochronous was listed here as out of scope under the 2026-08-17 freeze. That
reading is withdrawn: USB on this port is incomplete and defective, so
finishing it is repair, not new capability.

**Two defects fixed while doing this:**

- the `NSCMD_DEVICEQUERY` table advertised `UHCMD_BULKXFER` and
  `UHCMD_ISOXFER` while the switch answered `IOERR_NOCMD` to both, so the
  device promised Poseidon transfers it then refused;
- the data toggle was indexed by endpoint number alone. Endpoint 1 IN and
  endpoint 1 OUT are two endpoints with independent toggles and were sharing
  one bit -- harmless while only interrupt IN existed, wrong the moment a
  device uses both directions, which every bulk device does.

## Validated end to end, 2026-08-24

`BELLATRIX_USB=1 BELLATRIX_USB_DRIVER=dwc2emu68 ./scripts/build-aros.sh`, then
`./run.sh` with the usb-tablet QEMU attaches to the raspi3b DWC2:

    [DWC2/Emu68] OT2 core OT2.94a at f2980000
    [DWC2/Emu68] host initialized with 8 channels, SOF enabled
    [DWC2/Emu68] root port powered, HPRT=00021003
    [DWC2/Emu68:RH] port reset complete HPRT=0002100d
    [DWC2/Emu68:XFER] submit #8 cmd=12 addr=2 ep=0 len=18

and on the card, written by the Startup-Sequence:

    Adding hardware DEVS:USBHardware/dwc2emu68.device, unit 0...okay!

Poseidon accepted the driver, enumerated the virtual root hub over 24 control
transfers, reset port 1, addressed the attached device as address 2 and read
its descriptors over 7 more. No errors, no stalls. The boot still reaches
`hold released: icons`, at 1:20 against 1:06 with USB off.

"End-to-end input remains under investigation" is therefore retired: it
enumerates.

**Not clean, though:** 8 watchdog recoveries fired during enumeration
(`[DWC2/Emu68:WD]`), all on control data stages that completed without
raising a channel interrupt. Nothing failed, but a transfer engine that needs
its watchdog eight times in one enumeration is relying on it, and that is
worth understanding before the comparison decides anything.

> **Retracted 2026-08-26. There were no recoveries, and the 8 was the log
> filling up.**
>
> Two mistakes in one sentence, and it held up a decision for two days.
>
> `dwc2_transfer_watchdog()` prints its detailed line for *every* tick that
> finds a channel with a request, before it has looked at anything --
> recovery or not. Then, separately, `if (status != 0)` is the recovery. So
> a `[DWC2/Emu68:WD]` line means "the watchdog ran while a transfer was in
> flight", which is what a watchdog does; it does not mean anything was
> rescued.
>
> And the count was never a count: the line is gated on
> `if (unit->watchdog_log_count < 8)`. Eight is the cap. The number of ticks
> was unknown and the number of recoveries was never measured at all.
>
> An uncapped counter was added, reporting on the powers of two so a handful
> stays quiet and a storm cannot hide. Measured over a full enumeration plus
> interrupt polling: **8 watchdog ticks, 0 recoveries.** The engine does not
> rely on its watchdog. It never did.
>
> This is the trap `CLAUDE.md` records for the boot probes, wearing different
> clothes: a counter that hit its ceiling and a true count are the same text
> in a log.

**USB is off by default**, separately and deliberately: the
`kernel-usb-m68k-emu68` alias is commented out and `build-aros.sh` deletes
`Devs/USBHardware` after building, so both drivers keep compiling without
going on a card.

It is not, however, a rebuild-by-hand to turn on, which this section used to
imply. `build-aros.sh` takes two variables:

```sh
BELLATRIX_USB=1 BELLATRIX_USB_DRIVER=usb2otg   ./scripts/build-aros.sh
BELLATRIX_USB=1 BELLATRIX_USB_DRIVER=dwc2emu68 ./scripts/build-aros.sh
```

`BELLATRIX_USB=1` additionally builds `kernel-usb-nopci` -- the Poseidon
stack, `SYS:Classes/USB`, `C:AddUSBHardware`, `C:PsdStackLoader` -- which the
disabled path removes from the tree. `BELLATRIX_USB_DRIVER` picks which single
controller stays on the card; both are always compiled. `S:Startup-Sequence`
tests for each `.device` separately, so neither name is baked into it.

# What the inherited engine does on real hardware, 2026-08-26

`usb2otg` has run on the Pi many times, and repeats this:

```text
[USB2OTG] SOF: IntXFerQueue walk runaway - list cycle, aborting walk
```

The message comes from a guard in `usb2otg_intr.c:940` whose comment says a
cross-linked node "turns this walk into a cycle (seen hanging the machine)".
The guard is a bound on the damage, not a fix: it stops the SOF walk after 128
nodes and leaves the corrupted list in place.

Two findings about its shape, neither of which names the guilty insertion --
that needs instrumentation that says *which* request is doubled:

- **The engine already knows it double-queues, and scrubs rather than
  prevents.** `usb2otg_remove_bulk_queue_duplicates()` exists for no other
  purpose than to walk `hu_BulkXFerQueue` looking for the request about to be
  inserted and remove it if it is already there. There is no equivalent for
  `hu_IntXFerQueue`, which is the list that cycles.
- **Requests are returned to two different lists depending on which path
  failed.** The IRQ error paths and the watchdog `ADDTAIL` to
  `hu_IntXFerQueue` (`usb2otg_intr.c:1890, 1981, 2198, 3303`); the scheduler's
  setup-failure path `ADDHEAD`s to `hu_IntXFerScheduled`
  (`usb2otg_schedule.c:363`). A node reachable from both is exactly the
  "cross-linked node" the guard names, and the SOF walk -- which only knows
  about `hu_IntXFerQueue` -- walks into the other list and never comes out.

Counted across the engine: **34 insertions into the four request queues
against 7 removals.** That ratio is the shape of the defect. It is not a wrong
line; it is the absence of a single owner for a request's list membership.

One hypothesis was tested and rejected, and is recorded so it is not tried
again: the three `if (hu_Channel[chan].hc_Request != req)` guards commented
"Watchdog already handled this request" are inside `#if defined(__AROSEXEC_SMP__)`,
and this target does not define that macro (the generated
`bin/emu68-m68k/gen/include/aros/config.h` has no `#define`). That looked like
the watchdog's claim being compiled out. It is not: `usb2otg_intr.c:1282`
wraps the whole block in `if (req)`, so a request the watchdog claimed by
setting `hc_Request = NULL` is skipped on non-SMP too.

# Decision, 2026-08-26: rewrite

`usb2otg` becomes the **specification**, not the base. It is kept, built and
read; the knowledge in it -- split sequencing, the NAK and NYET policies, the
channel quarantine, the BCM2837 errata -- was earned on hardware and is
expensive to re-earn. What is not kept is its structure.

The reasoning, in the order it actually decides:

1. The defect above is not a bug at a location, it is the absence of an owner.
   Imposing one on `usb2otg` means rewriting its core across 34 insertion
   sites inside a 3,610-line interrupt handler -- the whole cost of a rewrite,
   without ever being able to declare one, and therefore without the benefit.
2. `dwc2emu68` already has the owner: one unit task, an interrupt top half
   that only acknowledges and signals, and per-channel state that makes
   "which transfer is this interrupt about" answerable from the channel index
   rather than from ambient state.
3. What the rewrite lacked -- channels, splits, isochronous -- is content to
   be written, not an argument against writing it. An earlier version of this
   reasoning treated the missing splits as a property of the rewrite rather
   than as work; that was answering the wrong question.

**The switch-over condition is not a date.** `usb2otg` stays the controller on
the card until `dwc2emu68` enumerates a keyboard behind the LAN9514 on real
hardware. Both keep building the whole time.

# Validation of the rewrite, QEMU, 2026-08-26

`./run.sh --headless -- -device usb-tablet`, against the same run with the
pre-refactor engine restored by `git stash` -- a control, not a memory of what
the log used to look like.

| | control (single-channel) | rewritten |
|---|---|---|
| control stages | `1 -> 2 -> 3` | `1 -> 2 -> 3` |
| device addressed | `submit #8 addr=2 len=18` | same, and further |
| channel interrupts | 23 | 23 |
| watchdog recoveries | 8 | 8 |
| serial lines | 373 | 370 |

Parity. The refactor neither broke the enumeration nor improved the watchdog
recoveries, which is what a structural change should look like.

**The control run earned its keep.** The first rewritten run did not get past
the SETUP stage: the stage never advanced, no arming followed the completion,
and the device answered STALL. Reasoning about it produced a plausible and
wrong theory; restoring the old engine and running the same test took ten
minutes and said plainly that the regression was in the new code.

The cause was one bit too many in `split_irq()`. A start-split was treated as
accepted on `ACK | XFERCOMP`, where `usb2otg` keys on `ACK` alone and is
right: `XFERCOMP` on a start-split does not mean the translator took the
transaction, it means the transaction finished outright -- which is what a
controller with no real transaction translator reports. So a finished control
SETUP was turned into a complete-split against a device that was never behind
a translator.

**One thing this run taught that changes how much QEMU is worth here.** The
`usb-tablet` is a full-speed device on a high-speed port, so Poseidon sets
`UHFF_SPLITTRANS` and the split sequencer is entered. QEMU does not emulate a
transaction translator, so it cannot validate the SSPLIT/CSPLIT handshake --
but it does exercise the sequencing, and it caught this bug without hardware.
Earlier notes here assumed the split path was unreachable off the Pi.

What is still unexercised is concurrency: Poseidon enumerates serially, so
every transfer in this trace ran on `chan=0`. Several channels in flight needs
several devices, which needs the hub, which needs the Pi.

## Second pass, same day

Three more things were ported after the first pass claimed only testing
remained. That claim was wrong, and checking rather than answering from memory
is what found them.

**The two in-flight guards, and they were urgent because of the refactor.**
Running more than one channel makes two situations possible that could not
happen before:

- two transactions to the same device address, endpoint and direction at once.
  An endpoint has one data toggle and one state machine; two transactions race
  over both and the toggle ends up describing neither.
- two split transactions on the same hub's translator. `../usb2otg` allows
  exactly one per hub and says why: overlapping split streams take the channel
  down, and cross-device damage on a shared hub has been observed. On a Pi 3
  every port is behind one LAN9514, so a keyboard and a mouse are precisely
  the pair this protects. Matched on hub address alone, not hub+port, because
  these hubs are single-TT.

Neither guard has been exercised yet, for the same reason concurrency has not.

**NYET is now unmasked for splits only.** It was unmasked for everything,
which predates the refactor. On a split it is the pacing signal the
complete-split sequencer needs; on anything else the buffer-DMA core drives
NAK, NYET and PING itself, and taking the interrupt halts the channel in the
middle of a burst the core was managing. `../usb2otg` calls that fighting the
core and opening wedge windows.

This was tested as a hypothesis for the watchdog behaviour and did not explain
it -- which is how the log-cap mistake above came to light. It stays on its own
merits.

**Complete-split pacing.** The complete-split was issued in the same
microframe as the start-split, asking the translator for an answer it cannot
have yet. Now paced by transfer type with `../usb2otg`'s hardware-bisected
values: one microframe for interrupt, sixteen (2 ms) for control, immediate
for bulk. A paced complete-split holds its channel -- it is waiting for a
microframe, not free -- and the SOF handler counts it down.

Measured after all three, over a full enumeration plus interrupt polling:
9 submissions, 23 channel interrupts, 0 errors, 0 watchdog recoveries, and the
interrupt endpoint polling with NAK parking at its requested interval.

# First run on real hardware, 2026-08-26

A Raspberry Pi 3, `BELLATRIX_USB=1 BELLATRIX_USB_DRIVER=dwc2emu68`. The
controller comes up, the virtual root hub enumerates, the port resets -- and
the very first SETUP of the first real device never completes:

```text
[DWC2/Emu68:XFER] submit #1 chan=0 cmd=12 addr=0 ep=0 len=8
[DWC2/Emu68:XFER] arm chan=0 stage=1 CHAR=00100008 TSIZ=60080008 DMA=c45cb1c0
[DWC2/Emu68:WD] #1 chan=0 stage=1 ... HAINT=00000000/00000001
                HCINT=00000000/000005ff CHAR=80100008 DMA=c45cb1c8
... eight ticks ...
[DWC2/Emu68:WD] chan=0 timed out stage=1 CHAR=80100008
```

Read it register by register: `DMA` advanced from `c45cb1c0` to `c45cb1c8`, so
the eight-byte SETUP was handed to the core and went out. `CHENA` stays set.
`HAINT` and `HCINT` are both zero for the whole budget -- the channel never
raises anything. The transfer did not fail; it was never answered.

**`HCINTMSK=0x5ff` is what names the cause.** The mask for a non-split
transfer is `0x59f`; `NYET|ACK` adds `0x60`, giving `0x5ff`. So this transfer
was armed as a split transaction -- against the root port.

## Why Poseidon asked for a split, and why it could not work

Poseidon's rule (`poseidon.library.c:4581`, `PDFF_NEEDSSPLIT`) is that a USB
1.1 device behind a USB 2.0 hub needs split transactions. It applied that rule
correctly to two facts:

- `HPRT=00021403` -> `PRTSPD=01`: **the root port negotiated full speed**;
- `dwc2emu68_root.c:31`: the virtual root hub declared `bcdUSB = 0x0200`,
  a USB 2.0 hub, unconditionally.

A full-speed device behind a USB 2.0 hub -- so, splits. But the DWC2 root port
has no transaction translator. A start-split there addresses nothing, and the
core sits with the channel enabled forever, which is precisely the log above.

QEMU hid this completely: the `usb-tablet` is also a full-speed device on a
port whose root hub claimed USB 2.0, so the flag was set there too -- but
QEMU's model answers a complete-split as though a translator existed, and the
transfer went through.

## Both sides fixed

- `split_needed()` in the transfer engine declines a split when the named hub
  is our own root hub, or when the root port did not come up at high speed.
  The same predicate gates the TT in-flight guard, so a transfer that runs
  directly does not reserve a translator it is not using.
- The root hub's `bcdUSB` now follows the port: `0x0110` unless `PRTSPD` says
  high speed. The engine declining a split is a defence; not making the false
  claim is the fix.

Validated in QEMU: `HCINTMSK` is `0x59f`, enumeration unchanged at 9
submissions, 23 channel interrupts, 0 errors.

Note what that validation is worth. The only thing exercising the split
sequencer under QEMU was the bug itself; with it fixed, QEMU no longer enters
that code at all. The sequencer is once again testable only on a Pi, and only
with a real high-speed hub below the port.

## Open, and not caused by any of the above

**The root port negotiates full speed on a Pi 3, and it is not understood.**
The LAN9514 is a high-speed hub and should chirp up to 480 Mbit/s. `PRTSPD=01`
and `HFIR=0000ea60` (60000, a full-speed frame interval) say the bus is
running at 12 Mbit/s.

`GUSBCFG=0x20001400` looks right for a high-speed UTMI+ PHY -- `PHYSEL=0`,
`ULPI_UTMI_SEL=0`, `ULPIFSLS=0` -- but note that this driver never writes
those bits: it inherits whatever the Pi firmware left and adds
`FORCEHOSTMODE`. `../usb2otg` does not inherit. It clears `PHYINTERFACE` and
`MODESELECT_UTMI` after the core reset and then decides `ULPIFSLS` from the
PHY types in `GHWCFG2` (`usb2otg_device.c:328-347`). That difference is where
to look next, and it is deliberately not ported yet: blind PHY bring-up fails
by making the controller not appear at all, which is a worse log to debug from
than a slow bus.

One discrepancy noticed and deliberately not acted on: `USBTRDTIM=5` with
`PHYIF=0` (8-bit UTMI+), where Linux's rule is 9 for 8-bit and 5 for 16-bit.
The value comes from the Pi firmware, which knows its own PHY better than a
table does.

**A defect of ours found while reading this, and left alone on purpose.**
`dwc2emu68_controller.c:176` writes `HFIR = 60000` unconditionally, which is
full-speed frame timing. It does not cause the negotiation -- speed is settled
by the PHY chirp and reported in `PRTSPD` -- but it means the driver is
written assuming full speed, and would be wrong by a factor of eight if the
port ever came up high speed. Not fixed in the same pack that exists to answer
whether the split fix works.

# Remaining work
- take it to the Pi and enumerate a device behind the LAN9514 -- the first
  test of the split path, which QEMU cannot exercise;
- ~~understand the 8 watchdog recoveries~~ -- retracted above: there were
  none, and the 8 was a log cap. Measured 0 recoveries over a full
  enumeration;
- port what is still missing, none of it blocking a first Pi run:
  **channel quarantine** (`hu_DeadChannels`, `usb2otg_exorcise_channel`, the
  blackout states -- ~55 references, none here; deliberately not ported blind,
  since it is compensating machinery whose need may not survive single
  ownership), the **NAK gating policy** (167 references against 12), and the
  **PING protocol** for high-speed bulk OUT, which masking NYET may already
  have delegated to the core;
- only then, retire `usb2otg` to reference and delete it from the build.

# What was written, 2026-08-26

Three stages, each compiling before the next began. `usb2otg` was open beside
each of them as the specification.

## Channels

`struct DWC2Channel` now carries the request, stage, retry budget, bounce
buffer and watchdog ticks; `unit->channel[DWC2_MAX_CHANNELS]` replaces the
single `active_request`. Nothing in the engine reads a "current transfer" from
the unit -- the channel is an argument everywhere.

Four things had to change with it, and three of them were latent bugs the
single-channel shape hid:

- **The ISR read `DWC2_HCINT(0)` unconditionally.** With eight channels that
  both misses seven of them and attributes their state to channel 0. It now
  walks `HAINT & HAINTMSK`, takes each flagged channel's `HCINT`, and hands
  the bits back through the channel that produced them. `channels_pending`
  became a bitmap of channels -- HAINT's own shape -- instead of a single set
  of HCINT bits, which cannot distinguish two channels completing between two
  wakes.
- **`HAINTMSK = 0` on completion** would have silenced every other channel in
  flight. Release now clears only the channel's own bit, and withdraws
  `GINTSTS.HCHINT` only when no channel is left armed.
- **One shared bounce buffer** would have had two concurrent transfers writing
  over each other, with the cache maintenance of one invalidating the other's
  data mid-flight. Each channel has its own, cache-line aligned because
  `CacheClearE()` works in lines and would otherwise touch a neighbour.
- `start_next()` fills every free channel and the SOF handler promotes every
  periodic request whose frame has come round, rather than one of each.

Two defects introduced by this change and caught before it ran: `start_next()`
took a request off the queue that `submit_on()` put back, and spun; and
`arm_setup()`'s `GRSTCTL.TXFFLSH` is global, so a control transfer opening
while another channel had work queued in the non-periodic FIFO would discard
it. Control transfers now wait for the hardware to go quiet (`can_arm_now()`).

## Split transactions

`HCSPLT` is programmed from `iouh_SplitHubAddr` / `iouh_SplitHubPort` whenever
`UHFF_SPLITTRANS` is set. The sequencer is `split_irq()`, which looks at every
channel interrupt before the completion path does, because in a split
transaction most interrupts are sequencing rather than completion:

- start-split answered `ACK` -> `split_complete()` sets `COMPSPLT` and
  re-enables the channel, changing nothing else about it;
- complete-split answered `NYET` -> the translator has not finished, ask
  again, bounded to `DWC2_SPLIT_NYET_LIMIT` (8 microframes, one full-speed
  frame, the longest a translator may legitimately take);
- complete-split answered `NAK` -> the translator dropped it;
  `split_restart()` for asynchronous transfers, `park_periodic()` for an
  interrupt IN.

Three details taken from `usb2otg` rather than from the databook alone:
`XACTPOS` is `ALL` and not `BEGIN` (their comment records `BEGIN` as a
misreading of "must advance via IRQ"); `HCCHAR.EC` is 1 for non-isochronous
splits, because 3 makes the core expect three transactions and deschedule the
periodic split with a bare `CHHLTD`; and a start-split carries at most one
full-speed frame of payload.

That payload cap produced one bug of its own: 188 is not a multiple of any
legal `MaxPktSize`, so truncating to it puts a short packet in the middle of a
transfer -- and a short packet is how a device says "that is all there was".
The chunk is now rounded down to a whole number of packets (128 bytes for a
64-byte full-speed bulk endpoint).

## Isochronous

Implemented rather than refused. The design point is that isochronous has no
handshake: a missed interval is lost data, not an error to retry -- an audio
stream would rather drop a millisecond than replay it late. So the path has no
retry budget, a `XACTERR` or `FRMOVRUN` completes the request with whatever
moved instead of failing it, and a short interval does not terminate the
request the way it terminates a bulk transfer.

`UHCMD_ISOXFER` is back in the `NSCMD_DEVICEQUERY` table, and only now: that
table is a promise about what the switch does. It once listed transfers the
switch answered `IOERR_NOCMD` to, which is a lie to Poseidon; it then listed
neither, which was honest and incomplete.

What it replaces is worse than either. `usb2otg`'s `cmdIsoXFer()`
(`usb2otg_core.c:893`) adds the request to `hu_IsoXFerQueue`, returns
`RC_DONTREPLY`, and has the `Cause()` that would drain that queue commented
out. The caller waits for an answer that cannot arrive.

## Endianness

Held to as a rule rather than an outcome: every register access goes through
`dwc2_readl`/`dwc2_writel`, which say *the register is little-endian*
(`AROS_LE2LONG`/`AROS_LONG2LE`), and every USB wire field through
`AROS_LE2WORD`/`AROS_WORD2LE`. Nothing dereferences a register directly and
nothing assumes the host's byte order, so the code compiles to no swaps at all
on a little-endian host. The target is big-endian m68k; that is a fact about
today, not an assumption the code is allowed to make.

# Superseded: the single-channel implementation

**The section below describes the engine before 2026-08-26 and is kept for the
bring-up knowledge in it.** "Serializes concurrent Poseidon requests through a
FIFO" and "polls interrupt-IN endpoints round-robin" are descriptions of the
single-channel design, not of what is there now.

The `dwc2emu68.device` module is what builds, and end-to-end input remains
under investigation -- it has never been shown to enumerate a keyboard or
mouse all the way through. It:

- implements Open, Close, BeginIO, AbortIO and `NSCMD_DEVICEQUERY` entry points;
- sends requests to one unit task and never processes them in the caller;
- probes `KATTR_PeripheralBase + 0x00980000` for an OT2 `GSNPSID`;
- encapsulates endian-safe MMIO and m68k barriers in `dwc2emu68_platform.c`;
- initializes the DWC2 core in host mode with DMA and SOF masked;
- exposes a virtual root hub and implements downstream control transfers;
- acknowledges channel IRQ status in the hardware top half and defers the
  transfer state machine through an Exec software interrupt to the unit task;
- serializes concurrent Poseidon requests through a FIFO;
- polls interrupt-IN endpoints round-robin at 10 ms without a permanent SOF
  interrupt, and lowers the worker priority to avoid starving the desktop.

Validated with:

```text
make kernel-usb-dwc2emu68
./scripts/setup.sh --verify
make AROS-emu68-m68k
./scripts/make-sdcard.sh
./run.sh --headless --serial out/dwc2-qemu-final.log --sd out/aros/sd.img -- -device usb-tablet
```

The image reaches Wanderer and a QEMU `mouse_button` injection produces a
six-byte interrupt report on endpoint 1 (`01 00 00 00 00 00`). This proves the
controller-to-HID-buffer path, not visible pointer operation. The user reports
that the driver is not functional, so the issue must not be considered resolved
until the exact visible failure is reproduced and verified end to end.

`AbortIO()` was initially a stub. Poseidon can abort and replace pipes during
class selection, leaving an obsolete request active and completing into an
abandoned pipe. Cancellation of active, queued and root-hub requests is now
implemented and awaits end-to-end validation.

# Removed legacy patches

The obsolete arm-native compatibility series `0013`, `0016`, and `0018` through
`0022` was reverted and deleted after the integrated validation. Patch `0014`
remains as the generic Poseidon startup integration and patch `0053` selects the
Bellatrix-owned module.

# 2026-08-26, second hardware run: the root cause was never a register

The pack carrying the two split fixes answered its question and raised a
better one. `HCINTMSK` came back `0x59f`, so the split was correctly declined
and both fixes hold. The first SETUP still never completed, and this time the
log says why on its own.

## What the log proves without reference to any other driver

```text
arm  chan=0 stage=1 CHAR=00100008 TSIZ=60080008 DMA=c4525a00 NPTX=00080100
WD#1 chan=0 stage=1 HCINT=00000000/0000059f CHAR=80100008 DMA=c4525a08
                    NPTX=010700fe HFNUM=43ef017c
```

- `XACTERR` is unmasked in `0x59f` and never fires. `XACTERR` *is* the core's
  own response timeout: had it transmitted and heard nothing, it would raise
  it. Silence therefore means it never transmitted.
- `HCDMA` advanced by eight and `GNPTXSTS` shows two words in the FIFO with one
  request-queue entry consumed. The MAC accepted the work. The stall sits
  between the request queue and the wire — the transmitter.
- Connect detect, reset and port enable are all passive: a pull-up sensed on
  D+, SE0 driven blind, line state sampled afterwards. None of them needs a
  transmitter, which is why all three succeeded.
- The root port came up **full speed** with a LAN9514 — a high-speed hub —
  soldered in front of it. The chirp handshake needs a transmitter too.

Two independent witnesses, one conclusion: the analogue side is dead. The
frame counter advances, so the PHY clock is present. What is missing is power.

## Root cause

The ARM does not own the USB power domain on this SoC; the VideoCore firmware
does, and it powers it on request rather than by default. `dwc2emu68` never
asked. `usb2otg` does (`usb2otg_device.c:161-200`), and so does every other
BCM2708 driver in the tree — `sdcard`, `sdio`, `vc4gallium`. Our own
`<hardware/videocore.h>` has carried `VCPOWER_USBHCD` and a comment naming the
USB OTG driver as a caller since 2026-08-14. The constant was there; the call
was not.

`dwc2_platform_power_on()` now issues `VCTAG_SETPOWER` for `VCPOWER_USBHCD`
with `ON|WAIT` before the core is touched at all, and reads the answer instead
of assuming it. It sits in the platform layer beside the clock query, because
"obtain the hardware this driver needs" is that layer's job.

The state word is not symmetric and that is worth remembering: going in, bit 1
asks the firmware to hold its reply until the domain settles; coming back, the
same bit means the device does not exist.

Silence is treated differently from a bad answer. A firmware that does not
recognise the tag leaves the domain state unknown, which is not the same as
knowing it is off and is no reason to refuse to run — QEMU has no such domain
and every reason to carry on. Only an answer that says *off* or *absent* fails
the probe. The distinction matters because the failure this fixes is silent:
a driver that proceeds on a dead PHY does not stop working, it works
wrongly and expensively.

The property-channel boilerplate is now one helper shared with the clock
query. It checks the tag's response mark, which neither caller did before: a
tag the firmware does not understand comes back successfully with the request
untouched, so a caller reading the values without that check cannot tell an
answer from its own echo.

## Two theories excluded, so they are not tried again

Both were mine, both were wrong, and both looked reasonable.

- **`HFIR` hardcoded to 60000 at init**, before the speed is known. Recorded
  above as a real defect and it still is one, but it is not this failure.
  `usb2otg` writes exactly the same value at exactly the same point
  (`usb2otg_core.c:214`).
- **`HCFG.FSLSPCLKSEL` / `FSLSSUPP` programmed from the negotiated speed**,
  the way Linux does it. `usb2otg` clears `FSLSPCLKSEL` to 30/60 MHz and never
  sets `FSLSSUPP` either, so neither separates the two drivers. Worse, the
  Linux template is actively wrong here: the bare-metal Bellatrix host driver
  established on this board that 48 MHz is right only for a core wired to a
  dedicated full-speed transceiver, and that selecting it behind a UTMI+ PHY
  desynchronises the scheduler from `HFIR`.

The general lesson is the specific one that cost the time: `HCFG` and `HFIR`
decide *when* a transaction is scheduled. Neither can explain a port that
enables at the wrong speed. The evidence excluded them before any other
driver was opened; they were pursued because they were the visible difference
from Linux, which is not the same as being what the log pointed at.

If `FSLSSUPP` turns out to be needed later — the bare-metal driver did need it
for low-speed devices behind the hub — it belongs in host init, not in the
port-enable path: the core samples it when the port enables, so setting it
afterwards arrives too late.

## The power fix landed, and the bus came up high speed

`[DWC2/Emu68] USB power domain on`, and then everything the previous section
predicted would change, changed:

- `port reset complete HPRT=0000100f` — `PRTSPD=00`, **high speed**. The chirp
  handshake works once there is a transmitter to chirp with, which retires the
  "why does a LAN9514 negotiate full speed" question entirely.
- The LAN9514 enumerates: `SET_ADDRESS` to 2, device descriptor, both
  configuration descriptor reads (9 then 41 bytes — the hub's two alternate
  settings), `SET_CONFIGURATION`, hub descriptor. Twenty-nine control
  transfers complete, every one with `HCINT=0x23`.
- Downstream port power reaches the devices: the mouse LED lights.

`GUSBCFG` also changed under us, from `20001400` to `20402700`. Powering the
domain is what makes the firmware's real configuration visible; the earlier
value was read from a core that was not fully there.

Enumeration then dies at `error chan=0 stage=3 HCINT=00000082` — `XACTERR` plus
`CHHLTD` on a control status stage.

### Three defects, each proved by this log

**A transaction error was treated as final.** `XACTERR` went straight to
`finish(UHIOERR_HOSTERROR)` with no retry. USB specifies three attempts before
a transfer may be called failed, precisely because CRC errors, bit-stuff
errors, false EOPs and response timeouts all arrive this way and none of them
is an answer from the device. One glitch during a hub's descriptor read ended
the enumeration. Fixed: a three-strike budget, separate from the NAK budget
because a NAK is not an error, and with `STALL`, `AHBERR` and babble still
fatal — a refusal, a host fault and a device talking out of turn respectively,
none of which improves on a second attempt.

**`HFIR` was eight times too long.** The port is now high speed, where a
microframe is 125 µs — 7500 clocks of the 60 MHz PHY — and the driver was
still publishing the 60000 it hardcoded at init. This is the same line flagged
as a defect earlier and correctly excluded from the *previous* failure, where
the port was full speed and 60000 was right. It is load-bearing now. The core
checks whether a transaction fits in the time left in the frame before
starting it, so an interval eight times too long tells it there is eight times
more room than there is: most transactions still land, and the ones that start
near a real boundary get cut short and return as transaction errors. Fixed:
`dwc2_controller_speed()` derives the interval from the negotiated speed and
the PHY strap, and runs at the end of the port reset — the first moment there
is an answer and the last before anything is armed.

**`GINTSTS.OTGINT` is pending.** `GUSBCFG` carries `SRPCAP`, `HNPCAP` and
`TSDPS` from the firmware, so the OTG session protocol is live in a host-only
driver. **Left alone in this pack, deliberately**: it is a real defect, but
unlike the other two nothing in this log ties it to the failure, and the port
did not drop. It is the next thing to try if transaction errors survive the
retry budget.

The error line now names the command, address, endpoint and length, plus
`HPRT` and `GINTSTS`. `chan=0 stage=3` identified neither the device nor the
request, and the arm/irq trace it would have to be read against is capped at
32 lines — long spent by the time a failure this late in a boot happens.

## The frame interval was right, and the failure moved downstream

`HFIR=00001d4c` — 7500, the high-speed microframe. The retry budget worked as
designed and reported itself: `xacterr chan=0 stage=3 attempt 2 of 3`, then
`attempt 3 of 3`, then the failure. Three attempts is not a glitch; the fault
is deterministic.

The new error line is what earned its place:

```text
error chan=0 stage=3 HCINT=00000082 cmd=12 addr=3 ep=0 len=0
      HPRT=00001005 GINT=04000025
```

**`addr=3`.** The hub is address 2 and enumerated completely. The failure is a
device *behind* the hub, on a high-speed root port — which is the split
transaction path, running on real hardware for the first time. Neither
reference has anything to say about it: `usb2otg` inherited its split handling
untested, and the bare-metal driver never used splits at all. It ran its root
port at full speed and addressed low-speed devices with PRE packets, and left
"PRE+LS on a full-speed port" recorded as unsolved. Being at high speed with
real splits is a better place than either, and there is nothing to copy.

### The defect was a number invented here

```c
#define DWC2_SPLIT_PACE_INT     1     /* interrupt: one microframe   */
#define DWC2_SPLIT_PACE_CTRL    16    /* control:  sixteen           */
```

Only periodic splits are paced. An interrupt endpoint's split runs on a
microframe pipeline — start-split in one microframe, complete-split in the
next — so one is right. Control and bulk are non-periodic and have no pipeline
at all: the host asks for the result immediately and keeps asking, and NYET is
what paces it. That loop already existed, bounded by `DWC2_SPLIT_NYET_LIMIT`.

Sixteen microframes is two milliseconds of silence, and a transaction
translator ages its buffers. Coming back that late finds the transaction
discarded and gets ERR, which the core reports as a transaction error —
indistinguishable, from the driver's side, from a bad cable.

It also explains why the transfer died where it did rather than at the start.
A `SETUP` survives the wait because all the translator has to keep is a
handshake. The status stage of the same transfer does not, because there the
translator is holding a data packet nobody came to collect.

`DWC2_SPLIT_PACE_CTRL` is now 0. `DWC2_SPLIT_NYET_LIMIT` rises from 8 to 24 as
a consequence, not as an independent guess: with the complete-split issued
immediately, NYET is now the normal answer rather than a rarity, and the
budget has to be read as a time limit — one complete-split per microframe, so
24 is three full-speed frames. A translator shared by several devices can
legitimately defer one of them across a couple of frames.

### On the OTG session bits, since the question was asked

`GINTSTS.OTGINT` is still pending (`GINT=04000025`) and `GUSBCFG` still carries
`SRPCAP`, `HNPCAP` and `TSDPS`. Checking both references:

- `usb2otg` does not clear them. It forces host mode and leaves the rest of
  the firmware's `GUSBCFG` alone, so it offers no evidence either way.
- The bare-metal driver does clear them, and records why: with them active the
  first low-speed transaction triggers the session state machine and **the
  root port is disabled mid-enumeration**.

That symptom is not ours. `HPRT=00001005` still has `PRTENA` set at the moment
of failure and the port never dropped. So the change is worth making — session
request and host negotiation have no role in a host-only driver, and an
interrupt asserted forever that nobody handles is not a state to keep — but it
is a correctness cleanup here, not a candidate cause, and bundling it with the
split fix would make neither result readable. It goes in the next pack.

## Correction: the pacing constant was not invented here, and was already ruled out

The pacing change made no difference — the failure repeated byte for byte,
same address, same stage, same `HCINT`. Checking the reference properly, which
should have come first, shows why.

`usb2otg` has a real split engine, not an inherited stub: an `hc_SplitState`
machine, delayed complete-split re-arms, and a set of BCM-specific recoveries.
Its history is upstream AROS's own — `usb2otg: split-transfer, channel and
bulk fixes`, `usb2otg: reset the split engine when a split transfer ends in
STALL` — so it is maintained code for this exact SoC, by the same author as
Emu68. Saying it was "inherited untested" was wrong and was said without
looking.

It paces exactly as this driver did: one microframe for interrupt, immediate
for bulk, `USB2OTG_CTRL_SPLIT_PACE_UFRAMES` for control. That constant is 16,
and its comment records that the value was already suspected and already
cleared:

> This is consistency, not a fix. The 8 was suspected of poisoning the split
> engine on a tester's low-speed keyboard; forcing a NAK on hardware disproved
> it — the storm follows a NAKed CSPLIT at 16 uframes exactly as it did at 8.
> The real cause was the missing engine reset, see `usb2otg_exorcise_channel()`.

So the 16 came from there, it had been tested on this hardware, and the
argument used against it — that nothing in the specification justifies it —
was true and irrelevant. Both constants are restored. Diverging from a value
that has actually run on this board needs a better reason than symmetry.

The engine reset that comment points at is already ported:
`reset_halted_channel()` is a functional equivalent of
`usb2otg_exorcise_channel()` — forced halt cycle, clear `HCINT`, zero `HCSPLT`,
zero `HCCHAR` — and it is on the transaction-error path. What is still missing
from that area is the quarantine around it (`hu_DeadChannels`, the blackout
states), not the reset itself.

## What this pack changes

**`HCCHAR.LSPDDEV` was never set.** The bit did not exist as a constant and
`UHFF_LOWSPEED` reached `dwc2emu68_unit.c` and stopped there. It describes the
device at the far end, not the bus the host is on, so it is required for a
low-speed device whether it sits on a full-speed bus directly or behind a
translator. `usb2otg` sets its equivalent from the same flag in the
corresponding place (`usb2otg_schedule.c:568`). Missing code, not a theory.

**The diagnostic budget stopped before the interesting part.** The arm and
completion trace was capped at 32 lines, which covered the root hub and the
hub behind it and ran out exactly at the first real device — so three rounds
of hardware testing produced a failure whose transfers were never printed, and
three rounds of inference stood in for them. A budget that stops early is
worse than none, because it looks like data. It is now 160, the arm line
carries `HCSPLT` instead of a DMA address that never varies, and the split
sequencer traces its own transitions: whether a start-split was accepted, what
the complete-split answered, how many NYETs.

The next log answers, without inference, whether the failing transfer is a
split at all.

## The trace paid for itself immediately: the root hub never reported a speed

With the budget raised and `HCSPLT` in the arm line, the log answers in one
column what three rounds of inference could not.

**`SPLT=00000000` on every transfer, including the ones that failed.** No
transfer was ever a split. Every hypothesis about the split sequencer — the
pacing, the NYET budget, the start-split handshake — was about code that never
ran.

What did happen:

- `addr=2`, the LAN9514 hub, enumerates completely.
- `addr=3` also enumerates completely — `CHAR=00d00040`, so MaxPktSize 64 and
  no `LSPDDEV`. That is the LAN9514's built-in Ethernet function, which is
  high speed like the hub itself and needs no translator.
- `addr=0` with `CHAR=00100008` — MaxPktSize 8, a new device's first
  descriptor read — fails with `XACTERR`, three attempts, forever. That is the
  mouse, and it is the first device on the bus that is *not* high speed.

A full or low-speed device behind a hub, addressed directly from a high-speed
host with no split, cannot be reached: the hub does not translate what it was
not asked to translate. Nothing answers, and the core reports a transaction
error.

### Why no split was ever requested

The chain is entirely in Poseidon and is short:

```c
/* hub.class.c:1296 */
if (nch->nch_IsUSB20)          /* a low/full speed device on a 2.0 hub */
    needssplit = TRUE;
```

`nch_IsUSB20` is the hub's own `DA_IsHighspeed`, and `DA_IsHighspeed` is set
from `UPSF_PORT_HIGH_SPEED` in the port status its *parent* hub reports. For
the LAN9514 the parent is this driver's root hub — and its `GetPortStatus`
built `UPSF_PORT_CONNECTION`, `_ENABLE`, `_POWER` and `_RESET` and stopped
there. It never reported a speed at all.

So: the root port says nothing, the LAN9514 is not marked high speed, its hub
instance has `nch_IsUSB20` false, nothing below it is marked as needing a
split, `UHFF_SPLITTRANS` is never set, and `HCSPLT` stays zero. The first
device that actually needed the translator is the first one that fails.

The same omission explains why the previous pack could not have worked:
`UPSF_PORT_LOW_SPEED` was equally absent, so Poseidon never set
`DA_IsLowspeed`, so `UHFF_LOWSPEED` never reached the driver, so
`HCCHAR.LSPDDEV` had nothing to fire on. Both fixes are the same root cause
seen from two ends — the driver never told Poseidon how fast the port was
running.

Full speed is the absence of both bits, which is how a hub reports it rather
than an omission; that is the one case the old code got right by accident.

### On the diagnostic budget

This is the finding the 32-line cap had been hiding since the first hardware
run. `SPLT` was not in the arm line, and the arm lines stopped before the
first device that mattered. Three packs went out reasoning about a split
engine that was never entered, and two of them changed code that could not
have been executing. The trace was the cheapest thing in this whole sequence
and should have come first.

## Splits are armed, and the complete-split waits on a tick that was off

Reporting the port speed worked, all the way through Poseidon:

```text
arm   chan=0 stage=1 CHAR=00100008 TSIZ=60080008 SPLT=8000c102
irq   #154 chan=0 stage=1 HCINT=00000022 HCTSIZ=60080008
SPLIT start chan=0 stage=1 addr=0 hub=2.2 HCINT=00000022 -> complete-split
WD    chan=0 timed out stage=1 CHAR=00100008
```

`SPLT=8000c102` is `SPLTENA | XACTPOS_ALL | HUBADDR(2) | PRTADDR(2)` — the
first split this driver has ever armed. `HCINT=0x22` is `CHHLTD | ACK` with no
`XFERCOMP`, which is a translator saying it took the transaction, and the
sequencer reads it correctly. Everything up to that point is right.

Then nothing. And between the two lines there is no `SOF` at all.

`split_complete()` parks the channel with `split_delay` microframes to run
down, and the countdown lives in `dwc2_transfer_sof()`. That handler only runs
when the SOF interrupt reaches the unit task, and the top half was throwing
SOF away unless a *periodic transfer* had come due:

```c
if (!unit->periodic_waiting || ((now - due) & 0x7ff) >= 0x400)
    active &= ~DWC2_GINTSTS_SOF;
```

Two things wait on that tick and only one was counted. During enumeration
nothing periodic is queued, so no SOF is delivered, so the countdown never
advances, so the complete-split is never issued, so every device behind a hub
dies in the watchdog with its channel still armed. The suppression is worth
keeping — delivering every SOF is a storm nobody reads — but it now also
yields to `unit->split_pacing`, a channel bitmap set when a complete-split is
parked and cleared when it goes out.

This also retires the pacing experiment properly. Setting
`DWC2_SPLIT_PACE_CTRL` to 0 two packs ago changed nothing because no split was
ever armed in that build; the test was run in a world where the code under
test could not execute. The 16 has still never actually been exercised here.
It will be now, and if it turns out to be wrong this time there will be a log
line showing it rather than an argument about the specification.

## The interrupt was registered correctly and used at the wrong altitude

Asked directly whether the IRQ was being used properly, the registration
checks out: `IRQ_VC_USB` is `GPUIRQ0_BASE + 9`, the same symbol `usb2otg`
uses, and the `GAHBCFG.GLBLINTRMSK` gate pairs correctly — the top half closes
it only on the path that also calls `Cause()`, and `dwc2_controller_drain_irq()`
reopens it. Channel interrupts demonstrably arrive.

The architecture around it is what was wrong. This driver's top half does no
work: it records which channels fired and wakes the unit task, which does
everything. That is the right shape for a transfer that completes in one step
and the wrong shape for a split, which has a deadline.

The watchdog runs at 10 ms, and the log shows `[WD]` lines for transfers whose
channel had already halted with `HCINT` consumed — the unit task round trip is
of that order. Worse, on a serial-traced build the `irq #N` and `[SPLIT] start`
lines sit *inside* the split's timing window: one line at 115200 baud is
roughly 9 ms. The instrumentation added to observe the split was long enough
to guarantee it failed.

So `split_complete()` began counting 16 microframes at a point where the
translator had already discarded the transaction, and the answer to the
complete-split was ERR — reported as a transaction error, and for several
rounds mistaken for one.

The two steps that have a deadline now run in the interrupt half, with no
allocation, no queue and nothing that can print: accepting the start-split
(`ACK` and only `ACK`), and issuing the complete-split when its pace has run
down, counted on the SOF tick in the same handler. `dwc2_transfer_split_irq()`
returns TRUE when it has taken the channel's interrupt entirely, and the task
is not woken at all. Only outcomes reach the task — the data, a NAK that means
starting over, or a translator that has stopped answering.

This is also what makes the 16-microframe pace mean 2 milliseconds rather than
"however long a task takes", which is the form `usb2otg` has always had: its
`hu_DelayedChannel[]` is ticked from its interrupt handler, and its comment
about serial output moving split timing is describing this exact hazard from
the other side.


## The design target is now written down

Six rounds of hardware testing produced six fixes, each justified by the log
line above it and none of them by a picture of where the driver is going. Two
of them were "do what `usb2otg` does", which is convergence on the thing being
replaced.

`AI_context/dwc2emu68.md` states what better means here as five checkable
invariants, records which ones the code currently holds, and orders what is
left. The open structural defect it names is the dispatch order in
`channel_irq()`: a split's STALL and transaction error are handled by generic
code that runs before the split sequencer, so the engine scrub added to
`channel_release()` is a reminder rather than a guarantee -- the same shape of
guarantee upstream had when it shipped the bug this port copied the fix for.

# 2026-08-26: a low-speed mouse works, through a translator

```text
[DWC2/Emu68] interrupt data #1 chan=0 addr=5 ep=1 bytes=4 data=00 ff 00 00 00 00
[DWC2/Emu68] interrupt data #2 chan=0 addr=5 ep=1 bytes=4 data=00 f5 fd 00 00 00
```

Four-byte HID reports carrying X and Y deltas, from a low-speed device on port
4 of a high-speed hub, over split transactions. The whole chain works: the
VideoCore power domain, the high-speed chirp, the port speed reported to
Poseidon, `HCCHAR.LSPDDEV`, `HCSPLT` addressed at the translator, the split
sequenced in the interrupt half, and the multi-packet continuation.

The continuation is visible doing exactly what it was written for:

```text
arm stage=2 TSIZ=40180012    18 bytes, three packets
irq stage=2 HCTSIZ=0010000a  ten left, two packets
arm stage=2 TSIZ=0010000a
irq stage=2 HCTSIZ=40080002  two left, one packet
arm stage=2 TSIZ=40080002
irq stage=2 HCTSIZ=00000000
arm stage=3                  status
```

18 → 10 → 2 → 0, and a 36-byte descriptor as 36 → 28 → 20 → 12 → 4 → 0. The
`PID` field alternates on its own across those armings — `40180012` is DATA1,
`0010000a` DATA0, `40080002` DATA1 — which confirms that taking the toggle
from `HCTSIZ` for a continuation is right.

## The next defect, named but not touched

```text
[DWC2/Emu68:WD] 512 recoveries so far (chan=0 stage=4 HCINT=00000012)
```

A recovery is the watchdog finding a channel in a terminal state whose
interrupt never reached the unit task. Five hundred of them, all on the
interrupt endpoint, with `HCINT` reading `0x23` completed, `0x12` NAK or
`0x42` NYET.

So the mouse works, but the periodic path is being driven by the 10 ms
watchdog rather than by channel interrupts. That is also what the `[SCHED] NAK`
frame numbers were saying earlier — gaps of 40 to 140 frames on an endpoint
whose interval is 2.

It is a robustness and latency defect rather than a functional one, and the
cause is not yet known: `HAINTMSK` and `GINTMSK.HCHINT` are both re-armed by
`arm()`, and the interrupt half does reach the task for control transfers on
the same channel. It goes on the list in `AI_context/dwc2emu68.md` rather than
into a guess.
