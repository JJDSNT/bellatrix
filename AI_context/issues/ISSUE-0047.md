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
