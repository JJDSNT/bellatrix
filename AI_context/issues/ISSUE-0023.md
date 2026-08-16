---
id: ISSUE-0023
title: "Split the port into an m68k-native architecture and an m68k-emu68 bootstrap"
status: backlog
priority: medium
type: refactor
owner: agent
created_at: 2026-08-16
updated_at: 2026-08-16
tags:
  - aros
  - m68k
  - boot
  - platform
  - fdt
  - portability
blockers:
  -
related_files:
  - docs/aros_port_contract.md
  - aros/arch/m68k-emu68/boot/boot.c
  - aros/arch/m68k-emu68/boot/boot.h
  - aros/arch/m68k-emu68/boot/entry.S
  - aros/arch/m68k-emu68/boot/console.c
  - aros/arch/m68k-emu68/platform/platform.c
  - aros/arch/m68k-emu68/platform/platform.h
  - aros/arch/m68k-emu68/kernel/kernel_debug.c
  - aros/arch/m68k-emu68/mmakefile.src
---

# Summary

Generalise the port so it can run on more than one machine, by putting it in the
shape AROS already uses for exactly this. **Emu68 on a Raspberry Pi remains the
primary target and does not regress**; it stops being the only machine the port
is able to describe.

Concretely: a `m68k-native` architecture holding the kernel, `exec` and the
runtime platform discovery, and `m68k-emu68` reduced to a bootstrap. The
contract between the two already exists and is `BootMsg` with its `KRN_*` tags.

The survey behind it — what the port requires, where AROS puts each half, and
the five places where the two are fused, with file and line — is in
[`docs/aros_port_contract.md`](../../docs/aros_port_contract.md). This file
tracks only the work.

# Problem

The port cannot be pointed at a second machine without editing `boot.c`. It has
one machine, so nothing has ever forced it to distinguish *what AROS needs* from
*how Emu68 supplies it*: both are true of the same code, and the requirements
that are Emu68's are not marked as such anywhere.

Structurally, the port occupies the `<cpu>-<machine>` slot in `arch/` and does
two jobs inside it — the kernel and its runtime platform discovery, plus the
Emu68 entry shim and FDT parser. AROS separates those two jobs by directory for
every other CPU that boots bare metal: `arm-native` / `arm-raspi`,
`aarch64-native` / `aarch64-raspi`, and the same for `ppc` and `riscv`. The
comment at the top of `platform/platform.h` already names
`arch/aarch64-native/platform_bcm2708.c` as its model, so the port is imitating
the native architecture from outside the slot.

That already costs something today, in two ways.

**Requirements are discovered by reading source.** `start_aros()` returns
immediately unless `EMU68_BOOT_MEMORY_VALID` is set, and only `parse_fdt()` can
set it. Nothing announces that; the failure is silent, and presents as a boot
that loads, runs and produces no output.

**The coupling is growing.** The FDT started as a boot-time delivery mechanism.
It is now republished as an OF tree through `openfirmware.resource` before
COLDSTART, and disk drivers consume it. Every driver that reads it adds a
component that cannot run on a machine with no tree.

# Goal

`arch/m68k-native/` holds everything above `BootMsg`. `arch/m68k-emu68/` holds
the bootstrap: the entry shim, the FDT parser, and the Emu68 character sink.

Adding a second machine means writing a bootstrap, not editing the kernel.

# What was done

- Surveyed the port against the AROS `arch/` convention and wrote
  `docs/aros_port_contract.md`.

# What is left

Ordered so each step is independently verifiable, with the Emu68 path working
throughout.

1. **Write down the m68k-specific half of the contract.** `BootMsg` and its
   `KRN_*` tags are already the convention and need no restating. What does need
   stating is what is specific to this port: the entry conditions, and the
   character sink. Two paragraphs in a header, no code movement.
2. **Create `arch/m68k-native/` and move the kernel half into it.** Everything
   downstream of `BootMsg = emu68_boot_tags` (`boot/boot.c:548`), plus `exec/`,
   `kernel/`, `platform/` and its drivers. By the precedent of
   `aarch64-native/kernel/platform_bcm2708.c`, the BCM283x drivers and `soc/`
   go here rather than staying with the machine. `m68k-emu68` keeps `entry.S`,
   `parse_fdt()` and `console.c`.
3. **Gate on the information, not on its source.** Replace the
   `EMU68_BOOT_MEMORY_VALID` check (`boot/boot.c:443`) with one that asks
   whether the memory range is known, whoever established it.
4. **Give discovery a device list.** `platform_timer_start()` takes an FDT and
   walks `/soc`. Have the bootstrap produce the list of `PlatformNode`s and pass
   that, so the driver ops tables — which are already abstract — can be reached
   without a tree.
5. **Make the console a function pointer**, supplied by the bootstrap.
   `0xdeadbeef` becomes one implementation.
6. **Decide what to do about the OF tree.** `KRN_OpenFirmwareTree` is part of
   the conventional contract and `aarch64-native` reads it too, so this is not a
   deviation to undo. What is specific here is that it is the only hardware
   description, with no path for a machine that has none. Either that stays a
   documented requirement of any machine wanting those drivers, or the drivers
   move to the platform seam. This is the only item with a real design choice in
   it, and it comes last, with the rest already separated.

Deliberately not in scope: writing a second bootstrap. It is listed under
**Open questions**.

# Decisions taken

**Follow the AROS convention rather than inventing a structure.** The
`<cpu>-all` / `<cpu>-native` / `<cpu>-<machine>` division has four precedents,
and the bootstrap-to-kernel contract is `BootMsg`. An abstraction designed here
would be a fifth dialect of something AROS has already settled.

**Emu68 stays the primary target.** Generalising is not a move away from it.
This is a refactor: Emu68-on-Pi behaviour does not change, every step keeps the
reference target booting, and anything that changes what the Pi does belongs in
its own issue. A step that cannot be taken without regressing the Pi is not
ready to be taken.

**The directory move is step 2, not an open question.** It was previously
recorded as one. It is the structure of the work: without it, "generic half" and
"bootstrap half" have no place to live and the separation is a convention that
the next change erodes.

**IRQ routing is part of the same separation, not a separate concern.** That
physical interrupts arrive as autovector level 6, and that there is exactly one
such level, are properties of this machine. Answering all seven levels is
correct m68k behaviour and stays in the native architecture.

# Acceptance criteria

- [ ] A header states the m68k-specific half of the contract: entry conditions
      and character sink
- [ ] `arch/m68k-native/` exists and holds the kernel, `exec` and platform
      discovery
- [ ] `arch/m68k-emu68/` holds only bootstrap: entry shim, FDT parser, console
- [ ] `boot.c`'s generic half contains no FDT parsing
- [ ] The memory gate tests whether the range is known, not who supplied it
- [ ] `platform_timer_start()` takes a device list, not a flattened tree
- [ ] The console is reached through a function pointer
- [ ] `0xdeadbeef`, `/memory`, `/soc` and the entry register assignment appear
      only in `m68k-emu68`
- [ ] The reference target still boots to the desktop
- [ ] `docs/aros_port_contract.md` is updated to describe the result

# Notes

**The Emu68 machine directory is thinner than the Pi's.** For `arm-raspi`, AROS
ships its own bootstrap — `boot/` there holds `boot.c`, `elf.c`, `devicetree.c`,
`mmu.c` and `vc_fb.c`. For Emu68 the bootstrap is foreign: Emu68 itself, running
on aarch64, loads the `ET_REL` image and enters it. So `m68k-emu68` ends up with
an entry shim and an FDT parser and nothing that loads anything.

**A machine whose bootstrap is not Emu68 has to load the image itself, and that
machinery is already in the tree.** `tools/elf2hunk/elf2hunk.c` accepts only
`ET_REL` and applies `R_68K_32` and `R_68K_PC32` — exactly the relocation set
`aros-emu68-m68k.elf` uses — and the two Pi bootstraps' `elf.c` both walk
`SHT_RELA`. None of them places the image at an absolute base and reports that
base as the entry, which is the remaining piece and the smaller one.

**AROS draws the same boundary a second way, by kickstart package.**
`boot/modular_kickstart.txt` splits the kickstart into BASE (portable),
FS, Poseidon and BSP (the only machine-specific one), and names the ports that
break it — `m68k-amiga` among them, for chipset code in `graphics.library`.
That axis is checkable by grep where the directory axis is not, and clearing it
is [`ISSUE-0024`](ISSUE-0024.md). Our overlay is already clean on it: every
`%build_archspecific` in `arch/m68k-emu68/` targets a BSP module.

**A second machine is what proves the contract.** Until one exists, the
separation is asserted rather than tested, and nothing stops the next change
from quietly reintroducing a dependency. This is a reason to expect a second
bootstrap eventually; it is not a reason to write one before the separation
lands.

# Open questions

- **Which second machine, and when.** The candidate on hand is the Rigel/Musashi
  harness, which would need a bootstrap and an `ET_REL` loader. Worth deciding
  only once the separation is done, and worth remembering that its first
  milestone would be a serial log out of Exec, not a desktop — with no hardware
  description there is no storage, so no dosboot.
- **How much of `soc/` and `platform/bcm283x/` belongs in native.** The
  precedent points at native, but `arm-raspi` and `aarch64-raspi` do keep a
  `timer/` and a `battclock/` of their own, so per-driver judgement is expected
  rather than a rule.
- **The minimum CPU.** `exec/` is assembled with `ISA_MC68060_FLAGS`, the same
  flag `m68k-amiga` uses for files that dispatch on the CPU at runtime, and
  nothing records what the port actually requires. A bootstrap author cannot
  currently tell what the machine has to present.

# Execution log

- 2026-08-16 — Opened. Survey written to `docs/aros_port_contract.md`.
- 2026-08-16 — Reworked against the AROS `arch/` convention after finding that
  `<cpu>-native` / `<cpu>-<machine>` and the `BootMsg` contract already describe
  the intended split. The directory move moved from *Open questions* into the
  plan; the bespoke "contract and bindings" framing was dropped.
