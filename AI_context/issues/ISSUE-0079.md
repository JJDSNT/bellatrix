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
