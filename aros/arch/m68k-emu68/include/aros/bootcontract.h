#ifndef AROS_BOOTCONTRACT_H
#define AROS_BOOTCONTRACT_H

/*
 * Copyright (C) 2026, The AROS Development Team. All rights reserved.
 *
 * What this m68k port requires of whatever starts it.
 *
 * This is the port's contract, not Emu68's. It lives in a machine directory
 * only because the port has exactly one machine directory today; it moves to
 * arch/m68k-native when that exists. See AI_context/issues/ISSUE-0023.md, and
 * docs/aros_port_contract.md for the survey this is condensed from.
 *
 *
 * THE GENERIC HALF IS BootMsg, AND IS NOT RESTATED HERE
 *
 * The bootstrap hands the kernel a struct TagItem * of KRN_* tags, and
 * everything downstream of that is already conventional: the system RAM range
 * as a KRN_MEMLower/KRN_MEMUpper pair, KRN_OpenFirmwareTree, KRN_CmdLine.
 * arch/aarch64-native/kernel/kernel_startup.c consumes exactly these, and this
 * port builds the same message in boot/boot.c. Read that file, or the aarch64
 * one; there is nothing m68k about it.
 *
 * What follows is the part that BootMsg cannot express, because it has to be
 * true before there is a message to read.
 *
 *
 * 1. ENTRY
 *
 * The image is ET_REL. Place it at an absolute base and enter it at the first
 * byte of the read-only allocation -- it has no ELF entry point of its own to
 * consult, which is why placing it and entering it are the same decision.
 * boot/entry.S must be first in .text.boot for that to be the stub that runs.
 *
 * On entry the CPU is in supervisor mode, and A7 holds a supervisor stack that
 * stays valid until the port moves onto one Exec owns. Nothing else about the
 * register file is required by the port: the assignment Emu68's loader uses --
 * A6 for the device tree, A0/D0/D1/D2 for the framebuffer -- is that loader's
 * convention, and boot/entry.S exists to turn it into an ordinary C call. A
 * different bootstrap writes a different stub and changes nothing else.
 *
 * The port supplies the rest of its own startup and a bootstrap should not
 * attempt any of it: the kernel image extent comes from link symbols, all seven
 * autovector levels are installed by platform/platform.c, and the classic
 * 24-bit domain is reserved out of the heap by boot/boot.c.
 *
 *
 * 2. A CHARACTER SINK
 *
 * An address where a byte store is absorbed. Every progress message this port
 * emits before the console exists is an unconditional store to it, on no
 * condition and with no test, so the requirement is not that anyone reads the
 * bytes -- it is that the store does not fault. A machine that leaves ordinary
 * RAM at that address boots correctly and loses the log.
 *
 * The bootstrap supplies it as m68k_boot_putc, below. Emu68 satisfies the
 * contract by leaving 0xdeadbeef unmapped and trapping the store, and that
 * address now appears in one place -- boot/console.c, the machine's side.
 *
 * Installing a sink is optional. The default discards, which is the right
 * behaviour for a machine that cannot absorb the store safely: losing the log
 * is a worse boot, not a failed one.
 *
 *
 * 3. NOT STATED ANYWHERE: THE MINIMUM CPU
 *
 * A bootstrap author cannot currently tell what CPU the machine has to
 * present, because the tree gives three different answers and none of them is
 * a statement of requirement:
 *
 *   configure.in                        gcc_target_cpu="m68000"
 *   configure.in                        aros_isa_flags=$(ISA_MC68040_FLAGS)
 *   arch/m68k-emu68/exec/mmakefile.src  TARGET_ISA_AFLAGS=$(ISA_MC68060_FLAGS)
 *
 * m68k-amiga uses the 68060 flag for files that dispatch on the CPU at
 * runtime, so it probably does not mean what it looks like -- but "probably"
 * is the current state of the answer, and this comment is the honest version
 * of it until someone measures.
 */

#ifndef __ASSEMBLER__

/*
 * The character sink of requirement 2.
 *
 * Installed by the bootstrap, before it emits anything. Defined with a
 * discarding default in kernel/kernel_debug.c, so the kernel half links and
 * runs whether or not a machine supplies one.
 *
 * Called one byte at a time and from any context, including before Exec
 * exists and from inside an interrupt. An implementation may not allocate,
 * take a lock, or call back into AROS.
 */
extern int (*m68k_boot_putc)(int chr);

#endif /* !__ASSEMBLER__ */

#endif /* AROS_BOOTCONTRACT_H */
