/*
 * src/machine/region.h
 *
 * What the machine's address space contains, as one table.
 *
 * The point of a table rather than a sequence of mmu_map() calls is an
 * invariant that docs/Bus.md section 5 states as mandatory:
 *
 *   The Bellatrix memory policy and fault policy describe the same machine.
 *   A hardware range intended to trap MUST NOT simultaneously have a direct
 *   mapping that bypasses the fault path, and an intentionally trapped range
 *   MUST have defined handling.
 *
 * Two policies that are written down twice can disagree, and this project has
 * already paid for one that did -- Emu68's fault handler recognised
 * $E80000-$E8FFFF as autoconfig while the MMU handed the same range to the
 * guest as plain DRAM, so the handler was unreachable and nobody noticed.
 * Here there is one description and two consumers of it: install() is the only
 * thing that programs the MMU, and find() is the only thing the fault path
 * asks. They cannot drift because there is nothing to keep in step.
 *
 * See also docs/Rigel_integration.md section 8, which makes the ownership
 * split normative: this side owns registration of address-space providers,
 * and the region's owner owns the semantics of accesses within it.
 */

#ifndef BELLATRIX_MACHINE_REGION_H
#define BELLATRIX_MACHINE_REGION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    /*
     * Normal memory. Mapped by the MMU and never seen by the fault path --
     * which is not only a performance property but a correctness one: a
     * region whose reads have no side effects should be DIRECT even when it
     * is not really RAM. The legacy integration measured Exec's romtag sweep
     * costing 262144 data aborts across a fault-driven $F00000 window, each
     * returning the same constant, and mapped a prepared read-only page
     * instead. Observationally identical, and not remotely equal in cost.
     */
    MACHINE_REGION_DIRECT = 0,

    /*
     * Machine semantics. Deliberately has no usable translation, so the access
     * reaches an owner that decides what it means. TRAPPED is not "invalid"
     * (docs/Bus.md section 4).
     */
    MACHINE_REGION_EXTERNAL,

    /*
     * No semantics at all. Also trapped, but nobody owns it: the access is
     * recorded and answered as open bus. This is the default for the classic
     * domain, and it is what makes discovery possible -- an address is
     * investigated and classified before it is promoted, never promoted
     * because software touched it.
     */
    MACHINE_REGION_UNMAPPED
} MachineRegionKind;

struct MachineRegion;

typedef struct MachineRegionOps
{
    uint32_t (*read)(const struct MachineRegion *region, uint32_t address,
                     int size);
    void     (*write)(const struct MachineRegion *region, uint32_t address,
                      int size, uint32_t value);
} MachineRegionOps;

typedef struct MachineRegion
{
    uint32_t base;          /* guest address, 4 KiB aligned */
    uint32_t size;          /* multiple of 4 KiB */
    MachineRegionKind kind;
    const char *name;       /* for the boot report; not optional in practice */

    /*
     * DIRECT only: the host physical address backing this region.
     *
     * Kept separate from `base` on purpose, and it stays separate even while
     * the two happen to be equal. docs/Rigel_integration.md section 30 lists
     * guest physical address and host memory address among four
     * representations that "MUST NOT be treated as interchangeable simply
     * because an implementation can sometimes map between them cheaply", and
     * the legacy notes are blunter about what it cost to conflate them:
     * "esses dois espaços não devem voltar a ser confundidos".
     */
    uintptr_t host_phys;

    /* DIRECT only: memory attributes -- cacheability, read-only. The access
     * flag is not set here; the kind decides it. */
    uint32_t attr;

    /* EXTERNAL only: who owns the semantics, and its context. */
    const MachineRegionOps *ops;
    void *owner;
} MachineRegion;

/*
 * Install a region and program the MMU to match it.
 *
 * This is the only place that calls mmu_map(). Returns 0 on success, and
 * refuses -- loudly -- a region that is misaligned, empty, or overlaps one
 * already installed, because an overlap is precisely the ambiguity the table
 * exists to prevent.
 */
int machine_region_install(const MachineRegion *region);

/*
 * The region containing an address, or NULL.
 *
 * For the fault path only. Ordinary loads and stores are resolved by the MMU
 * and must never reach a lookup: the table is a control plane, not a decoder
 * on the hot path.
 */
const MachineRegion *machine_region_find(uint32_t address);

typedef enum
{
    MACHINE_ACCESS_INSIDE = 0,  /* wholly within the region returned */
    MACHINE_ACCESS_NONE,        /* the start address is in no region */
    MACHINE_ACCESS_STRADDLES    /* starts in a region and leaves it */
} MachineAccessFit;

/*
 * Classify a whole access, not just where it starts.
 *
 * An access is a width, and the region it begins in is not necessarily the
 * region it ends in. The legacy Emu68 audit put this plainly among the facts
 * it established -- "full-width page-boundary classification is required" --
 * and classifying by start address alone gets a boundary-crossing access
 * wrong in the direction that matters: it would let a longword that begins in
 * the last two bytes of normal memory be answered as though all four bytes
 * were, when half of them belong to something with different semantics.
 *
 * Returns the region when the whole access fits inside it. When it straddles,
 * returns the region the access begins in and says so through *fit, because
 * the caller needs to know it is looking at an ambiguity rather than a fact.
 */
const MachineRegion *machine_region_classify(uint32_t address, uint32_t size,
                                             MachineAccessFit *fit);

/* Print the installed map. Makes the section 5 invariant auditable. */
void machine_region_report(void);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_MACHINE_REGION_H */
