---
id: ISSUE-0079
title: "Demo Reel 3 reaches Paula's audio and then jumps to PC=0"
status: open
priority: high
type: defect
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-30
tags:
  - rigel
  - audio
  - demoreel
  - m68k
blockers: []
related_files:
  - aros/arch/m68k-emu68/boot/mmakefile.src
  - external/aros/arch/m68k-amiga/devs/audio/
---

# Where this got to

Demo Reel 3's player, `Slish`, opens five things: `intuition.library`,
`dos.library`, `graphics.library`, `timer.device` and `audio.device`. Four
were present; `audio.device` answered "object not found", because this target
built neither of AROS's two implementations -- the AHI-backed one is excluded
for m68k on purpose, and Paula's own lives under `arch/m68k-amiga`.

`3564aa8` links Paula's `audio.device` into the ROM, the way `cia_resource`
already was. **It works**, and this is the first time anything has driven
Rigel's audio:

```text
[InitResident] audio.device: MakeLibrary 0 ms, calling init @ 0x0730ef2c
[rigel] event=audio_per_write f0=00000000 f1=00000064 f2=00000064
[rigel] event=audio_per_write f0=00000001 f1=00000064 f2=00000064
[rigel] event=audio_per_write f0=00000002 f1=00000064 f2=00000064
[rigel] event=audio_per_write f0=00000003 f1=00000064 f2=00000064
...  f1=00000165 on all four, then back to 00000064
```

All four channels, periods 100 and 357. The demo is programming Paula.

# And then the guest dies

```text
[JIT] Back from translated code.
[JIT]     A6 = 0x00000000
[JIT]     PC = 0x00000000    SR = T0|S.|IPM0|.....
[BELLATRIX:LIVE] pc=00000000 sr=2000 arm=1 ipl=0
```

`A6 = 0` with `PC = 0` is a library call through a null base -- `jsr
-offset(a6)` with nothing in A6. The m68k stops and Emu68 dumps the context.

D0 = 0x61, A5 = 0x07301ae4, A1 = 0x073066e0, A2 = A3 = a structure near
0x0203b81c/0x074d94c0. The device itself initialised at 0x0730ef2c, so A1 and
A5 are inside it.

# What to look at first

- Which base is null. Either `Slish` calls `audio.device` with a base it never
  received -- an `OpenDevice` whose failure it does not check -- or the device
  reaches a library it opened and did not get. Paula's `audio.device` needs
  little, but it is a ROM module here for the first time and its init runs at
  `residentpri -120`, very late.
- Whether the interrupts it wants exist. `audio_hardware.c` calls
  `SetIntVector(INTB_AUD0 + ch)`, and this port answers every m68k autovector
  with one trampoline that asks the ARM interrupt controller what is pending
  (`platform/platform.c`). **A Paula interrupt reaches the CPU and finds
  nobody home.** A sample can still be played -- Paula's DMA is autonomous
  once armed -- but nothing chains buffers, and a player that waits for its
  channel interrupt waits for ever. That is the chipset interrupt domain of
  `docs/New_emu68.md` sections 3 and 14, and it is the next real piece of
  work here.

# Note on the driver underneath

This run was made with `usb2otg` on the card (ISSUE-0078). That is unrelated
to the crash -- it is simply the configuration in which the machine boots far
enough to run the demo at all.


# 2026-08-30: Paula's interrupts are delivered

The gap named above is closed. `Platform_Autovector()` now dispatches Paula's
own interrupts as well as the ARM controller's, in the shape of
`arch/m68k-amiga/kernel/amiga_irq.c` with its seven per-level handlers merged
into one pass -- this trampoline is shared across all seven levels and does
not know which it was entered for, and the SR mask has already ordered them
by the time it runs.

```c
ena  = *INTENAR;  if (!(ena & INTF_INTEN)) return;
mask = ena & *INTREQR;  if (!mask) return;
*INTREQ = mask & ~INTF_SOFTINT;          /* ack before dispatch */
for (bit = INTB_EXTER; bit >= 0; bit--)
    if (mask & (1 << bit)) core_Cause(bit, mask);
```

Two properties worth keeping:

- **Acknowledge before dispatching.** A bit with no server installed is then
  cleared rather than re-asserting for ever. This machine has already lost a
  day to one interrupt that would not go quiet (ISSUE-0078); this ordering is
  what keeps a missing server from becoming the next one.
- **One register read on the idle path.** `INTENAR` is read first and the
  function returns on `!INTF_INTEN`, because that read is an MMIO fault to
  Rigel and most of a boot has nothing armed.

`rigel_get_ipl()` already resolves the level from all of INTENA/INTREQ, so no
change was needed on the publishing side -- the audio bits were always being
turned into an IPL, and there was simply nothing at the other end.

## What to watch on the next boot

- `audio_per_write` followed by the channels actually advancing, rather than
  the same period being reprogrammed.
- `[BELLATRIX:IRQ] stuck level N asked M times` from `src/amiga/irq.c`. That
  line exists for exactly this: a chipset level raised and never cleared. It
  has never fired; if it starts, a server is not clearing its own INTREQ bit
  and this is the first change that could cause that.
- Whether the null-base crash above survives. It may have been the missing
  chain all along, or it may be independent.


## The cost of asking the chipset, and the cheaper way

Delivering Paula's interrupts put an `INTENAR` read on every entry to the
autovector trampoline, and on this machine that read is an MMIO fault into
Rigel. The platform path is not slow by accident -- `usb2otg` keeps
start-of-frame unmasked, 8000 entries a second -- so the machine stopped
keeping up and IRQ 9 backed up into a storm:

```text
[intc] irq 9 dispatched 65536 times
[intc] still pending after 4 rounds: arm=00000000 gpu0=00000200 gpu1=00000000
[BELLATRIX:LIVE] pc=3061f950   ->  Platform_Autovector +0x42
```

The gate is `JITCTRL2` bit 29, which is `INTF.ARM` on the read (Emu68's
`M68k_LINE4.c`, `bfi(reg, tmp, 29, 1)`) and the same bit the acknowledge
writes. A MOVEC, no bus cycle. A platform interrupt no longer asks the
chipset anything; if the chipset's level is also up it re-enters, because it
is a level and re-entering is what the autovector is for.

**Note for whoever reads that code:** the comment there said "bit 0 is what
that register exposes", which is about the value stored being 1 rather than a
level -- not about the bit position. Written as `& 1` the gate never fires and
the regression stays. The read path is the authority.

### If the remaining fault ever matters

It should not: with the gate, `INTENAR` is read only on chipset interrupts --
VERTB at 50 Hz plus one per audio buffer -- rather than 8000 times a second.
But the fault can be removed entirely, and this machine can do better than a
real Amiga here, because the answer is already known on the ARM side:
`rigel_get_ipl()` resolves the level from `INTENA & INTREQ` before the CPU
ever sees it.

Two ways to hand the guest that resolved mask without a bus cycle:

1. **Extend JITCTRL2.** The read already assembles INTF bits into the value;
   putting the 16-bit pending mask in the upper half costs the guest nothing
   it is not already paying (one MOVEC). Needs an Emu68 patch.
2. **A word in the descriptor page.** Bellatrix already publishes a page the
   guest reads with plain loads (`src/amiga/frame.h`, the Denise descriptor at
   `$01200000`); an interrupt word there is a version bump and no new
   mechanism.

The acknowledge still has to reach Rigel -- one write to `INTREQ` per
dispatch -- and that is unavoidable and correct.


## 2026-08-30, later: the crash is deterministic, and two hypotheses are out

With Paula's interrupts delivered and gated, the machine boots to Wanderer
(`[BootUI] [00:32.827] display takeover`) and the demo still dies in exactly
the same place. Two runs, register for register:

```text
D0 = 0x00000061   A0 = 0x000060ca   A3 = A4 = 0x0203b81c
A6 = 0x00000000   A7 = 0x02020608   PC = 0x00000000
```

Only the heap-dependent values move between runs (`A1`, `A5`, `D3`). `A6 = 0`
with `PC = 0` is a call through a library or device base that is null.

**Ruled out: no chip memory.** `boot.c` creates a second `MemHeader`, "Chip
Memory" at `AMIGA_CHIP_RAM_ALLOC_BASE` with `MEMF_CHIP | MEMF_24BITDMA`, and
enqueues it on `SysBase->MemList` whenever `bellatrix.rigel=1`. The machine map
confirms the region: `$00000000-$001fffff DIRECT Chip RAM`.
`AllocMem(MEMF_CHIP)` works, so `AudioBase->zerosample` is not the null.

**Ruled out: the Paula interrupts were the missing chain.** They are delivered
now and the crash is unchanged, so it is not a player waiting on a channel
interrupt that never came.

**Standing hypothesis.** `Slish` calls `OpenDevice("audio.device", ...)`, does
not check the result, and then calls through `io_Device`. That fits `A6 = 0`
exactly, and it fits the timing: the crash is the first thing after the
device's own init and the period writes. `A0 = 0x000060ca` is a chip-RAM
pointer, which is what an audio open's key array or sample would be.

## The next step, and only this one

Find the caller. `A7 = 0x02020608` and the `JSR` pushed a return address
there, so the top few longwords of the stack name the code that made the call
-- and with the ELF's load base at `0x30600000` that address resolves to a
symbol the same way the liveness PC does.

The dump belongs where the crash is already reported: Emu68 prints the whole
context at `[JIT] Back from translated code` and has the mapping in hand.
Reading guest memory from the chipset core instead is the wrong place --
`host_phys` is physical and the guest's is virtual, and that trap has been
paid for once already.

Do not add another probe before this one. Today four investigations ended in
the wrong place because an instrument was built in a hurry and its silence was
read as evidence.


### The dump is in (`0cb9d4f`, `patches/emu68/0023`)

`start.c` prints eight longwords from A7 beside the register dump it already
produced. The return address the JSR pushed is among them: in the
`0x306xxxxx` range it belongs to the AROS ELF and resolves against
`0x30600000`; in the heap (`0x02xxxxxx`, `0x07xxxxxx`) it belongs to `Slish`
itself or to a loaded module, and the question becomes which.

### On using the Rigel harness as an oracle

Worth doing, and not for this question. `Slish` is not a bootblock -- it is a
Workbench program that opens intuition, dos and graphics -- so the harness
would need a Kickstart and an AmigaOS above it to run at all, and having no
Kickstart is this port's premise. The harness runs the chipset, not the
operating system.

Where it does earn its keep is the step after: once the demo runs, comparing
frame by frame what Denise and Paula should be doing against what they do
here. That is exactly an oracle, and it is how ISSUE-0071 already treats bus
timing.
