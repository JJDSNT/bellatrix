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
 * handed to the guest as m68k autovector level 6. Emu68's core-0 IRQ fast
 * path (Emu68 src/aarch64/vectors.c, "curr_el_spx_irq") asserts it on
 * INTF.ARM, and the arbitration in ExecutionLoop.c maps that channel to
 * level 6 unconditionally and honours it against the SR mask exactly like
 * a real 68k.
 *
 * There is nothing for this port to *arm*: the decision has already been
 * made by the time the CPU sees it, and no register access has to fault to
 * deliver an interrupt. There is one thing to acknowledge, and the guest
 * owns it -- see platform_host_irq_ack() below.
 *
 * Two channels reach the same arbitration and they are not
 * interchangeable. INTF.ARM is a latch and belongs to the ARM platform;
 * INTF.IPL is a level, mirrors a physical line on PiStorm, and belongs to
 * whatever resolves an Amiga IPL. This port used INTF.IPL while it had
 * neither a chipset nor an acknowledge, which worked and was always
 * transitional -- docs/New_emu68.md sections 3, 4 and 18 keep INTF.IPL for
 * Rigel and put platform interrupts back on INTF.ARM, which is what this
 * code now does.
 *
 * Before that this port emulated Paula: INTENA/INTREQ served from a
 * shadow, one page fault per arm and per acknowledge. That is upstream's
 * INTF.ARM lifecycle, and it is the part that does not fit a machine with
 * no chipset -- not the channel itself. docs/irq.md compares all three.
 */

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
/*
 * Deassert the host interrupt.
 *
 * INTF.ARM is a latch, not a level: nothing in Emu68 lowers it, so leaving
 * it set re-enters level 6 the moment our RTE drops the SR mask. The guest
 * owns the deassert, and Emu68 already exposes it -- MOVEC on the JITCTRL2
 * control register (0x1e0), where bit 29 is INTF.ARM: it reads back as
 * pending state and writing it back as 1 clears the byte
 * (Emu68 src/M68k_LINE4.c:2104 for the write, :2365 for the read).
 *
 * Register to register. No MMIO, no fault, no chipset register to emulate --
 * which is the whole reason this channel is usable on a machine with no
 * Paula, where upstream's INTENA/INTREQ lifecycle is not.
 *
 * Read-modify-write rather than a blind store, because the low 29 bits of
 * the same register are JIT_CONTROL2 and a blind write would clear them.
 * Bits 29..31 are command bits and are masked out of the stored value, so
 * what comes back has them clear and cannot re-trigger anything.
 */
static inline void platform_host_irq_ack(void)
{
    ULONG ctrl;

    __asm__ volatile (
        "   .word 0x4e7a, 0x01e0    \n"     /* movec JITCTRL2,%%d0        */
        "   move.l %%d0,%0          \n"
        : "=r" (ctrl) : : "d0");

    ctrl |= 1UL << 29;

    __asm__ volatile (
        "   move.l %0,%%d0          \n"
        "   .word 0x4e7b, 0x01e0    \n"     /* movec %%d0,JITCTRL2        */
        : : "r" (ctrl) : "d0", "memory");
}

/*
 * Paula's own interrupts, which this port had no way to deliver.
 *
 * Every m68k autovector here goes to one trampoline that asks the ARM
 * interrupt controller what is pending. That is right for a platform
 * interrupt and blind to a chipset one: Rigel raises INTREQ, the arbitration
 * hands the CPU a level, the trampoline finds nothing on the ARM side and
 * returns. Anything that installed an Amiga interrupt server was never
 * called.
 *
 * It is not academic. arch/m68k-amiga's audio.device -- linked into this ROM
 * on 2026-08-30 -- installs SetIntVector(INTB_AUD0 + ch) for its four
 * channels and refills each buffer from there. Without this, Paula plays
 * whatever was armed and never advances: one sample, for ever. Demo Reel 3
 * programmed all four channels' periods through Rigel and then had nothing
 * to chain them (ISSUE-0079).
 *
 * The shape is arch/m68k-amiga/kernel/amiga_irq.c's, with its seven
 * per-level handlers merged into one pass, because this trampoline is shared
 * across all seven levels and does not know which it was entered for. That
 * costs nothing in correctness: the SR mask is what orders these, and it has
 * already done its work by the time we are here.
 *
 * Acknowledge before dispatching, exactly as upstream does, and for the same
 * reason: a bit with no server installed is then simply cleared instead of
 * re-asserting for ever. SOFTINT is the one exception -- its handler clears
 * it, because it may Cause() again from inside itself.
 */
static void platform_paula_dispatch(void)
{
    volatile UWORD *const intenar = (volatile UWORD *)0x00dff01cUL;
    volatile UWORD *const intreqr = (volatile UWORD *)0x00dff01eUL;
    volatile UWORD *const intreq  = (volatile UWORD *)0x00dff09cUL;
    UWORD ena, mask;
    int bit;

    /*
     * One read before anything else. A machine whose chipset is idle -- which
     * is most of a boot -- pays a single register read per interrupt and
     * leaves, and that read is an MMIO fault to Rigel, so it is not free.
     */
    ena = *intenar;
    if (!(ena & INTF_INTEN))
        return;

    mask = ena & *intreqr;
    if (mask == 0)
        return;

    *intreq = (UWORD)(mask & ~INTF_SOFTINT);

    /* Highest first, the order the seven vectors would have given us. */
    for (bit = INTB_EXTER; bit >= 0; bit--)
        if (mask & (1 << bit))
            core_Cause((unsigned char)bit, mask);
}

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

    /* Deassert before dispatching, not after. ARM interrupts are masked for
     * the whole of this handler -- Emu68 sets the I bit on the eret and only
     * reopens the gate when the guest's SR mask drops below 6 -- so a source
     * that asserts while we are in here cannot re-latch INTF.ARM, and
     * clearing afterwards would discard it. Clearing first means the worst
     * case is one spurious level 6 with nothing to do, instead of a lost one.
     */
    platform_host_irq_ack();

    if (g_intc_ops)
        g_intc_ops->Dispatch(KernelBase);

    platform_paula_dispatch();

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

    platform_trace_val("[soc] periiobase   ", platform_periiobase);

    g_timer_ops->SetPeriod(interval_us);
    g_timer_ops->Start();

    return TRUE;
}

APTR platform_openfirmware_tree(void)
{
    return dt_root_node();
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
