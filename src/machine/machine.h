/*
 * src/machine/machine.h
 *
 * The Bellatrix machine: what the M68K address space contains, and which parts
 * of it the CPU may reach without going through machine semantics.
 *
 * Emu68 owns the mechanism -- page tables, translation, the fault path.
 * Bellatrix owns the policy. This header is the whole of the boundary between
 * them on the memory side: Emu68 calls machine_init() once, and everything the
 * machine decides is expressed through the mmu_map() Emu68 already provides.
 *
 * See docs/Bus.md sections 3 and 10, and AI_context/issues/ISSUE-0016.md.
 */

#ifndef BELLATRIX_MACHINE_H
#define BELLATRIX_MACHINE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Establish the machine's memory policy.
 *
 * Called by Emu68 once the advertised system memory has been mapped and before
 * the guest runs, so that what this decides is what the guest sees.
 */
void machine_init(void);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_MACHINE_H */
