---
id: ISSUE-0090
title: "Evaluate EMU68AGA Bare Metal: adapt it for AROS, and learn from it for Bellatrix"
status: backlog
priority: medium
type: research
owner: unassigned
created_at: 2026-10-25
updated_at: 2026-10-25
tags:
  - emu68
  - chipset
  - rigel
  - usb
  - hdmi
  - external-project
blockers:
related_files:
  - AI_context/issues/ISSUE-0068.md
---

# Summary

<https://github.com/futureretrofusion/EMU68AGA-Bare-Metal> (Future Retro
Fusion, created 2026-10-14, release v0.10.40a on 2026-10-23) is a standalone
Raspberry Pi 3 Amiga built from Emu68 and the AGA-PiStorm virtual chipset:
no Amiga board and no PiStorm adapter. HDMI carries video and sound, USB the
input, and the SD card the storage. That is the same machine Bellatrix is
building, reached from the other side: they start from a working AGA
chipset and boot Kickstart/AmigaOS on it, where Bellatrix starts from AROS
with no chipset and adds one (Rigel, [ISSUE-0068](ISSUE-0068.md)).

Requested by the project owner on 2026-10-25, for later. Two goals:

1. **Modify it so it can be used with AROS**: AROS as the operating system
   on their machine, in place of a Kickstart ROM and AmigaOS.
2. **Bring what it learned to Bellatrix**: anything they have
   hardware-verified on a Pi 3 that we have not.

**The main lessons sought are two (owner, 2026-10-25): how they integrate
the chipset with Emu68, and how they support WHDLoad.** Of the two, **how
they synchronise the chipset with the CPU comes first.** Everything else in
this file is secondary. For WHDLoad the owner's own direction is Saiph
(route 1 below), not moving the drivers to the ARM side.

Nothing has been evaluated yet. This file records what a first look found
and what the evaluation has to answer.

# What a first look found

- `machine/` holds the code: `Emu68`, `core`, `arm`, `package`, plus
  `MERGE_PROVENANCE.txt`, which records a three-way merge of AGA-PiStorm
  1.113 (base), upstream 1.114 (core/audio/HDMI/image features) and FRF's
  own 1.0.0 (HAM6/RGBPORT allocation, single/double buffering, Copper
  handoff, P96/ASL HDMI ownership).
- Their feature list, all in local development, not all public:
  - Emu68 JIT with virtual AGA (Alice/Lisa/Paula), Copper, blitter, CIAs;
  - HDMI video and Paula audio over HDMI;
  - 2 MiB Chip, 512 KiB Ranger and Zorro III Fast RAM, an Emu68
    ROM-board/autoconfig path;
  - USB keyboard/mouse/joypad. The USB HID code is **not in public `main`**
    for licensing reasons (`KNOWN_ISSUES.md`, `USB-LICENSING.md`); it is
    built on USPi, and v0.10.40a is described as a hardware-verified Pi 3
    USB/HID endian correction.
- Open on their side: WHDLoad compatibility, reset semantics (Amiga reset
  against a Pi reboot), RTG/P96 screen modes.

# What the evaluation has to answer

## Licensing, first

The repository mixes several licences: GPL-2.0-or-later for FRF-original
material (`LICENSE-FRF.md`), MPL-2.0 for Emu68 (`machine/MPL-2.0.txt`), and
whatever AGA-PiStorm, PiStorm and UAE/WinUAE carry per file. USPi is not
redistributed by them at all. AROS is under the APL, and Bellatrix's own
choice has to be checked. Every piece we consider copying needs its
file-level licence checked against where it would land, before anyone reads
it with an eye to porting. Check provenance with `git log` and the remote
before judging any code.

## For goal 1 (AROS on their machine)

- What does their boot path expect: a Kickstart ROM image loaded by Emu68,
  an autoconfig ROM board? Can an AROS m68k ROM (the `amiga-m68k` target,
  not ours) take its place, and what breaks?
- Which of their virtual chips does AROS's Amiga HIDDs (`amigavideo`,
  Paula audio, CIA timers) exercise that AmigaOS does not, and the other
  way round?

## For goal 2 (learning for Bellatrix)

### Chipset integration (primary)

How their virtual AGA sits beside Emu68, compared piece by piece with
Rigel's ([ISSUE-0068](ISSUE-0068.md)):

- **The bus.** How a guest access to `$DFFxxx`, CIA space and Chip RAM
  reaches the chipset: page faults, a JIT hook, a shared map? Rigel pays a
  lock hold per CPU access to the classic domain, and under QEMU that cost
  is what forced `bellatrix.chipdiv` (ISSUE-0075).
- **Time.** What drives chipset time -- wall clock, CPU cycles, frame
  boundaries -- and how they keep up on a Pi 3. Our findings: wall-clock
  time saturates a core that cannot keep up, and cycle-exact throttles the
  whole machine to Amiga speed (CLAUDE.md, *The chipset is a boot
  argument*).
- **Cores.** Which core runs what: the JIT, the chipset, video and audio
  output. Rigel takes cores 2 and 3.
- **Interrupts.** How chipset interrupts reach the 68k IPL, against our two
  domains (platform IRQs on level 6 through `INTF.ARM`, chipset IRQs owned
  by Rigel; CLAUDE.md, *Boot chain and the IRQ bridge*).
- **Output.** How Denise/Lisa output becomes an HDMI frame, and how Paula
  becomes HDMI audio, at what latency.

### WHDLoad (primary)

WHDLoad has never been tried on Bellatrix; two possible routes are below.
EMU68AGA's own `KNOWN_ISSUES.md` lists WHDLoad compatibility as still under
investigation, so what they have is a working state with known gaps, not a
solved problem. What to learn:

- **What WHDLoad needs from the machine, and what they had to change in
  Emu68 for it**: CPU-control instructions (`MOVEC` of CACR/VBR, cache
  flushes), the absence of a 68k MMU, taking the system over and handing it
  back (QuitKey), and reset.
- **Kickstart emulation.** WHDLoad slaves load original Kickstart images
  (kickemu) rather than running on the resident OS. What does that mean for
  an AROS system: does WHDLoad run with AROS as the host OS and a Kickstart
  image only for the slave, and is that legal and practical for us?
- **Which titles work and which fail**, by failure kind -- timing, blitter,
  Copper, CPU -- so their list can be a test list for Rigel.
- **What a WHDLoad game demands of the chipset** that the AROS desktop
  never does: direct hardware banging, exact Copper and blitter timing,
  raster-synchronised effects. This is the strongest test Rigel could get.

### Route 1, our own: Saiph

One way WHDLoad could be supported here is written down in
<https://github.com/JJDSNT/saiph> (`README.md`). Recorded as a possibility,
not a decision:

- WHDLoad takes the machine over and switches the OS off. Doing that on
  the real AROS/Emu68 context would stop the services that make the Pi a
  computer (video, audio, USB, storage).
- So the takeover software gets a separate 68k context instead: **Musashi
  as its CPU, Rigel as its chipset**, while AROS on Emu68 stays alive.

### What their source shows (read 2026-10-25, commit `5d9f5ce4ba`)

**WHDLoad does not have Bellatrix's problem there, because their drivers
are ARM code.** HDMI video (`machine/arm/aga_hdmi.c`, `aga_hvs.c`), HDMI
audio (`aga_audio.c`) and USB input (`machine/Emu68/src/frf_usb/`, on
USPi) all run inside Emu68, outside the 68k. When WHDLoad shuts AmigaOS
down, nothing the machine needs to stay a computer goes with it, so they
run WHDLoad on the same CPU context, Emu68's JIT, with no second machine.
On Bellatrix those services are AROS drivers inside the 68k, which is why
a takeover kills them and why Saiph was proposed. That is the real
difference, and it is architectural, not a matter of their emulation
being better.

So there are two routes, not one:

1. **Saiph**: a second 68k context for the takeover, AROS kept alive.
2. **Their route**: the services a game needs -- presenting the chipset's
   picture, Paula to HDMI, keyboard/mouse/joypad -- live on the ARM side,
   and the game takes over the one 68k. Rigel already presents its own
   output from the host side, so part of this exists here; input and
   audio would have to follow.

How the rest of it works:

- **Chipset placement.** Core 0 runs the JIT; core 3 runs
  `aga_chipset_main()`. Core 1 only drives the physical RGB port, which
  exists on a PiStorm (a real Amiga's video out) and idles on the standalone
  build, so the standalone machine uses two cores. A 68k access to Chip RAM
  or a custom register enters `aga_trap_read/write` from Emu68's data-abort
  path (`vectors.c`), under one lock shared with core 3 (WFE/SEV spinlock;
  the missing SEV hung the machine once, and an AGA->CIA lock order gave an
  ABBA deadlock at WHDLoad takeover).
- **Time.** The chipset core paces itself on the ARM generic timer
  (`cntvct`) at the PAL or NTSC colour clock, and the CPU free-runs: the
  "chipset on its own core" shape of [ISSUE-0074](ISSUE-0074.md), answer 1
  of ISSUE-0068. Their notes record the same traps ours did -- catch-up
  that discards owed time, and a 0.92% PAL/NTSC pacing error that showed up
  as audio drift.
- **Chip RAM on a Pi 3.** Serving it from the abort handler cost 10.7 ms of
  each 20 ms frame (96k accesses, measured 2026-09-14), starving both the
  game and the chipset core. A Pi 3 now maps Chip RAM with the ARM MMU, so
  the 68k's accesses are plain stores; the Pi 4 keeps the abort path.
  Bellatrix already does this: Chip RAM is `MACHINE_REGION_DIRECT`, cached
  (`src/machine/machine.c`), and only the CIA, custom and RTC apertures
  decode through Rigel per access. Their measurement is confirmation that
  the choice matters, not a gap here.
- **When "game mode" starts.** `agaboot SANDBOX AUTO` runs from WHDLoad's
  startup script while AmigaOS is still up, because WHDLoad then loads the
  slave, the game and, for KickEmu titles, a Kickstart image through the
  OS filesystem. Game mode is only armed there; it engages at the first
  INTENA write that clears EXTER, which is WHDLoad shutting the OS down.
  From then on level 6 stays as the game set it: a game that installs only
  level 3 (ClockwiserAGA) was killed by a level 6 it never asked for.
- **Virtual CIAs** (`frf_vcia_*` in `vectors.c`). At takeover, CIA-B's
  interrupt enables are cleared, as if `$7F` were written to its ICR, while
  its timers keep counting. `aga.keepciab` and `aga.cia=N` exist because
  some games need the opposite.
- **Licence.** `aga_glue.c` is GPL-2.0-only; Emu68 is MPL-2.0. Ideas and
  measurements transfer freely; code does not without a check.

**Core budget against Saiph.** Bellatrix today: core 0 runs Emu68's JIT
and takes the physical IRQs, core 2 runs Rigel (`patches/emu68/0020`),
core 3 drains the console (the same patch), and **core 1 is free**. A
Saiph Musashi would fit on core 1 without taking anything from the
chipset. Their layout, two busy cores, says the same shape fits a Pi 3.

Questions this leaves for the evaluation:

- **Synchronisation, first.** What exactly the lock protects and for how
  long; how long core 3 holds it per chunk of colour clocks, against the
  ~1 ms budget of `amiga_clock_run_on_core()`; how a CPU read of a
  register that depends on the beam or on DMA progress (VPOSR, INTREQR,
  DMACONR, the blitter's busy bit) gets a value consistent with the
  chipset's time; how blitter completion is timed (their note: against
  the real clock, not colour clocks); and how the catch-up after a stall
  is bounded without discarding owed time.

- For the titles that fail there, is the cause CPU timing or the chipset?
  Only the first would argue for Musashi.
- Which of their WHDLoad-specific behaviours (game-mode trigger, CIA-B
  silencing, KickEmu loading through the OS) would Rigel need, whichever
  route is taken?

### Secondary

- **HDMI audio**: they ship Paula over HDMI; we validated the output path
  but not the production feed rate (ISSUE-0055).
- **USB on a Pi 3**: their endian correction against our dwc2emu68 and
  usb2otg drivers; their HID problems may be ours.
- **SD card**: their partition layout (FAT32 boot plus Emu68 `0x76`
  partitions) and their SD read rate on hardware, if they have one. Our
  own rate has not been measured on a Pi; under QEMU the emulator dominates
  (2026-10-25: 95% of device-level read time is inside the SDCMD write that
  QEMU executes synchronously).
- **Emu68 itself**: which upstream Emu68 revision they carry, and what they
  changed in it, against our `patches/emu68/` series.

# The plan this opens (owner, 2026-10-25)

In this order:

1. **Bring their chipset in whole and confirm it works for us.** Their AGA
   core and its Emu68 glue, integrated into Bellatrix as they integrate it,
   and run until it is shown working on this machine. Their AGA core
   (`machine/core`) is **GPL-2.0-only**, derived from UAE/WinUAE; a local
   test build is fine, but an Emu68 image that carries it is distributed
   under the GPL-2.0 (MPL-2.0 sec.3.3 allows the combination, and it is what
   they do). So it is a separate test build beside Rigel, not the default.
2. **Then find out why the Rigel integration does not work.** With a
   working reference on the same machine, the comparison is like for like:
   same CPU, same apertures, same core, one chipset swapped for the other.
3. **Then improve the system as a whole, and WHDLoad support.** For WHDLoad,
   Musashi on a core of its own (route 1, Saiph). That core could be the
   one Emu68 gives the PPC: Emu68 runs its PowerPC JIT (WarpOS) on core 3
   when `ppc_enable` is on (`external/emu68/src/aarch64/start.c:465`), and
   no WarpOS code runs while a WHDLoad game owns the machine, so when
   WHDLoad takes over the PPC is quiesced and Musashi comes up there, and
   the reverse when the game quits. Bellatrix uses core 3 today to drain the
   console (`patches/emu68/0020`) and core 1 is free; the drainer is light
   and can move.
4. **Later, AGA support in Rigel.**

# Next

1. Clone at a fixed commit into a scratch location (not a submodule), and
   record the commit here.
2. Licence map of `machine/`, file by file.
3. Diff their Emu68 against its upstream base and against ours.
4. Then the two goals, chipset integration and WHDLoad first.
