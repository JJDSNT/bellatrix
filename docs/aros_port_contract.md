# AROS m68k boot contract

The port is intended to run on more than one machine. **Emu68 on a Raspberry Pi
remains the primary target and does not regress**; what changes is that it stops
being the only machine the port is able to describe.

AROS already has a shape for this, used by four other CPUs, and the port is
currently outside it. This document states what `aros/arch/m68k-emu68/` requires
from the machine underneath it, shows where AROS would put each half, and marks
every place where the two are fused. It is the survey, not the work; the work is
tracked in [`ISSUE-0023`](../AI_context/issues/ISSUE-0023.md).

Line references are to the overlay in `aros/arch/m68k-emu68/`, which is the copy
this repository owns. References to other architectures are to
`external/aros/arch/`.

## The shape AROS already has

`arch/` is organised in three families, consistently across every CPU:

| | Holds | Examples |
|---|---|---|
| `<cpu>-all` | CPU-specific, machine-independent | `m68k-all`, `arm-all`, `aarch64-all` |
| `<cpu>-native` | bare metal; the machine is discovered at runtime | `arm-native`, `aarch64-native`, `ppc-native`, `riscv-native` |
| `<cpu>-<machine>` | one concrete machine | `m68k-amiga`, `arm-raspi`, `aarch64-raspi`, `ppc-sam440` |

The contents confirm the division. `aarch64-native/kernel/` holds
`platform_init.c`, `platform_bcm2708.c`, `platform_bcm2711.c`, `devicetree.c`
and `kernel_startup.c` — several platforms coexisting inside the native
architecture, selected at runtime. `arm-raspi/boot/` holds `boot.c`, `elf.c`,
`devicetree.c`, `mmu.c` and `vc_fb.c`: the **bootstrap**, the thing that runs
before AROS and loads it.

Where a particular driver falls has precedent on both sides — `arm-raspi` keeps
a `timer/` and a `battclock/` while `aarch64-native` keeps the BCM platform
files — so each driver is a judgement call. The split that is *not* a judgement
call is bootstrap versus kernel.

### `<cpu>-native` is a source convention, not a build-system one

Worth knowing before planning any move: there is no `native` key in the arch
selection chain. `%gen_archspecificrules` (`config/make.tmpl:3232-3241`) offers
`$(CPU)`, `$(FAMILY)`, `$(ARCH)`, `$(ARCH)-$(VARIANT)` and `$(ARCH)-$(CPU)`, and
`FAMILY` is empty for every non-hosted target (`config/target.cfg.in:11`, from
`aros_target_family`, which `configure.in` sets only for hosted).

So `arch/aarch64-native/` and `arch/arm-native/` build under the *machine's*
key: every `%build_archspecific` in them says `arch=raspi-aarch64`,
`arch=raspi-arm` or `arch=raspi-armeb`, seventeen in total and not one saying
`native`. The directory expresses intent; the mmakefile enumerates the machines
that opt in. A second machine therefore costs one `arch=` line per native
mmakefile — the kernel *source* stays untouched, which is the property worth
having, but the enumeration is explicit and lives in the build system.

### The contract between them is `BootMsg`

The bootstrap hands the kernel a `struct TagItem *` of `KRN_*` tags. This is not
a convention this port would be inventing:
`aarch64-native/kernel/kernel_startup.c` consumes `KRN_KernelBss`,
`KRN_MEMLower`/`KRN_MEMUpper`, `KRN_OpenFirmwareTree`, `KRN_CmdLine` and
`KRN_Platform`, and carries the comment *"The bootstrap emits a
`KRN_MEMLower`/`KRN_MEMUpper` pair per range"*.

`m68k-emu68` already converts into it (`boot/boot.c:512-548`):

```c
add_boot_tag(&tag_index, KRN_MEMLower, lower);
add_boot_tag(&tag_index, KRN_MEMUpper, upper);
add_boot_tag(&tag_index, KRN_OpenFirmwareTree, (uint32_t)ctx->fdt);
...
BootMsg = emu68_boot_tags;
...
sys_base = krnPrepareExecBase(ranges, memory, BootMsg);
```

Everything downstream of that assignment is already portable and already
conventional. Everything upstream of it is bootstrap.

### Where this port sits

`m68k-emu68` occupies the `<cpu>-<machine>` slot and does both jobs inside it:
the kernel, `exec`, the runtime platform discovery and the drivers, alongside
the Emu68 entry shim and FDT parser. The comment at the top of
`platform/platform.h` already names `arch/aarch64-native/platform_bcm2708.c` as
the model it copies — the port is imitating `-native` from outside the slot.

The Emu68 case differs from the Pi's in one way worth stating: for `arm-raspi`
AROS ships its own bootstrap, while for Emu68 the bootstrap is foreign — Emu68
itself, running on aarch64, which loads the `ET_REL` image and enters it. So an
Emu68 machine directory is thinner than `arm-raspi/boot/`: an entry shim and an
FDT parser, and nothing that loads anything.

## The contract

Stated as an obligation, from the point of view of whatever starts this port.
The generic half is `BootMsg` and needs no restating; what follows is the part
that is specific to m68k or to this port.

### A bootstrap must provide

**The system RAM range — base and size.** Without it, `start_aros()` returns
before doing anything (`boot/boot.c:443`) and the machine sits there. There is
no default and no fallback, and nothing is printed.

**An address that absorbs byte stores, for progress output.** Every console
message is an unconditional store (`boot/console.c:23`), so the address must not
fault. Whether the bytes are collected anywhere is the bootstrap's choice — a
machine that leaves ordinary RAM there boots and loses the log.

**Entry.** Place the `ET_REL` image and enter it at the start of the read-only
allocation, in supervisor mode, with `A7` holding a supervisor stack that stays
valid until the port moves onto one Exec owns.

### A bootstrap may provide

**A framebuffer** — address, pitch, width and height. Absent, the boot UI
declines to draw and boot continues; it tests its own flag and all four fields
before touching anything (`boot/bootui.c:254-256`).

**A command line.** Absent, `KRN_CmdLine` is simply not published.

**A periodic tick, a way to mask its source, and a way to ask what is pending.**
Absent, boot prints `platform timer not found` and continues
(`boot/boot.c:114-117`), and everything that wants a tick — `timer.device`,
preemption, the boot animation's clock — has no source. What the port requires
is that a device interrupt reach the CPU and that its source be identifiable;
*which* autovector level carries it is not part of the requirement.

**A hardware description drivers can query at runtime.** Absent, anything that
configures itself through `openfirmware.resource` cannot — storage, in practice.

### What the port supplies itself

A bootstrap is not asked for any of these, and should not attempt them.

- the kernel image extent, from link symbols, published as `KRN_KernelBase`,
  `KRN_KernelLowest` and `KRN_KernelHighest`
- handlers on all seven autovector levels (`platform/platform.c:338`)
- reserving the classic 24-bit domain out of the heap (`boot/boot.c:454`)

### Not stated anywhere

The minimum CPU. `exec/` is assembled with `ISA_MC68060_FLAGS`
(`exec/mmakefile.src:19`), and nothing records what the rest of the port needs,
so a bootstrap cannot currently tell what it has to present. The same flag is
used by `m68k-amiga` for files that dispatch on the CPU at runtime, so it is
unlikely to mean what it looks like — but "unlikely" is the current state of the
answer.

### How Emu68 satisfies it

| Contract item | Delivered by |
|---|---|
| System RAM range | `/memory` node in the FDT |
| Character sink | byte stores to `0xdeadbeef`, which Emu68 leaves unmapped and traps |
| Entry | the initramfs loader, in the registers described under [Entry](#entry) |
| Framebuffer | entry registers `A0`, `D0`, `D1`, `D2` |
| Command line | `/chosen` `bootargs` in the FDT |
| Tick and interrupt source | BCM283x System Timer and ARM control interrupt controller, matched under `/soc`; physical IRQs arrive as autovector level 6 |
| Hardware description | the FDT itself, reparsed and republished as an OF tree |

Four of the seven arrive through the device tree, which is why it is the thing
that has to be generalised first.

## Where delivery is fused into the contract

Five places state a requirement in terms of Emu68 rather than in terms of the
information. Each is a piece of bootstrap living on the kernel side of
`BootMsg`.

### 1. Memory can only arrive as a device tree

`EMU68_BOOT_MEMORY_VALID` is set in exactly one place — inside `parse_fdt()`,
on finding a `/memory` node (`boot/boot.c:397`). `start_aros()` opens with:

```c
if (!(ctx->flags & EMU68_BOOT_MEMORY_VALID))
    return;
```

(`boot/boot.c:443`.) The requirement is that the RAM range be known. The code
requires that a device tree said so. There is no other route, no default and no
fallback, and the refusal is silent — `parse_fdt()` returns cleanly on a header
it does not recognise, so a machine that supplies memory some other way produces
a boot that loads, runs and does nothing.

### 2. Platform discovery takes a flattened tree, not a device list

`platform_timer_start(const void *fdt, ULONG interval_us)`
(`platform/platform.h:129`) is handed the FDT and walks `/soc` itself. The
driver ops underneath are abstract; the discovery above them is not. A machine
with two known devices and no device tree has nothing to pass.

The call site (`boot/boot.c:114`) is not fatal — failure prints
`platform timer not found` and boot continues.

### 3. The device tree outlives boot

`KRN_OpenFirmwareTree` is published into `BootMsg` and then rewritten
(`boot/boot.c:128-135`) with the parsed tree that discovery built, before
COLDSTART residents initialise, so that disk drivers see the same hardware
description that established `KATTR_PeripheralBase` and the IRQ wiring.

This is the coupling that grows. The FDT began as a boot-time delivery mechanism
and is now a runtime hardware description consumed by drivers through
`openfirmware.resource`. Each driver that reads it adds a component that cannot
run on a machine that has no tree.

Note that `KRN_OpenFirmwareTree` is part of the conventional contract —
`aarch64-native` reads it too — so this is not a deviation. What is specific
here is that it is the *only* description, with no path for a machine that has
none.

### 4. Interrupt routing is an Emu68 policy

Any real physical IRQ not claimed by one of Emu68's own virtual devices arrives
as m68k autovector level 6 — Emu68's fixed "EXTER" channel
(`platform/platform.c:268-271`). The port installs all seven autovectors to
`Platform_Autovector_Direct` (`platform/platform.c:338`) and the level-6 handler
asks the discovered controller what is pending.

Answering every level is correct for an m68k and is not the issue. Which level
carries physical interrupts, and the fact that there is exactly one such level,
are properties of this machine.

### 5. The console is an address

`0xdeadbeef` appears in `boot/console.c:23`, `kernel/kernel_debug.c:22` and
`platform/platform.h:94`. Emu68 leaves that address deliberately unmapped and
traps the store. The port needs a character sink; it names a hole in one
machine's address map.

### Adjacent: the address-space assumption

The allocator keeps the whole classic 24-bit domain out of the heap
(`boot/boot.c:454`) because Emu68 maps advertised memory 1:1 and then punches
holes in it — `0x00dff000` for custom chip registers, `0xdeadb000` for its debug
port — and every access to a holed page is trapped and emulated as a device.

The reservation is deliberate and is a precondition for the memory policy
described in [`New_emu68.md`](New_emu68.md) section 6. It is recorded here
because its justification is a fact about one machine's address map, so any
other machine inherits a policy whose reason does not apply to it.

## Loading the image

A machine whose bootstrap is not Emu68 has to load the `ET_REL` image itself.
That machinery exists in AROS already and does not have to be written from
scratch:

- `tools/elf2hunk/elf2hunk.c` — 1253 lines, runs on the build host, accepts
  **only** `ET_REL` (line 439) and applies `R_68K_32` and `R_68K_PC32`
  (rejecting `R_68K_PC16`). That is exactly the relocation set
  `aros-emu68-m68k.elf` uses.
- `arm-raspi/boot/elf.c` and `aarch64-raspi/boot/elf.c` — the Pi bootstraps'
  loaders, both accepting `ET_REL` and walking `SHT_RELA`.

What none of them does is place the image at an absolute base and report that
base as the entry point, which is what `ET_REL` requires and what Emu68's
`src/ElfLoader.c` does. That is the smaller half of the job.

## What is genuinely machine-specific

These need no change either way. They are listed so that the boundary is
unambiguous.

- `platform/bcm283x/` — System Timer and ARM control interrupt controller
- `soc/sdcard/`, `soc/mbox/` — BCM2708 SD host and VideoCore mailbox
- the Emu68 framebuffer HIDD
- Emu68's Zorro III ROM board at `E_EXPANSIONBASE`, which this repository does
  not use (`EMU68_EXPANSION_ROM` is 0)

By the precedent of `aarch64-native/kernel/platform_bcm2708.c`, the first two
would sit inside the native architecture rather than in a machine directory.

## Entry

The native Emu68 initramfs loader enters an `ET_REL` m68k ELF at the start of
the read-only allocation with:

```
A6 = flattened device tree      A0 = framebuffer address
A7 = supervisor stack           D0 = framebuffer pitch
                                D1 = framebuffer width
                                D2 = framebuffer height
```

`boot/entry.S` reorders those into a normal C call to
`emu68_bootstrap(fdt, framebuffer, pitch, width, height)`, declared `noreturn`
in `boot/boot.h`.

The register assignment is a convention of this loader. The information it
carries is not.

## What is missing

Not a layer, and not a contract that has to be designed. The seams are already
the right ones and are already conventional: `BootMsg` with its `KRN_*` tags is
what every other native architecture uses, and `struct PlatformDriver` with its
ops tables is copied from `aarch64-native` by its own admission.

What is missing is that the port occupies one directory in the
`<cpu>-<machine>` slot and does both jobs there, so the bootstrap half cannot be
replaced without editing the kernel half. Moving the kernel half into a
`m68k-native` architecture, and leaving `m68k-emu68` as a bootstrap, is tracked
in [`ISSUE-0023`](../AI_context/issues/ISSUE-0023.md).
