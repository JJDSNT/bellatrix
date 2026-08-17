/*
    Copyright (C) 2026, The Bellatrix Project. All rights reserved.

    The BCM2835 SDHOST controller, compiled in place from arch/arm-native.

    Three lines instead of a build-system change, and the reason is worth
    stating because the obvious approach was tried first and does not work.

    soc/dma and soc/usb/usb2otg compile arm-native sources where they live, by
    naming them with $(SRCDIR) in front: %build_module goes through
    rule_compile_multi, which pairs `vpath %.c $(..._MC_DIRS)` with a bare
    `%.c` prerequisite and so accepts an absolute basename.

    The SD card backend does not attach through %build_module. It attaches
    through %build_archspecific, which cannot reach outside its own directory:
    its vpath prefixes $(CURDIR) unconditionally *and* its pattern rules name
    the source literally as $(SRCDIR)/$(CURDIR)/%.c, so vpath is never
    consulted. Worse, it does not fail -- `$(BD_OBJS) : | $(BD_GENDIR)` gives
    every object a rule with no recipe, so make reports success and the module
    links whatever was already in the objdir.

    A relative path is not a way out either: this repository symlinks
    arch/m68k-emu68 to a directory outside the AROS tree, and the kernel
    resolves ".." physically, so soc/sdcard/../../.. leaves the tree.

    So the file that %build_archspecific compiles lives here, and the source it
    compiles does not. There is still exactly one copy of the driver, and it is
    upstream's. The include resolves through the -I in mmakefile.src.

    See AI_context/issues/ISSUE-0013.md.
*/

#include "sdcard_sdhost_init.c"
