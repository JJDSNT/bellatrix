# NallePuh as a Chipset Integration Reference

[`JJDSNT/NallePuh`](https://github.com/JJDSNT/NallePuh) is an AmigaOS 4
compatibility layer for system-friendly 68k software that accesses classic
Amiga registers. It emulates selected Paula, blitter, CIAA, and CIAB behavior
and redirects Paula audio through AHI. It is useful as a behavioral reference
for Bellatrix, but its interception architecture should not be ported directly.

This analysis uses repository commit `a0022a31e2bdd9b1261ffdab9f91f50cda5a312b`
(2023-12-11).

## How NallePuh Works

NallePuh installs an AmigaOS 4 data-fault handler and handles accesses to:

| Address | Device | Main implementation |
|---|---|---|
| `0xDFF000` | Custom chipset | `PUH.c` |
| `0xBFE001` | CIAA | `ciaa.c` |
| `0xBFD000` | CIAB | `ciab.c` |

The handler decodes the faulting PowerPC load/store instruction, derives the
register offset, performs an emulated read or write, advances the instruction
pointer, and chains to the previous handler when the access is unrelated.
Paula state is held in `PUHData` and `CustomData`; CIA timers are modeled by
`struct chip` and updated from elapsed host time. Interrupts are delivered
through `Cause()`, `CallInt()`, or `EmulateTags()` depending on whether the
handler is native or emulated 68k code.

The original 68k design used MMU page faults and optionally rewrote
instructions to a mirror register page to reduce the extreme cost of trapping
every access. The current OS4 implementation instead depends on its exception
and emulation interfaces.

## What Bellatrix Should Reuse

The most valuable material is the register-level behavior:

- `DMACON`, `INTENA`, and `INTREQ` use Amiga set/clear bit semantics.
- Paula has four independently latched audio channels with location, length,
  period, volume, data, and DMA-enable state.
- Audio interrupt requests are gated by both pending and enabled bits.
- `VPOSR` and `VHPOSR` provide basic PAL/NTSC beam-position behavior.
- CIA timer latches, one-shot/continuous modes, underflow flags, ICR masking,
  timer-B chaining, and TOD counters are modeled separately for CIAA and CIAB.
- The blitter exposes busy state around `BLTSIZE`/`BLTSIZH` execution.

These semantics should become small, testable device models rather than one
global compatibility application.

## What Should Not Be Ported

Bellatrix already sees m68k MMIO accesses inside Emu68. Consequently it does
not need NallePuh's PowerPC instruction decoder, OS4 data-fault vector,
Petunia `EmulateTags()` bridge, MMU-invalid pages, application patching, GUI,
or register mirror.

NallePuh also explicitly depends on multitasking for timekeeping. Busy loops
under `Forbid()` cannot be modeled accurately by a helper process. Bellatrix
should derive timing from Emu68's monotonic hardware clock and schedule device
events independently of AROS task scheduling. This avoids making chipset
progress depend on the guest being preemptible.

The project targets system-friendly applications, not programs that take over
the machine, program copper displays directly, or require cycle-accurate DMA.
Its compatibility list must therefore not be treated as proof of complete
chipset behavior.

## Proposed Bellatrix Architecture

Keep ownership boundaries explicit:

1. Emu68 decodes accesses to custom and CIA address ranges.
2. Dedicated device-state modules implement register reads, writes, latches,
   DMA state, and timer deadlines.
3. The Emu68 interrupt bridge converts device events into Amiga interrupt
   request bits and asserts the guest IRQ only while an enabled request exists.
4. AROS `cia*.resource`, `audio.device`, and related APIs remain guest-side
   clients; they must not compete with a second owner of the same registers.
5. Optional host backends consume device output: framebuffer/blitter first,
   then Paula samples through an AHI or native audio backend.

Do not perform callbacks into AROS from the MMIO fault path. Update state,
queue an event, and return. This is especially important given the current
work on interrupt acknowledgement, preemption, and preservation of m68k task
contexts.

## Recommended Implementation Order

1. Add a register conformance harness for set/clear behavior and readback.
2. Implement CIAA/CIAB timer A/B and ICR delivery using Emu68 clock deadlines.
3. Add joystick, mouse-button, and keyboard-related CIA port bits.
4. Implement Paula channel latching and interrupt cadence without audio output.
5. Add a host audio backend after interrupt timing is stable.
6. Integrate blitter operations and busy signaling.
7. Consider beam counters and copper only after the earlier components have
   deterministic tests.

Each stage should boot repeatedly with chipset emulation disabled and enabled.
Failures must be classified separately as incorrect register semantics,
missing event delivery, duplicate device ownership, or task-context corruption.

## Licensing and Validation

The repository carries GPL version 2-or-later at project level, while some CIA
files contain MIT notices. Treat code copying as GPL-derived unless individual
file provenance is verified; a clean behavioral reimplementation from Amiga
hardware documentation may be preferable.

Use NallePuh's supported and unsupported application lists only as candidate
tests. Add deterministic unit tests for every register rule, followed by boot
tests and small hardware-banging programs. Full demos and games should be the
last validation layer, not the first debugging tool.
