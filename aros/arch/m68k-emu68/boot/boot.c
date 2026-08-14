/*
 * Native Emu68 bootstrap.
 *
 * Emu68 has already initialized the Raspberry Pi and its m68k execution
 * environment before entering this code. This layer translates Emu68's
 * register ABI into persistent state which the AROS kernel can consume.
 */

#include "boot.h"

#include <aros/kernel.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

#include "kernel_base.h"
#include "kernel_romtags.h"
#include "m68k_exception.h"
#include "platform.h"

#define FDT_MAGIC       0xd00dfeedUL
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

struct FdtHeader
{
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct Emu68BootContext emu68_boot_context;

extern struct TagItem *BootMsg;
extern char __aros_resident_start[];
extern char __aros_resident_end[];
extern void Exec_Supervisor_Trap(void);
extern void emu68_enter_user(void (*entry)(void), void *stack)
    __attribute__((noreturn));
extern void m68k_ExecInstallPreserveAll(struct ExecBase *SysBase);
extern void SuperstackSwap(void);

/* arch/m68k-amiga/boot/start.c uses the same 8KB. */
#define SS_STACK_SIZE   0x2000

/* boot/trapprobe.c -- bring-up instrumentation, compiles to an empty table. */
extern const struct M68KException emu68_exception_table[];

static struct TagItem emu68_boot_tags[9];

void emu68_set_stage(uint32_t stage)
{
    struct Emu68BootContext *ctx = &emu68_boot_context;

    ctx->stage = stage;

    /*
     * The first page is kept out of the allocator. Leave a big-endian marker
     * immediately above the 68k vector table so a bare-metal monitor can
     * diagnose boot progress before a console is available.
     */
    *(volatile uint32_t *)0x400 = stage;
}

/*
 * There is no classic Expansion on this port.
 *
 * What used to stand here walked expansion.library's BoardList for DiagAreas
 * and romtags, on the premise that Emu68 answers autoconfig cycles at
 * E_EXPANSIONBASE. It does -- but only when that address faults, and on this
 * target it does not: Emu68 maps the advertised system memory 1:1, so
 * $E80000-$E8FFFF is ordinary DRAM and the bus walk was reading whatever
 * happened to be there.
 *
 * expansion.library stays in CORERESIDENTS, because DOS boot needs the library
 * itself and removing it ends in an "Exec Bootstrap Task" requester. What is
 * gone is the port-specific backend: rom/expansion's own ConfigChain() and
 * ReadExpansionByte() stubs do nothing, which is the correct behaviour for a
 * machine with no Zorro bus.
 *
 * See AI_context/issues/ISSUE-0016.md and docs/New_emu68.md section 25.
 */

static void coldstart_user(void)
{
    struct Emu68BootContext *ctx = &emu68_boot_context;
    ULONG timer_interval_us;
    APTR ss_stack;

    emu68_set_stage(EMU68_STAGE_COLDSTART);
    emu68_console_puts("[AROS/Emu68] InitCode COLDSTART in user mode\n");

    /*
     * Start the platform heartbeat before resident initialization, so
     * timer.device and the boot animation have a tick source as soon as they
     * come up.
     */
    timer_interval_us = SysBase->VBlankFrequency
        ? 1000000UL / SysBase->VBlankFrequency
        : 20000UL;
    if (timer_interval_us &&
        platform_timer_start(ctx->fdt, timer_interval_us))
        emu68_console_puts("[AROS/Emu68] platform timer enabled\n");
    else
        emu68_console_puts("[AROS/Emu68] platform timer not found\n");

    /*
     * Move off the supervisor stack Emu68 gave us and onto one Exec owns.
     *
     * arch/m68k-amiga/boot/start.c does this in doInitCode(), in user mode,
     * immediately before InitCode(RTF_COLDSTART) -- so this is the same point
     * in the same boot phase. Until it happens, every trap into supervisor
     * (Exec/Supervisor(), and therefore the scheduler's KrnSchedule() path)
     * is pushing onto whatever the bootstrap left in SSP, of unknown size and
     * unknown ownership.
     *
     * MEMF_REVERSE keeps it out of the way of the low allocations the rest of
     * the boot makes. Amiga page-aligns it for its MMU tables; we have no MMU
     * of our own to protect it with, so alignment buys nothing here.
     */
    ss_stack = AllocMem(SS_STACK_SIZE, MEMF_ANY | MEMF_CLEAR | MEMF_REVERSE);
    if (ss_stack)
    {
        SysBase->SysStkLower = ss_stack;
        SysBase->SysStkUpper = (UBYTE *)ss_stack + SS_STACK_SIZE;
        Supervisor((ULONG_FUNC)SuperstackSwap);
        emu68_console_puts("[AROS/Emu68] supervisor stack swapped\n");
    }
    else
    {
        emu68_console_puts("[AROS/Emu68] supervisor stack alloc failed\n");
    }

    /*
     * This does not return, and every other AROS target relies on that:
     * dosboot.resource's init function either hands over to dos.library or
     * loops forever retrying for boot media, so control never comes back.
     * arch/aarch64-native treats a return as fatal ("System Boot Failed!").
     *
     * Boot with "sysdebug=InitCode" to see the resident list and watch each
     * module initialize.
     */
    InitCode(RTF_COLDSTART, 0);

    emu68_console_puts("[AROS/Emu68] InitCode COLDSTART returned -- boot failed\n");

    for (;;)
        ;
}

static uint32_t align4(uint32_t value)
{
    return (value + 3) & ~3UL;
}

static void put_hex32(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[9];
    int i;

    for (i = 7; i >= 0; i--, value >>= 4)
        buffer[i] = digits[value & 15];
    buffer[8] = '\0';

    emu68_console_puts(buffer);
}

static int bounded_string_equal(const char *value, uint32_t value_size,
                                const char *expected)
{
    uint32_t i = 0;

    while (expected[i] != '\0')
    {
        if (i >= value_size || value[i] != expected[i])
            return 0;
        i++;
    }

    return i < value_size && value[i] == '\0';
}

static int node_is_memory(const char *name, const uint8_t *limit)
{
    static const char prefix[] = "memory";
    uint32_t i;

    for (i = 0; i < sizeof(prefix) - 1; i++)
    {
        if ((const uint8_t *)&name[i] >= limit || name[i] != prefix[i])
            return 0;
    }

    return (const uint8_t *)&name[i] < limit &&
           (name[i] == '\0' || name[i] == '@');
}

static const char *fdt_string(const uint8_t *strings, uint32_t strings_size,
                              uint32_t offset)
{
    uint32_t i;

    if (offset >= strings_size)
        return 0;

    for (i = offset; i < strings_size; i++)
    {
        if (strings[i] == '\0')
            return (const char *)&strings[offset];
    }

    return 0;
}

static uint32_t cells_to_u32(const uint32_t *cells, uint32_t count)
{
    if (count == 0)
        return 0;

    /*
     * m68k addresses are 32-bit. For multi-cell FDT values, accept only
     * ranges whose high cells are zero and return the least significant one.
     */
    while (count > 1)
    {
        if (*cells++ != 0)
            return 0;
        count--;
    }

    return *cells;
}

static void parse_fdt(struct Emu68BootContext *ctx)
{
    const struct FdtHeader *header = ctx->fdt;
    const uint8_t *base = ctx->fdt;
    const uint8_t *structure;
    const uint8_t *structure_end;
    const uint8_t *strings;
    uint32_t address_cells = 1;
    uint32_t size_cells = 1;
    uint32_t depth = 0;
    int in_memory = 0;
    int in_emu68 = 0;
    int in_chosen = 0;

    if (!header || header->magic != FDT_MAGIC ||
        header->totalsize < sizeof(*header))
        return;

    if (header->off_dt_struct > header->totalsize ||
        header->size_dt_struct > header->totalsize - header->off_dt_struct ||
        header->off_dt_strings > header->totalsize ||
        header->size_dt_strings > header->totalsize - header->off_dt_strings)
        return;

    ctx->fdt_size = header->totalsize;
    ctx->flags |= EMU68_BOOT_FDT_VALID;

    structure = base + header->off_dt_struct;
    structure_end = structure + header->size_dt_struct;
    strings = base + header->off_dt_strings;

    while (structure + sizeof(uint32_t) <= structure_end)
    {
        uint32_t token = *(const uint32_t *)structure;
        structure += sizeof(uint32_t);

        if (token == FDT_BEGIN_NODE)
        {
            const char *name = (const char *)structure;
            const uint8_t *cursor = structure;

            while (cursor < structure_end && *cursor != '\0')
                cursor++;
            if (cursor == structure_end)
                return;

            depth++;
            in_memory = depth == 2 && node_is_memory(name, structure_end);
            in_emu68 = depth == 2 &&
                       bounded_string_equal(name, 6, "emu68");

            in_chosen = depth == 2 &&
                        bounded_string_equal(name,
                                             (uint32_t)(cursor - structure + 1),
                                             "chosen");
            structure += align4((uint32_t)(cursor - structure + 1));
        }
        else if (token == FDT_END_NODE)
        {
            if (depth == 0)
                return;
            if (depth == 2)
            {
                in_memory = 0;
            in_emu68 = 0;
                in_chosen = 0;
            }
            depth--;
        }
        else if (token == FDT_PROP)
        {
            uint32_t length;
            uint32_t name_offset;
            const char *name;
            const uint8_t *value;

            if (structure + 2 * sizeof(uint32_t) > structure_end)
                return;

            length = *(const uint32_t *)structure;
            name_offset = *(const uint32_t *)(structure + sizeof(uint32_t));
            structure += 2 * sizeof(uint32_t);
            if (length > (uint32_t)(structure_end - structure))
                return;

            value = structure;
            name = fdt_string(strings, header->size_dt_strings, name_offset);
            if (!name)
                return;

            if (depth == 1 && length == sizeof(uint32_t))
            {
                if (bounded_string_equal(name, 15, "#address-cells"))
                    address_cells = *(const uint32_t *)value;
                else if (bounded_string_equal(name, 12, "#size-cells"))
                    size_cells = *(const uint32_t *)value;
            }
            else if (in_emu68 &&
                     bounded_string_equal(name, 9, "host-mem") &&
                     length >= 2 * sizeof(uint32_t))
            {
                /*
                 * Emu68's own allocator lives in the same DRAM we are told we
                 * own, and this is where it says so. Without it the guest heap
                 * is built over the top of it and the two hand out the same
                 * addresses -- measured on 2026-08-13 as a sixteen-megabyte
                 * overlap.
                 *
                 * That overlap was real and this closes it, but it was not the
                 * cause of the heap corruption it was found while chasing; that
                 * was an undersized TLSF split (patches/aros/0011). Keeping the
                 * two claims apart matters, because the first was asserted as a
                 * fix on one run and withdrawn -- see
                 * AI_context/consolidated/history/ISSUE-0007.md.
                 */
                const uint32_t *cells = (const uint32_t *)value;

                ctx->host_mem_end = cells[1];
            }
            else if (in_memory &&
                     bounded_string_equal(name, 4, "reg") &&
                     length >= (address_cells + size_cells) * sizeof(uint32_t))
            {
                const uint32_t *cells = (const uint32_t *)value;
                uint32_t memory_base = cells_to_u32(cells, address_cells);
                uint32_t memory_size =
                    cells_to_u32(cells + address_cells, size_cells);

                if (memory_size != 0)
                {
                    ctx->memory_base = memory_base;
                    ctx->memory_size = memory_size;
                    ctx->flags |= EMU68_BOOT_MEMORY_VALID;
                }
            }
            else if (in_chosen &&
                     bounded_string_equal(name, 9, "bootargs") &&
                     length != 0)
            {
                ctx->bootargs = (const char *)value;
                ctx->bootargs_size = length;
                ctx->flags |= EMU68_BOOT_BOOTARGS_VALID;
            }

            structure += align4(length);
        }
        else if (token == FDT_NOP)
        {
            continue;
        }
        else if (token == FDT_END)
        {
            return;
        }
        else
        {
            return;
        }
    }
}

static void add_boot_tag(uint32_t *index, uint32_t tag, uint32_t data)
{
    emu68_boot_tags[*index].ti_Tag = tag;
    emu68_boot_tags[*index].ti_Data = data;
    (*index)++;
}

static void start_aros(struct Emu68BootContext *ctx)
{
    UWORD *ranges[3];
    struct MemHeader *memory;
    struct ExecBase *sys_base;
    void *user_stack;
    uint32_t lower;
    uint32_t upper;
    uint32_t tag_index = 0;

    if (!(ctx->flags & EMU68_BOOT_MEMORY_VALID))
        return;

    upper = ctx->memory_base + ctx->memory_size;
    if (upper < ctx->memory_base)
        return;

    /*
     * Keep the whole classic 24-bit address domain out of the allocator.
     *
     * That domain is not ours to hand out. Emu68 maps the advertised system
     * memory 1:1 and then punches holes in it -- 0x00dff000 for the custom
     * chip registers, 0xdeadb000 for its own debug port (src/aarch64/start.c)
     * -- and every access to a holed page is trapped and emulated as a device.
     * A heap that starts at 0x1000 contains both of them, so an ordinary
     * allocation can be handed a page whose every store goes to INTENA/INTREQ
     * instead of to memory. Nothing reserved them, because the heap was laid
     * out as if the address space were plain RAM.
     *
     * Starting above 0x00ffffff reserves them by construction, and it is the
     * precondition for the memory policy this port is moving to: the low 24
     * bits start inaccessible to direct loads and stores, and ranges are
     * promoted back to a direct mapping only once classified as normal memory.
     * See AI_context/issues/ISSUE-0016.md and docs/New_emu68.md section 6.
     *
     * The vector table and the absolute SysBase slot at address 4 fall out of
     * the allocator for free. Emu68 has already removed its FDT and the loaded
     * ELF from the top of the advertised memory range.
     */
    lower = ctx->memory_base;
    if (lower < 0x01000000)
        lower = 0x01000000;

    /*
     * And above Emu68's own pools.
     *
     * This is the one that mattered. Emu68 carves its allocator and its JIT
     * buffers out of the same DRAM it then advertises to us, and said nothing
     * about it; measured on 2026-08-13, its SYS pool ran 0x0088a000-0x01ffffff
     * while this heap started at 0x01000000, so sixteen megabytes had two
     * independent allocators handing out the same addresses. The guest
     * allocator's own free-list pointers came back as somebody else's data.
     *
     * host_mem_end is what Emu68 now publishes on /emu68. Zero means an Emu68
     * that does not publish it, and then this is no worse than before.
     */
    if (ctx->host_mem_end > lower)
        lower = ctx->host_mem_end;
    lower = (lower + 15) & ~15UL;

    if (upper <= lower || upper - lower < 0x10000)
        return;

    /*
     * One line for the heap, and it replaces two.
     *
     * The bring-up FDT dump named every top-level node and printed host-mem
     * raw, because at the time the question was whether the property arrived
     * at all. It does, and what is worth reading on an ordinary boot is not the
     * property but its consequence: where this heap actually begins and ends. A
     * base that is not above Emu68's pools, or an unexpected top, is visible
     * here without a debug build.
     */
    emu68_console_puts("[AROS/Emu68] heap 0x");
    put_hex32(lower);
    emu68_console_puts("-0x");
    put_hex32(upper - 1);
    emu68_console_puts("\n");

    add_boot_tag(&tag_index, KRN_KernelBase,
                 (uint32_t)__aros_resident_start);
    add_boot_tag(&tag_index, KRN_KernelLowest,
                 (uint32_t)__aros_resident_start);
    add_boot_tag(&tag_index, KRN_KernelHighest,
                 (uint32_t)__aros_resident_end);
    add_boot_tag(&tag_index, KRN_MEMLower, lower);
    add_boot_tag(&tag_index, KRN_MEMUpper, upper);
    add_boot_tag(&tag_index, KRN_OpenFirmwareTree, (uint32_t)ctx->fdt);
    if (ctx->flags & EMU68_BOOT_BOOTARGS_VALID)
        add_boot_tag(&tag_index, KRN_CmdLine, (uint32_t)ctx->bootargs);
    add_boot_tag(&tag_index, TAG_DONE, 0);

    /*
     * Assert the machine's trap contract, once.
     *
     * The classic 24-bit domain is meant to be inaccessible to direct loads
     * and stores, so that an access to it reaches machine semantics rather
     * than reading back whatever DRAM happens to be there. That is an
     * invariant, not an aspiration: "a hardware range intended to trap MUST
     * NOT simultaneously have a direct mapping that bypasses the fault path"
     * (docs/Bus.md section 5).
     *
     * Nothing in this port touches the classic domain in normal operation --
     * interrupts arrive as an IPL level, not through Paula, so even $DFF000 is
     * never written (see platform/platform.c and exec/dispatch.S). That is
     * correct, and it also means an instrument watching that domain would
     * report nothing whether the trap works or not. So read one address that
     * should trap and let the machine say it saw it.
     *
     * $E80000 is chosen because it is where autoconfig would be if this
     * machine had a Zorro bus; it has no other meaning here. The value is
     * discarded -- what is being tested is that the read is seen at all.
     */
    (void)*(volatile UWORD *)0x00e80000;

    BootMsg = emu68_boot_tags;
    memory = (struct MemHeader *)lower;
    krnCreateTLSFMemHeader("System Memory", 0, memory, upper - lower,
                           MEMF_CHIP | MEMF_FAST | MEMF_PUBLIC |
                           MEMF_KICK | MEMF_LOCAL);

    ranges[0] = (UWORD *)__aros_resident_start;
    ranges[1] = (UWORD *)__aros_resident_end;
    ranges[2] = (UWORD *)~0UL;

    sys_base = krnPrepareExecBase(ranges, memory, BootMsg);
    if (sys_base)
    {
        /*
         * Tell exec what Emu68 actually emulates.
         *
         * Nothing else sets this, so it was left at zero and exec believed it
         * was running on a bare 68000. That is not a cosmetic mistake: the
         * size of an exception stack frame depends on it. Emu68 emits frames
         * with a format word (src/M68k_Exception.c), i.e. 68010 and up, while
         * Exec_Supervisor_Entry (arch/m68k-all/exec/supervisor.S) pushes that
         * word only when AFF_68010 is set. With the flag clear, the fake frame
         * it builds is two bytes short, and the RTE that ends the supervisor
         * call returns to the wrong address.
         *
         * That path is reached whenever Permit() finds a switch pending and
         * calls KrnSchedule(), which is why it survived the timer/scheduler
         * selftest: preemption from an interrupt goes through this port's own
         * trampoline, which pushes and pops symmetrically and never consults
         * AttnFlags.
         *
         * The target is built -march=68040 to match (configure sets
         * gcc_default_cpu for this arch), so the code these flags select --
         * the cache and context routines in arch/m68k-all -- is built for the
         * CPU that is actually underneath.
         *
         * The FPU is claimed too. Emu68 emulates one, the target is built
         * against the toolchain's hard-float multilib, and
         * arch/m68k-all/kernel/fpu{save,restore}context.S already carry the
         * context handling that goes with saying so.
         */
        sys_base->AttnFlags |= AFF_68010 | AFF_68020 | AFF_68030 |
                               AFF_68040 | AFF_ADDR32 |
                               AFF_68881 | AFF_68882 | AFF_FPU40;

        /* Preserve the registers guaranteed by the public m68k Exec ABI. */
        m68k_ExecInstallPreserveAll(sys_base);
        ctx->exec_base = sys_base;
        ctx->flags |= EMU68_BOOT_EXEC_READY;
        emu68_set_stage(EMU68_STAGE_EXEC_READY);
        emu68_console_puts("[AROS/Emu68] ExecBase ready\n");

        emu68_set_stage(EMU68_STAGE_SINGLETASK);
        emu68_console_puts("[AROS/Emu68] InitCode SINGLETASK\n");
        InitCode(RTF_SINGLETASK, 0);
        ctx->flags |= EMU68_BOOT_KERNEL_READY;
        emu68_set_stage(EMU68_STAGE_KERNEL_READY);
        emu68_console_puts("[AROS/Emu68] kernel.resource ready\n");

        /*
         * Populate the m68k exception vectors.
         *
         * Until this runs, the only two vectors this port has ever written
         * are 8 (below) and 30 (level 6, from platform_timer_start()). Every
         * other vector holds whatever Emu68 left there. Any exception AROS
         * raises -- an illegal instruction, a trap, a divide by zero, an
         * unclaimed autovector -- then jumps to an address that was never a
         * function, and Emu68's JIT starts translating whatever it finds,
         * typically the vector table itself.
         *
         * M68KExceptionInit() points vectors 2..63 at M68KTrapHelper_10,
         * which routes into AROS's normal exception handling. The table
         * argument is only for per-vector overrides. arch/m68k-amiga uses it
         * for its seven autovector levels; we cover those the other way
         * arch/m68k-amiga also does, writing all seven vectors directly in
         * platform.c, so ours carries the fault vectors instead.
         *
         * Ordering matters and mirrors arch/m68k-amiga/boot/start.c:1026-1030
         * -- this overwrites vector 8, so Exec_Supervisor_Trap goes back in
         * afterwards, and level 6 is installed later still.
         */
        M68KExceptionInit(emu68_exception_table, sys_base);

        ((volatile void **)0)[8] = Exec_Supervisor_Trap;
        user_stack = AllocMem(64 * 1024, MEMF_PUBLIC | MEMF_CLEAR);
        if (user_stack)
            emu68_enter_user(coldstart_user, user_stack + 64 * 1024);

        emu68_console_puts("[AROS/Emu68] failed to allocate user stack\n");
    }
}

void emu68_bootstrap(const void *fdt, void *framebuffer, uint32_t pitch,
                     uint32_t width, uint32_t height)
{
    emu68_boot_context.magic = EMU68_BOOT_MAGIC;
    emu68_boot_context.abi_version = EMU68_BOOT_ABI;
    emu68_boot_context.flags = 0;
    emu68_boot_context.fdt = fdt;
    emu68_boot_context.fdt_size = 0;
    emu68_boot_context.framebuffer = framebuffer;
    emu68_boot_context.framebuffer_pitch = pitch;
    emu68_boot_context.framebuffer_width = width;
    emu68_boot_context.framebuffer_height = height;
    emu68_boot_context.memory_base = 0;
    emu68_boot_context.memory_size = 0;
    emu68_boot_context.bootargs = 0;
    emu68_boot_context.bootargs_size = 0;
    emu68_boot_context.exec_base = 0;
    emu68_set_stage(EMU68_STAGE_ENTRY);

    if (framebuffer && pitch && width && height)
        emu68_boot_context.flags |= EMU68_BOOT_FRAMEBUFFER;

    emu68_console_init(framebuffer, pitch, width, height);
    emu68_console_puts("[AROS/Emu68] native m68k bootstrap\n");

    parse_fdt(&emu68_boot_context);
    start_aros(&emu68_boot_context);

    for (;;)
        __asm__ volatile ("stop #0x2700");
}
