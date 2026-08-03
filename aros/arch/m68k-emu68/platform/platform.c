/*
 * Port-common platform discovery and dispatch for arch/m68k-emu68.
 * See platform.h.
 */
#include "platform.h"
#include "fdt.h"

#include <aros/kernel.h>
#include <aros/macros.h>
#include <exec/types.h>
#include <hardware/intbits.h>

#include "cpu_m68k.h"
#include <kernel_base.h>
#include <proto/kernel.h>
#include <kernel_intr.h>
#include <kernel_interrupts.h>

#include "exec_platform.h"

#define PLATFORM_AUTOVECTOR_LEVEL 6

/*
 * Emu68's ARM -> m68k interrupt bridge.
 *
 * A real physical IRQ that none of Emu68's own virtual devices claims is
 * handed to the guest over the Amiga EXTER channel rather than over a
 * vector of its own. Emu68's core-0 IRQ fast path (Emu68
 * src/aarch64/vectors.c, "curr_el_spx_irq") drives that channel off the
 * INTENA/INTREQ shadow it maintains for us, and the protocol has two halves
 * the guest must honour:
 *
 *  - The fast path always records its internal ARMPending flag, but only
 *    raises the m68k level-6 line when the shadow has *both* INTEN and
 *    EXTER set. The channel therefore has to be armed once at startup, or
 *    the physical IRQ arrives and is silently dropped.
 *
 *  - It clears ARMPending (and drops the level-6 line) only on a guest
 *    write to INTREQ with the SET/CLR bit clear and EXTER set. Every
 *    level-6 entry must therefore acknowledge the bridge *in addition* to
 *    whatever the peripheral that fired needs, exactly the way
 *    arch/m68k-amiga/kernel/amiga_irq.c's PAULA_IRQ_ACK does after running
 *    a server chain. Acknowledging only the peripheral leaves the level-6
 *    line asserted.
 *
 * These are ordinary Amiga custom-chip writes; Emu68 traps them and they
 * never reach real silicon.
 */
/*
 * Both halves are ordinary Amiga custom-chip writes. Emu68 traps them and
 * they never reach silicon -- there is none. This is the same idiom
 * arch/m68k-amiga/kernel/amiga_irq.c uses against real Paula, which is the
 * point: nothing here is Emu68-specific.
 */
#define CUSTOM_INTENA ((volatile UWORD *)0x00dff09aUL)
#define CUSTOM_INTREQ ((volatile UWORD *)0x00dff09cUL)

static inline void emu68_exter_enable(void)
{
    *CUSTOM_INTENA = INTF_SETCLR | INTF_INTEN | INTF_EXTER;
}

static inline void emu68_exter_ack(void)
{
    /* SET/CLR clear -> clear EXTER, which is what drops the level-6 line. */
    *CUSTOM_INTREQ = INTF_EXTER;
}

extern const struct PlatformDriver bcm283x_system_timer_driver;
extern const struct PlatformDriver bcm283x_armctrl_ic_driver;

static const struct PlatformDriver *drivers[] = {
    &bcm283x_system_timer_driver,
    &bcm283x_armctrl_ic_driver,
};
#define NUM_DRIVERS (sizeof(drivers) / sizeof(drivers[0]))

static of_node_t *soc_node;
static uint32_t soc_address_cells;
static uint32_t soc_size_cells;
static uint32_t root_address_cells;

static const struct PlatformIntcOps *g_intc_ops;
static const struct PlatformTimerOps *g_timer_ops;

/*
 * Base of the peripheral window as the guest sees it, i.e. the parent address
 * that /soc's first "ranges" entry maps to. Emu68 places the Pi peripherals
 * somewhere of its own choosing and rewrites the FDT to match, so this is
 * discovered rather than assumed.
 *
 * Read back through KrnGetSystemAttr(KATTR_PeripheralBase) -- see
 * kernel/getsystemattr.c. Drivers that live outside this module (sdcard,
 * mbox) need it and cannot see our statics: every module is linked with
 * --localize-symbols.
 *
 * Explicitly initialised, and this file is built -fno-common: a tentative
 * definition would land in COMMON, and Emu68's ELF loader rejects the whole
 * image on the first COMMON symbol it meets (src/ElfLoader.c, SHN_COMMON ->
 * return 0) rather than allocating for it.
 */
ULONG platform_periiobase = 0;

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }

    return *a == *b;
}

/* A "compatible" property is a NUL-separated list of strings, most specific
 * first; match against any of them. */
static BOOL compatible_matches(of_property_t *prop, const char *want)
{
    const char *cursor = (const char *)prop->op_value;
    const char *end = cursor + prop->op_length;

    while (cursor < end && *cursor)
    {
        if (str_eq(cursor, want))
            return TRUE;

        while (cursor < end && *cursor)
            cursor++;
        cursor++;
    }

    return FALSE;
}

/* Collapse a multi-cell FDT address/size field to a ULONG, rejecting
 * anything that doesn't fit in 32 bits -- this target's address space is
 * 32-bit throughout. */
static uint32_t cells_to_u32(const uint32_t *raw_cells, uint32_t count)
{
    uint32_t i;

    if (count == 0)
        return 0;

    for (i = 0; i + 1 < count; i++)
    {
        if (AROS_BE2LONG(raw_cells[i]) != 0)
            return 0;
    }

    return AROS_BE2LONG(raw_cells[count - 1]);
}

/* Translate a child bus address (as it appears in a /soc child's own "reg")
 * through /soc's "ranges" into the real, guest-accessible address that
 * Emu68 already mapped it to (see boot.c for how that mapping got there).
 * No "ranges" property means identity mapping, per DT convention. */
static BOOL soc_translate(uint32_t child_addr, ULONG *out)
{
    of_property_t *ranges = dt_find_property(soc_node, "ranges");
    uint32_t entry_cells = soc_address_cells + root_address_cells + soc_size_cells;
    const uint32_t *cells;
    uint32_t count, i;

    if (!ranges || entry_cells == 0 || ranges->op_length == 0)
    {
        *out = child_addr;
        return TRUE;
    }

    cells = (const uint32_t *)ranges->op_value;
    count = ranges->op_length / (entry_cells * sizeof(uint32_t));

    for (i = 0; i < count; i++)
    {
        const uint32_t *entry = cells + i * entry_cells;
        uint32_t bus_base = cells_to_u32(entry, soc_address_cells);
        uint32_t parent_base = cells_to_u32(entry + soc_address_cells,
                                            root_address_cells);
        uint32_t size = cells_to_u32(entry + soc_address_cells + root_address_cells,
                                     soc_size_cells);

        if (child_addr >= bus_base && child_addr - bus_base < size)
        {
            *out = parent_base + (child_addr - bus_base);
            return TRUE;
        }
    }

    return FALSE;
}

/* Parent address of /soc's first "ranges" entry: where the peripheral window
 * starts as far as the guest is concerned. Falls back to leaving *out alone
 * if /soc has no usable "ranges", in which case addresses are identity-mapped
 * and there is no single base to report. */
static void soc_periiobase(ULONG *out)
{
    of_property_t *ranges = dt_find_property(soc_node, "ranges");
    uint32_t entry_cells = soc_address_cells + root_address_cells + soc_size_cells;

    if (!ranges || entry_cells == 0 ||
        ranges->op_length < entry_cells * sizeof(uint32_t))
        return;

    *out = cells_to_u32((const uint32_t *)ranges->op_value + soc_address_cells,
                        root_address_cells);
}

static BOOL node_reg(of_node_t *node, struct PlatformNode *out)
{
    of_property_t *reg = dt_find_property(node, "reg");
    uint32_t entry_cells = soc_address_cells + soc_size_cells;
    uint32_t child_addr;

    if (!reg || entry_cells == 0 ||
        reg->op_length < entry_cells * sizeof(uint32_t))
        return FALSE;

    child_addr = cells_to_u32((const uint32_t *)reg->op_value, soc_address_cells);
    out->size = cells_to_u32((const uint32_t *)reg->op_value + soc_address_cells,
                             soc_size_cells);

    return soc_translate(child_addr, &out->base);
}

static const struct PlatformDriver *find_driver(of_node_t *node)
{
    of_property_t *compat = dt_find_property(node, "compatible");
    uint32_t i;

    if (!compat)
        return NULL;

    for (i = 0; i < NUM_DRIVERS; i++)
    {
        if (compatible_matches(compat, drivers[i]->compatible))
            return drivers[i];
    }

    return NULL;
}

/*
 * Two passes on purpose: KrnAddIRQHandler() (called from a timer driver's
 * Init()) immediately calls ictl_enable_irq() -> g_intc_ops->EnableIRQ(), so
 * the interrupt controller has to be discovered and initialised first,
 * regardless of the order the two nodes appear in the FDT.
 */
static BOOL discover(void)
{
    of_node_t *child;

    soc_node = dt_find_node("/soc");
    if (!soc_node)
        return FALSE;

    soc_address_cells = dt_prop_u32_default(soc_node, "#address-cells", 1);
    soc_size_cells = dt_prop_u32_default(soc_node, "#size-cells", 1);
    root_address_cells = dt_prop_u32_default(dt_find_node("/"), "#address-cells", 1);

    soc_periiobase(&platform_periiobase);

    ForeachNode((struct List *)&soc_node->on_children, child)
    {
        const struct PlatformDriver *driver = find_driver(child);
        struct PlatformNode node;

        if (driver && driver->intc_ops && !g_intc_ops &&
            node_reg(child, &node) && driver->intc_ops->Init(&node))
            g_intc_ops = driver->intc_ops;
    }

    if (!g_intc_ops)
        return FALSE;

    ForeachNode((struct List *)&soc_node->on_children, child)
    {
        const struct PlatformDriver *driver = find_driver(child);
        struct PlatformNode node;

        if (driver && driver->timer_ops && !g_timer_ops &&
            node_reg(child, &node) && driver->timer_ops->Init(&node))
            g_timer_ops = driver->timer_ops;
    }

    return g_timer_ops != NULL;
}

/*
 * Emu68's fixed "EXTER" channel: any real physical IRQ not claimed by one
 * of Emu68's own virtual devices always arrives as m68k autovector level 6.
 * Install one common trampoline there (same shape as
 * arch/m68k-amiga/kernel/amiga_irq.c's DECLARE_TrapCode levels) that hands
 * off to whichever interrupt controller driver was discovered.
 */
BOOL Platform_Autovector(void)
{
    /* Bounded: did level 6 ever reach us at all, independently of whether
     * dispatch then finds and runs a handler? */
    static ULONG entries = 0;
    if (entries < 3)
    {
        entries++;
        platform_trace_val("[exter] LEVEL6 entry ", entries);
    }

    if (g_intc_ops)
        g_intc_ops->Dispatch(KernelBase);

    /* Acknowledge the bridge only once Dispatch() has drained every source
     * it can see -- the same ordering arch/m68k-amiga uses for a server
     * chain. Emu68 keeps the host IRQ masked for the whole of this handler,
     * so nothing can set ARMPending again behind us and be lost here. */
    emu68_exter_ack();

    return TRUE;
}

void Platform_Autovector_Direct(void);
asm (
    "   .global Platform_Autovector_Direct\n"
    "   .type Platform_Autovector_Direct,@function\n"
    "Platform_Autovector_Direct:\n"
    "   movem.l %d0/%d1/%a0/%a1/%a5/%a6,%sp@-\n"
    "   jsr     Platform_Autovector\n"
    "   tst.w   %d0\n"
    "   beq     0f\n"
    "   jmp     Exec_6_ExitIntr\n"
    "0:\n"
    "   movem.l %sp@+,%d0/%d1/%a0/%a1/%a5/%a6\n"
    "   rte\n"
);

BOOL platform_timer_start(const void *fdt, ULONG interval_us)
{
    volatile APTR *vectors = (volatile APTR *)0;

    if (!dt_parse(fdt))
        return FALSE;

    /*
     * Arm before discover(): the timer driver's Init() registers its handler,
     * which unmasks the source at the controller. Emu68's fast path masks ARM
     * IRQs on return and nothing re-enables them, so if an IRQ lands while the
     * shadow is still clear that is the only one we ever get -- it records
     * ARMPending, skips INTF.ARM, and leaves the CPU deaf.
     */
    emu68_exter_enable();

    if (!discover())
        return FALSE;

    /*
     * All seven autovector levels.
     *
     * This is an Amiga, and an Amiga answers every level -- which level a
     * given interrupt arrives on is decided below us and is not something
     * this port should be modelling. arch/m68k-amiga/kernel/amiga_irq.c
     * installs the same seven with its irqVector[0..6]. Wiring only
     * PLATFORM_AUTOVECTOR_LEVEL would leave six vectors pointing at whatever
     * the bootstrap left, which is how an unexpected level turns into the
     * JIT translating the vector table as if it were code.
     *
     * They share one trampoline because the dispatch does not depend on the
     * level: it asks the interrupt controller what is pending and drains it.
     */
    {
        int level;

        for (level = 1; level <= 7; level++)
            vectors[24 + level] = Platform_Autovector_Direct;
    }

    /* INTENAR (0xdff01c) reads back the mask Emu68 is holding for us. */
    platform_trace_val("[exter] INTENAR    ", *(volatile UWORD *)0x00dff01cUL);
    platform_trace_val("[soc] periiobase   ", platform_periiobase);

    g_timer_ops->SetPeriod(interval_us);
    g_timer_ops->Start();

    return TRUE;
}

/*
 * kernel_arch.h wires these to the generic KrnAddIRQHandler()/
 * KrnRemIRQHandler() path (see rom/kernel/addirqhandler.c), same as
 * arch/aarch64-native and arch/arm-native do for the same hardware.
 */
void ictl_enable_irq(uint8_t irq, struct KernelBase *kb)
{
    (void)kb;

    if (g_intc_ops)
        g_intc_ops->EnableIRQ(irq);
}

void ictl_disable_irq(uint8_t irq, struct KernelBase *kb)
{
    (void)kb;

    if (g_intc_ops)
        g_intc_ops->DisableIRQ(irq);
}
