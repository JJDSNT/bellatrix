/*
 * Copyright (C) 2022, The AROS Development Team.  All rights reserved.
 *
 * This target does not use a BootStruct -- arch/m68k-emu68/boot/boot.h has its
 * own Emu68BootContext, and nothing here ever reads this one.
 *
 * It is carried only to keep the build moving. workbench/c/mmakefile.src
 * resolves workbench-c-$(AROS_TARGET_CPU) to "workbench-c-m68k", and that
 * target is registered by arch/m68k-amiga/c -- so every m68k target, Amiga or
 * not, inherits the Amiga command set. One of those commands, AROSBootstrap,
 * includes <aros/bootstruct.h>, which reaches the Amiga build through that
 * target's %copy_includes. Without an equivalent here the compile fails, mmake
 * aborts workbench-c, and neither the ~150 generic commands nor
 * S/Startup-Sequence are ever produced -- which is why C/ held six files and
 * S/ did not exist at all.
 *
 * The real fix belongs upstream: arch/m68k-amiga/c should register itself as
 * "workbench-c-amiga-m68k" rather than claiming the whole CPU. Copied verbatim
 * from arch/m68k-amiga/include/aros/bootstruct.h.
 */

#define ABS_BOOT_MAGIC 0x4d363802
struct BootStruct
{
    ULONG magic;
    struct ExecBase *RealBase;
    struct ExecBase *RealBase2;
    struct List *mlist;
    struct TagItem *kerneltags;
    struct Resident **reslist;
    struct ExecBase *FakeBase;
    APTR bootcode;
    APTR ss_address;
    LONG ss_size;
    APTR magicfastmem;
    LONG magicfastmemsize;
};