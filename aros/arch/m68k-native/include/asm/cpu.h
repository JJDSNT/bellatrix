#ifndef ASM_M68K_EMU68_CPU_H
#define ASM_M68K_EMU68_CPU_H

/*
 * The CPU primitives drivers written for arm-native expect.
 *
 * arch/arm-all/include/asm/cpu.h defines isb(), dsb(), dmb(), sev() and wfe()
 * for every ARM target. arch/m68k-all has no include/asm/ at all, so a driver
 * that reaches for <asm/cpu.h> -- rd32le()/wr32le() in the BCM2708 USB OTG
 * driver call dmb() on every register access -- does not build here for want
 * of about thirty lines. This is those lines. See
 * AI_context/issues/ISSUE-0019.md.
 *
 * They live in this target rather than in arch/m68k-all because what is right
 * here follows from Emu68, not from the 68k. On a machine with a real 68040
 * and a real chipset the answers may differ, and asserting them for
 * m68k-amiga is not something this port has studied.
 */

/*
 * Barriers are compiler barriers only, and that is not a shortcut.
 *
 * The guest has no barrier instruction to issue: the m68k architecture has
 * none, and what actually reaches the bus is AArch64 code the JIT emitted.
 * Ordering between those stores and a device is Emu68's to guarantee, not the
 * guest's -- the guest cannot even name the barrier it would want. What the
 * guest *can* do, and must, is stop the compiler moving accesses across the
 * point where it believes a device is watching.
 *
 * This is the same answer soc/mbox/mbox_init.c reached independently, where
 * MBOX_DSB()/MBOX_DMB() are exactly this and have been carrying every mailbox
 * transaction on this port for months.
 *
 * Where real ordering against a device *is* required, it comes from cache
 * maintenance rather than from here: CacheClearE() issues CPUSHP, which Emu68
 * translates with a dsb sy on either side of the maintenance loop.
 */
static inline void isb(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void dsb(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void dmb(void) { __asm__ __volatile__("" ::: "memory"); }

/*
 * Event signalling is an ARMv7 idiom for parking a core cheaply, and this
 * guest is a single m68k with nothing to signal to. sev() is a no-op and
 * wfe() must not block: a driver that spins on wfe() waiting for a hardware
 * bit would otherwise stop making progress. Both keep the compiler barrier so
 * the surrounding poll re-reads what it is polling.
 */
static inline void sev(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void wfe(void) { __asm__ __volatile__("" ::: "memory"); }

/*
 * Stop the machine, for rom/kernel/kernel_panic.c's `for (;;) HALT;`.
 *
 * This target reached that line through arch/m68k-amiga/include/asm/cpu.h until
 * this file existed, which is how it was found: adding <asm/cpu.h> here shadowed
 * m68k-amiga's and took HALT away with it. The rest of that header is Amiga
 * interrupt-vector addresses and handler installers, and nothing outside
 * m68k-amiga uses them, so they are deliberately not carried over.
 *
 * `stop #0x2700` is the same instruction m68k-amiga uses: supervisor, interrupts
 * masked, halted. Emu68 implements STOP, so this stops rather than falling
 * through into a spin.
 */
#define HALT    __asm__ volatile("stop #0x2700")

#endif /* ASM_M68K_EMU68_CPU_H */
