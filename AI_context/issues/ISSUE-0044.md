---
id: ISSUE-0044
title: "Load modules the way AROS does, instead of linking every one into the kernel ELF"
status: doing
priority: medium
type: refactor
owner: unassigned
created_at: 2026-08-18
updated_at: 2026-08-18
tags:
  - build
  - boot
  - kickstart
  - emu68
  - aros
blockers:
related_files:
  - aros/arch/m68k-emu68/boot/mmakefile.src
  - external/aros/arch/arm-native/soc/broadcom/2708/mmakefile.src
  - external/aros/config/make.tmpl
  - external/aros/rom/dos/cliinit.c
  - AI_context/consolidated/history/ISSUE-0043.md
---

# Summary

This port links every module into one kernel ELF. AROS does not, and the target
these drivers came from does not.

`arch/arm-native/soc/broadcom/2708/mmakefile.src` declares its hardware as a
list and hands it to a macro:

```
PKG_RSRC  := gpio mbox dma sdio bwfm
PKG_DEVS  := sdcard USBHardware/usb2otg
PKG_HIDDS := i2c-bcm2708 vc4gfx

%make_package mmake=kernel-package-arm-bcm2708 file=$(AROSDIR)/$(BCM2708_SP) \
    res=$(PKG_RSRC) devs=$(PKG_DEVS) hidds=$(PKG_HIDDS) ...
```

Ours puts the same kinds of thing in `CORERESIDENTS`, and
`CORELOCAL := $(addprefix $(KOBJSDIR)/,$(addsuffix .emu68.ko,$(CORERESIDENTS)))`
links them into the image (`boot/mmakefile.src:74,107`).

`%make_package` is not arm-native's; it is defined in `config/make.tmpl:3744`
and available to any target.

# Why it matters, with today's evidence

**Choosing a driver costs a rebuild.** `GFX_BACKEND` picks between emu68gfx,
fbgfx and vc4gfx by changing what is linked, so every switch is a kernel
relink and a fresh card -- and because make does not reliably notice a module
source change, it is a *careful* relink. That cost was paid repeatedly while
comparing the three, and twice a stale object silently produced a wrong
conclusion.

Under the package mechanism the three would coexist as files and the choice
would be a boot-time one.

**The image carries what it does not use.** Linking vc4gfx took the kernel from
1258 KB to 1286 KB, and the same is true of every driver whether or not the
machine has that hardware.

**It is the shape the sources assume.** These drivers are written to be
packaged: `vc4gallium.hidd` already installs to `Devs/Drivers/` and is opened
by name, and `cliinit.c:632-636` assigns that directory to `DRIVERS:` and adds
it to `LIBS:` precisely so hidds there can be found by `OpenLibrary`. Every
driver ported here so far has been bent away from that.

# The hard part, which is not the packaging

A package has to be *loaded*, and this port's boot chain has nowhere to load it
from.

Emu68 takes one file: `run.sh` passes the ELF as `-initrd` and that is the
whole of what reaches the m68k side. arm-native and m68k-amiga have a
bootstrap that reads several files before AROS starts (`AROSBootstrap`), which
is what makes a package meaningful there.

And the ordering rules it out as a plain disk load: a graphics HIDD is a
resident at priority 9, wanted long before there is a filesystem to read it
from. Loading it from the card would need sdcard.device, fat-handler and DOS
first -- everything the package was meant to contain.

So adopting this means answering **how a second file reaches the m68k side
before AROS starts**. Three shapes, none investigated:

* **Emu68 loads it.** It already loads one initrd; whether it can be asked for
  more, and whether that is a change we can make upstream, is unknown.
* **The package is appended to the ELF** and found by the bootstrap already in
  `arch/m68k-emu68/boot`, which runs before Exec and could parse it.
* **Two-stage**: keep a minimal set resident -- enough to read the card -- and
  package everything after that. This is the modular-kickstart design AROS
  already has, and the one the parked ISSUE-0024 work was reading.

# Notes

**Not ISSUE-0024.** That one is about upstream AROS's BASE package containing
machine-specific code, and lives with `docs/base-package-rfc.md` on the parked
`kickstart-base-package` branch. It is worth reading first -- whoever wrote it
had the modular-kickstart documentation open -- but it is a different subject:
that issue cleans a package, this one is about having any.

**Inside the freeze?** Arguably not: it adds no functionality. It also is not
speed or stability. It is the kind of structural change that is cheapest before
more drivers land and most expensive after, which is an argument for deciding
deliberately rather than drifting.

# Acceptance criteria

- [ ] How a package reaches the m68k side before AROS starts is answered, not assumed
- [ ] At least one driver loads from a package rather than from the ELF
- [ ] `GFX_BACKEND` becomes a boot-time choice rather than a build-time one
- [ ] The kernel image no longer carries drivers the machine may not use

# Execution log

- 2026-08-22 -- **For graphics the hard part turns out not to apply, and the
  work is done.** This issue was written around kickstart packages because a
  gfx HIDD is resident at pri 9 before any filesystem exists, and Emu68 hands
  the m68k side exactly one file. Both halves are true and neither matters: a
  display driver does not need to be resident. AROS expects a *boot* driver in
  the kickstart, registered with `DDRV_BootMode`, and the real driver to arrive
  later from `DEVS:Monitors` -- `AROSMonDrvs`, run from `rom/dos/boot.c:103`,
  executes what it finds there once DOS and the card exist.
  `rom/hidds/gfx/headless` (new in the AROS HEAD refresh) is the worked
  example: `%build_module ... moduledir=Storage/Drivers` plus a `%build_prog`
  loader.

  So `vcgfx` left `CORERESIDENTS` and is now `DEVS:Drivers/vcgfx.hidd` plus
  `DEVS:Monitors/VideoCore`, with `fbgfx` as the resident boot driver. Verified
  in QEMU: the disk driver loads and creates the screen bitmap.

  What remains of this issue is everything that is *not* graphics -- the
  filesystems, the USB stack, the shell commands still linked into the ELF.
  The mechanism for those is still `%make_package`, and for those the one-file
  boot chain is still the obstacle. The acceptance criteria below stand, minus
  `GFX_BACKEND`, which is now a boot-driver choice rather than a display-driver
  one.

- 2026-08-18 -- Answered a follow-up: on aarch64 the driver is still a
  *resident*, it is simply not in the kernel binary. `arch/arm-native/.../
  vc4gfx/mmakefile.src` builds it with `%build_module modtype=hidd`, and
  `arch/aarch64-raspi/boot/mmakefile.src` puts `vc4gfx` in `PKG_HIDDS`, which
  `%make_package` bundles into `$(ARM_BSP)`. `config.txt` then loads that
  bundle separately from the kernel -- `kernel=aros-aarch64-raspi.img` plus
  `initramfs $(ARM_BSP) 0x00800000` -- and the bootstrap registers each
  module's romtag before Exec init, so a pri-9 gfx HIDD in the package runs at
  exactly the point it runs today from the ELF. Residency and packaging are
  independent; only the second is what this issue changes. It also confirms the
  hard part named above: the Pi firmware loads one initramfs, and on this port
  Emu68 has already claimed it.

- 2026-08-18 -- Opened at the user's request, from their question about how the
  aarch64/arm targets load their graphics driver. The answer was the one this
  issue is about: they do not link it. Written after checking that
  `%make_package` is generic rather than arm-native's, that `DEVS:Drivers` is
  already wired into `LIBS:` at boot, and that this port's single-file boot
  chain is what actually stands in the way.
