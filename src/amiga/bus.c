/*
 * Classic Amiga compatibility bus.
 *
 * Bellatrix translates an Emu68 fault into one public Rigel transaction.
 * Address decoding and all classic hardware semantics stay inside Rigel.
 */

#include "amiga/bus.h"
#include "amiga/console.h"
#include "amiga/core.h"
#include "amiga/frame.h"
#include "amiga/irq.h"
#include "machine/memory.h"
#include "machine/vecpage.h"

#include "A64.h"
#include "M68k.h"
#include "rigel/rigel.h"
#include "tlsf.h"

extern void *tlsf;
extern struct M68KState *__m68k_state;

static RigelContext *rigel;
static uint8_t unsupported_reported;
static volatile uint64_t pending_cck;
static uint8_t clock_reported;
static uint8_t census_reports;
static uint32_t vblank_count;
static uint8_t vblank_reports;
static volatile uint8_t chipset_observed;

enum { AMIGA_MAX_STEP_CCK = 512 };

#if defined(CONFIG_RIGEL_SELFTEST) && CONFIG_RIGEL_SELFTEST
static void amiga_bus_selftest(void);
static void amiga_bus_display_selftest(void);
#endif

static void amiga_bus_write(const MachineRegion *region, uint32_t address,
                            int size, uint32_t value);
static void amiga_chipset_enable_dma(void);

/*
 * What did Denise actually produce?
 *
 * There is no display yet, and on QEMU there is no HVS to build one on, so
 * this is the only way to tell a chipset that is rendering from one that is
 * merely running. It answers three separate questions that a blank screen
 * cannot: whether a frame was composed at all, whether anything in it is not
 * the background colour, and whether it changes.
 *
 * Deliberately cheap and bounded: a stride-sampled census, a handful of times,
 * so it can be left in a normal build.
 */
static void amiga_frame_census(void)
{
    /*
     * Report when the picture *changes*, not for the first few frames.
     *
     * The old rule -- six reports and then silence -- says whether Denise is
     * drawing during Emu68's boot, which is the one moment nothing is. By the
     * time a guest opens a screen the census has been quiet for minutes, so a
     * machine that starts rendering says nothing at all.
     *
     * The interesting event is the transition: a frame that was empty stops
     * being empty, or stops again. That is "something started drawing" and
     * "something stopped", and it is the whole question when the goal is to
     * find out whether an application's output reached the chipset. A slow
     * heartbeat underneath catches the case where it was already drawing.
     */
    enum { AMIGA_CENSUS_HEARTBEAT = 1000, AMIGA_CENSUS_STRIDE = 7 };
    static uint32_t census_frames;
    static uint8_t  census_was_drawing;
    static uint32_t census_last_background;
    rigel_frame_t frame;
    const uint32_t *row;
    uint32_t background;
    uint32_t non_background = 0;
    uint32_t checksum = 0;
    uint32_t sampled = 0;
    uint32_t x, y;

    if (!rigel_get_frame(rigel, &frame))
        return;
    if (frame.pixels == 0 || frame.width == 0 || frame.height == 0)
        return;
    if (frame.format != RIGEL_PIXEL_RGBA8888)
    {
        if (census_reports++ == 0)
            kprintf("[BELLATRIX:RIGEL:CENSUS] unexpected pixel format %d\n",
                (int)frame.format);
        return;
    }

    /*
     * The top-left visible pixel is the background: every Amiga display has
     * COLOR00 there unless the copper has been told otherwise, so it is the
     * cheapest reference for "this frame is not empty" that needs no knowledge
     * of what was programmed.
     */
    background = *(const uint32_t *)frame.pixels;

    for (y = 0; y < frame.height; y += AMIGA_CENSUS_STRIDE)
    {
        row = (const uint32_t *)((const uint8_t *)frame.pixels +
                                 (size_t)y * frame.pitch);
        for (x = 0; x < frame.width; x += AMIGA_CENSUS_STRIDE)
        {
            uint32_t pixel = row[x];

            sampled++;
            checksum = (checksum * 31u) + pixel;
            if (pixel != background)
                non_background++;
        }
    }

    {
        uint8_t drawing = non_background != 0;
        uint8_t report = 0;

        census_frames++;

        /*
         * The background colour is a signal in its own right, and missing it
         * cost a boot's worth of confusion.
         *
         * This counts pixels that differ from the top-left one, so a screen
         * filled with a single colour reads as empty however loudly it says
         * something happened. A frame that went from black to 0x00aaaaaa is
         * the Workbench grey: somebody programmed COLOR00, which means an
         * application's display reached the chipset. That is the event the
         * whole exercise is waiting for, and the count alone throws it away.
         */
        if (background != census_last_background)
        {
            census_last_background = background;
            report = 1;
            kprintf("[BELLATRIX:RIGEL:CENSUS] background is now %08x\n",
                (unsigned)background);
        }

        if (drawing != census_was_drawing)
        {
            census_was_drawing = drawing;
            report = 1;
            kprintf("[BELLATRIX:RIGEL:CENSUS] %s\n",
                drawing ? "something is drawing" : "the picture went empty");
        }
        else if (census_frames % AMIGA_CENSUS_HEARTBEAT == 0)
            report = 1;

        if (!report)
            return;
    }

    census_reports++;
    kprintf("[BELLATRIX:RIGEL:CENSUS] frame=%llu %ux%u pitch=%u bg=%08x "
            "non-bg=%u/%u sum=%08x flags=%02x\n",
        (unsigned long long)frame.frame_count,
        (unsigned)frame.width, (unsigned)frame.height, (unsigned)frame.pitch,
        (unsigned)background, (unsigned)non_background, (unsigned)sampled,
        (unsigned)checksum, (unsigned)frame.flags);
}

/*
 * What the chipset costs in real time.
 *
 * Every performance claim about this integration so far was made by timing a
 * whole run from outside and dividing, which averages the chipset together
 * with a boot that is mostly not chipset. On hardware that is worse: if the
 * machine is too slow to reach a Shell, the only thing that can report is the
 * serial line during boot. So measure here, where the exclusive cost is.
 *
 * Rigel's own AI_context asks for exactly this split before any chipset
 * optimisation is chosen: exclusive time inside the step, the number of calls,
 * and colour clocks per call. Many short calls mean the integration's
 * granularity is wrong; few long ones with high exclusive time mean the
 * internal path is.
 *
 * Two counter reads per call, on a call that advances up to 512 colour clocks.
 */
static uint64_t perf_ticks;
static uint64_t perf_cck;
static uint64_t perf_calls;
static uint64_t perf_next_report;
static uint32_t perf_reports;

static uint64_t amiga_perf_now(void)
{
    uint64_t t;

    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(t));
    return t;
}

static void amiga_perf_report(void)
{
    enum { AMIGA_PERF_EVERY_CCK = 4000000ull, AMIGA_PERF_REPORTS = 20 };
    uint64_t freq, ns, ns_per_cck, cck_per_s;

    if (perf_cck < perf_next_report)
        return;
    perf_next_report = perf_cck + AMIGA_PERF_EVERY_CCK;
    if (perf_reports >= AMIGA_PERF_REPORTS)
        return;
    perf_reports++;

    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    if (freq == 0 || perf_cck == 0 || perf_calls == 0)
        return;

    ns = (perf_ticks * 1000000000ull) / freq;
    ns_per_cck = ns / perf_cck;
    cck_per_s = ns ? (perf_cck * 1000000000ull) / ns : 0;

    /*
     * 3546895 CCK/s is realtime for PAL. The percentage is the number that
     * decides whether this machine can run a chipset at all; the colour clocks
     * per call is the number that says whose fault it is if it cannot.
     */
    kprintf("[BELLATRIX:RIGEL:PERF] %llu CCK in %llu ms over %llu calls -> "
            "%llu ns/CCK, %llu CCK/s (%llu%% of realtime), %llu CCK/call\n",
        (unsigned long long)perf_cck,
        (unsigned long long)(ns / 1000000ull),
        (unsigned long long)perf_calls,
        (unsigned long long)ns_per_cck,
        (unsigned long long)cck_per_s,
        (unsigned long long)((cck_per_s * 100ull) / 3546895ull),
        (unsigned long long)(perf_cck / perf_calls));
}

static void amiga_clock_step(rigel_cycle_t cycles)
{
    rigel_step_result_t result;
    uint64_t started;

    if (rigel == 0 || cycles == 0)
        return;

    started = amiga_perf_now();
    result = rigel_step(rigel, cycles);
    perf_ticks += amiga_perf_now() - started;
    perf_cck += cycles;
    perf_calls++;
    amiga_perf_report();
    if (!clock_reported)
    {
        kprintf("[BELLATRIX:RIGEL] clock active at %llu CCK\n",
            (unsigned long long)result.time);
        clock_reported = 1;
    }
    if ((result.events & RIGEL_EVENT_VBLANK) != 0)
    {
        /*
         * Report the first frame and then every 128th, a handful of times.
         * A machine whose CPU has gone idle keeps producing these only if
         * idle time reaches the chipset -- which is the whole point of the
         * STOP path in EMIT_STOP, and is otherwise invisible.
         */
        enum { AMIGA_VBLANK_REPORT_EVERY = 512, AMIGA_VBLANK_REPORTS = 8 };

        vblank_count++;
        if (vblank_reports < AMIGA_VBLANK_REPORTS &&
            (vblank_count == 1 ||
             vblank_count % AMIGA_VBLANK_REPORT_EVERY == 0))
        {
            vblank_reports++;
            kprintf("[BELLATRIX:RIGEL] VBLANK %u at %llu CCK\n",
                (unsigned)vblank_count, (unsigned long long)result.time);
        }
    }
    if ((result.events & RIGEL_EVENT_FRAME_READY) != 0)
    {
        amiga_frame_publish(rigel);
        amiga_frame_census();
    }
    amiga_irq_sync();
}

static rigel_cycle_t amiga_clock_quantum(void)
{
    rigel_cycle_t now = rigel_get_time(rigel);
    rigel_cycle_t next = rigel_get_next_observable_deadline(rigel);
    rigel_cycle_t quantum = AMIGA_MAX_STEP_CCK;

    if (next <= now)
        return 1;
    if (next - now < quantum)
        quantum = next - now;
    return quantum;
}

static void amiga_clock_consume(void)
{
    while (pending_cck != 0)
    {
        rigel_cycle_t quantum = amiga_clock_quantum();

        if (pending_cck < quantum)
            quantum = (rigel_cycle_t)pending_cck;
        amiga_clock_step(quantum);
        pending_cck -= quantum;
    }
}

/*
 * Where chipset time comes from: real time, and nothing else.
 *
 * This used to be a choice. CPU-driven time -- two modelled 68000 cycles per
 * colour clock -- gave a stock Amiga's exact ratio between the chipset and the
 * processor, and it is what capped the machine: a chipset sustaining 112% of
 * realtime held the guest to 7.99 MHz-equivalent and AROS took thirteen
 * minutes to boot (ISSUE-0068). Paying for that ratio meant carrying an
 * instruction-cost model in the JIT, a cycle counter written by every
 * translated instruction, and a STOP that yielded instead of sleeping so the
 * count would keep moving. All of it existed to hand the chipset a number the
 * chipset no longer wants.
 *
 * What is left is an accelerated Amiga: the chipset advances by real elapsed
 * nanoseconds on a core of its own, and the CPU never waits for it. A guest
 * gets its VBLANK every 20 ms of real time, which is more correct in real
 * terms than the coupled mode ever gave it. What it gives up is the ratio --
 * the defining property of every accelerator ever sold for this machine,
 * rather than something wrong with this one.
 *
 * Not deferred CPU cycles. That reading sounds like the same idea and is not:
 * the same total work happens in bursts and the machine stays throttled.
 *
 * A stopped CPU needs nothing from this. Real time does not stop with it, and
 * the chipset core raises the IPL and sends the event that ends the STOP, so
 * the guest parks in WFE exactly as it would on a real machine.
 *
 * ISSUE-0075.
 */
static uint64_t wall_last;
static uint64_t wall_remainder;

/* Colour clocks per second, PAL. NTSC differs by 0.1% and nothing here cares. */
#define AMIGA_CCK_PER_SECOND 3546895ull

/*
 * How far behind the chipset may fall before the rest is forgiven.
 *
 * A long JIT translation, a serial write or a scheduler gap leaves real time
 * running while nothing steps the chipset. Replaying all of it in one burst
 * would reintroduce exactly the stall this mode removes -- the legacy notes
 * call it an expensive catch-up burst -- so cap it at about one frame and let
 * the machine slip rather than freeze. A guest cannot tell a dropped frame from
 * a slow one; it can very much tell a machine that stops.
 */
#define AMIGA_CCK_MAX_CATCHUP 80000ull

static void amiga_clock_advance_wall(void)
{
    uint64_t now, freq, elapsed, scaled;

    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    if (freq == 0)
        return;
    now = amiga_perf_now();
    if (wall_last == 0)
    {
        wall_last = now;
        return;
    }
    elapsed = now - wall_last;
    wall_last = now;

    /* ticks * CCK/s / Hz, with the remainder carried so it does not drift. */
    scaled = elapsed * AMIGA_CCK_PER_SECOND + wall_remainder;
    pending_cck += scaled / freq;
    wall_remainder = scaled % freq;

    if (pending_cck > AMIGA_CCK_MAX_CATCHUP)
        pending_cck = AMIGA_CCK_MAX_CATCHUP;
}

static void amiga_clock_flush(void)
{
    /*
     * Nothing to flush when the chipset has a core of its own: it is already
     * current, continuously, which is better than the flush ever was. The
     * whole point of the flush was that a register read must not see a
     * chipset frozen since the last time the CPU happened to step it.
     */
    if (amiga_core_owns_chipset())
        return;
    amiga_clock_advance_wall();
    amiga_clock_consume();
}

/*
 * Does this access mean the chipset has to be running?
 *
 * Not every transaction does, and the difference is worth about 4x. AROS's
 * ordinary boot reads the classic domain twice without wanting anything of
 * it -- dosboot samples CIAA.PRA and POTINP for the classic boot buttons,
 * battclock probes the RTC -- and arming on the first access of any kind
 * turned both into a machine that runs the chipset for the rest of the boot.
 * The cost is not subtle: gfx.hidd's init went from milliseconds to 10964 ms.
 *
 * So arm on what actually needs time to pass:
 *
 *   - any write to the custom chips, which is how DMA, the copper, the
 *     blitter and audio are started, and
 *   - any write to a CIA, which is how its timers are started, and
 *   - a read of the beam position, which is meaningless if it never moves.
 *
 * A read of a button, a port or a clock is answered from state Rigel already
 * holds and needs no time at all.
 */
static int amiga_access_needs_time(uint32_t address, int write)
{
    enum
    {
        CUSTOM_BASE = 0x00dff000u,
        CUSTOM_END  = 0x00e00000u,
        CIA_BASE    = 0x00bfd000u,
        CIA_END     = 0x00bff000u,
        CUSTOM_VPOSR  = 0x00dff004u,
        CUSTOM_VHPOSR = 0x00dff006u
    };

    if (write)
        return (address >= CUSTOM_BASE && address < CUSTOM_END) ||
               (address >= CIA_BASE && address < CIA_END);

    return (address & ~1u) == CUSTOM_VPOSR ||
           (address & ~1u) == CUSTOM_VHPOSR;
}

static void amiga_clock_observe(uint32_t address, int write)
{
    if (!chipset_observed)
    {
        if (!amiga_access_needs_time(address, write))
            return;

        /*
         * Before this the chipset has no observer and no programmed
         * asynchronous work. Start its time domain here rather than replaying
         * the whole CPU boot as an expensive catch-up burst.
         */
        /*
         * Only the owner touches the clock's own state.
         *
         * This runs on the CPU core, and when the chipset has a core of its
         * own that core is inside amiga_clock_advance_wall() reading and
         * writing exactly these words. Resetting wall_last from here races it,
         * and the visible result is not a crash but a wrong elapsed time --
         * one huge delta, clamped to the catch-up cap, and then a chipset core
         * that holds the lock for a whole frame of work while the CPU waits.
         *
         * The chipset core initialises its own reference on first use, so
         * there is nothing to hand it.
         */
        if (!amiga_core_owns_chipset())
        {
            pending_cck = 0;
            wall_last = amiga_perf_now();
            wall_remainder = 0;
        }
        chipset_observed = 1;
        kprintf("[BELLATRIX:RIGEL] clock armed by %s $%08x\n",
            write ? "a write to" : "a read of", (unsigned)address);
    }
    amiga_clock_flush();
}

static void amiga_rigel_log(const char *message, void *opaque)
{
    (void)opaque;
    kprintf("[rigel] %s\n", message);
}

static void amiga_rigel_log_event(const rigel_log_event_t *event, void *opaque)
{
    /*
     * Bounded, because the serial line is the boot's bottleneck under QEMU and
     * this is per chipset event. A running display produces them by the
     * hundred thousand -- one boot with the chipset display driver installed
     * emitted 170274 compose events and never reached a Shell, which reads
     * exactly like a hang and is not one.
     */
    enum { AMIGA_LOG_EVENT_LIMIT = 64 };
    static uint32_t logged;
    rigel_u8 i;

    (void)opaque;
    if (event == 0)
        return;
    if (logged >= AMIGA_LOG_EVENT_LIMIT)
        return;
    if (++logged == AMIGA_LOG_EVENT_LIMIT)
    {
        kprintf("[rigel] event log silenced after %u events\n",
            (unsigned)AMIGA_LOG_EVENT_LIMIT);
        return;
    }

    kprintf("[rigel] event=%s", event->name ? event->name : "unknown");
    for (i = 0; i < event->field_count && i < 4; ++i)
        kprintf(" f%d=%08x", i, event->fields[i]);
    kprintf("\n");
}

static void *amiga_bus_alloc(size_t size, void *opaque)
{
    (void)opaque;
    return tlsf_malloc(tlsf, size);
}

static void amiga_bus_free(void *ptr, void *opaque)
{
    (void)opaque;
    tlsf_free(tlsf, ptr);
}

static rigel_u16 amiga_chip_ram_read16(void *opaque, rigel_u32 address)
{
    (void)opaque;
    return machine_chip_ram_read16(address);
}

/*
 * Every chipset DMA write to chip RAM funnels through here -- blitter, copper,
 * sprites, bitplanes, disk, audio -- so it is the one place that can say
 * whether the chipset ever writes somewhere it must not.
 *
 * There is such a place, and it is not a matter of taste. AROS m68k links
 * `SysBase` and `AbsExecBase` as absolute address 4
 * (`arch/m68k-emu68/boot/mmakefile.src`: `--defsym,SysBase=0x4`), so every one
 * of the 3011 `moveal SysBase,%a6` in the kernel reads a longword out of the
 * first page of chip RAM. That page is why AMIGA_CHIP_RAM_ALLOC_BASE is
 * 0x1000 and not 0: nothing allocates below it, so nothing should ever write
 * there either.
 *
 * A chipset write below that floor overwrites ExecBase's address, and the
 * machine then dies at whatever library call happens to come next -- a wild
 * `jsr -LVO(A6)` with an A6 that was correct when it was read and garbage by
 * the time it was used. That is exactly the crash under ISSUE-0082, whose A6
 * came back as 0x0200011b, 0x020000b1 and 0x02000121 on three runs: not
 * random, and not a pointer, but whatever the chipset was moving at the time.
 *
 * Report the address and the value rather than only a count. The value is the
 * evidence of which unit is responsible: a copper instruction, a sprite word
 * and a run of blitter output do not look alike.
 */
static void amiga_chip_ram_write16(void *opaque, rigel_u32 address,
                                   rigel_u16 value)
{
    (void)opaque;

    if (address < AMIGA_CHIP_RAM_ALLOC_BASE)
    {
        static unsigned reported;

        if (reported < 16u)
        {
            reported++;
            kprintf("[BELLATRIX:RIGEL:LOWCHIP] write $%04x to $%06x"
                    " -- below the allocation floor ($%06x)%s\n",
                    (unsigned)value, (unsigned)address,
                    (unsigned)AMIGA_CHIP_RAM_ALLOC_BASE,
                    (address >= 4u && address < 8u)
                        ? "  <- this is AbsExecBase" : "");
        }
    }

    machine_chip_ram_write16(address, value);
}

void amiga_bus_init(void)
{
    rigel_config_t config = { 0 };

    config.alloc_fn = amiga_bus_alloc;
    config.free_fn = amiga_bus_free;
    config.log_fn = amiga_rigel_log;
    config.log_event_fn = amiga_rigel_log_event;
    config.chip_ram_size = AMIGA_CHIP_RAM_SIZE;
    config.chip_ram.read16 = amiga_chip_ram_read16;
    config.chip_ram.write16 = amiga_chip_ram_write16;

    /*
     * Say that this image carries the instruments, before using them.
     *
     * A guard that never fires and a guard that was never flashed produce the
     * same log, and reading the first as the second is how an afternoon goes
     * the wrong way (CLAUDE.md says the same thing about boot probes). One
     * line at init makes the silence mean something.
     */
    kprintf("[BELLATRIX:RIGEL] guards armed: low-chip writes, AbsExecBase\n");

    rigel = rigel_create(&config);
    if (rigel == 0)
        kprintf("[BELLATRIX:RIGEL] initialization failed\n");
    else
    {
        /*
         * The console sink first: from the next line onward a second core may
         * print, and it must not do that straight into the UART.
         */
#if !defined(CONFIG_RIGEL_CONSOLE_SINK) || CONFIG_RIGEL_CONSOLE_SINK
        amiga_console_init();
#endif
        amiga_irq_init(rigel);
        /*
         * Ask for the chipset's core now. The secondary core reaches its entry
         * point during Emu68's boot, and one that finds the flag clear parks
         * for good -- so this cannot wait until something arms the clock.
         */
        amiga_core_enable();
        kprintf("[BELLATRIX:RIGEL] enabled; address decode owned by Rigel\n");
        amiga_chipset_enable_dma();
#if defined(CONFIG_RIGEL_SELFTEST) && CONFIG_RIGEL_SELFTEST
        amiga_bus_selftest();
        amiga_bus_display_selftest();
#endif
    }
}

static uint32_t open_bus_value(int size)
{
    switch (size)
    {
        case 1:  return 0xffu;
        case 2:  return 0xffffu;
        default: return 0xffffffffu;
    }
}

static uint32_t amiga_bus_read(const MachineRegion *region, uint32_t address,
                               int size)
{
    rigel_mmio_result_t result;
    uint32_t value = open_bus_value(size);

    (void)region;
    if (rigel == 0)
        return value;

    amiga_clock_observe(address, 0);
    amiga_core_lock_acquire();
    result = rigel_mmio_read(rigel, address, (rigel_u8)size, &value);
    /* Only the owner may ask Rigel for the level; see src/amiga/irq.c. */
    if (!amiga_core_owns_chipset())
        amiga_irq_sync();
    amiga_core_lock_release();

    if (result == RIGEL_MMIO_UNSUPPORTED)
    {
        value = open_bus_value(size);
        if (!unsupported_reported)
        {
            unsupported_reported = 1;
            kprintf("[BELLATRIX:RIGEL] unsupported R%d at %08x\n",
                    size * 8, address);
        }
    }

    return value;
}

/*
 * The master DMA enable, which on this machine nobody else is going to set.
 *
 * DMACON bit 9 gates every other DMA channel: with it clear the Copper does
 * not run, no bitplane is fetched and the blitter does not move a word, no
 * matter what the per-channel bits say. Nothing in the whole AROS tree ever
 * sets it -- amigavideo's initcustom writes 0x80E0 (COPEN|BLTEN|SPREN), its
 * compositor writes 0x8100 (BPLEN), trackdisk writes 0x8010 (DSKEN), and every
 * one of them assumes the master enable is already on because on an Amiga the
 * Kickstart left it on before graphics.library ever ran.
 *
 * This machine has no Kickstart, so that assumption has no one to satisfy it.
 * It was accidentally satisfied for a while by the display selftest, which
 * wrote 0x8380 on its way to drawing a test pattern -- so when the selftest
 * was switched off the chipset went quiet, and the symptom was a 320x256x5
 * Amiga screen that opened correctly and composed nothing at all:
 *
 *     [BELLATRIX:RIGEL:CENSUS] frame=5000 ... non-bg=0/1369 flags=00
 *
 * flags carries RIGEL_FRAME_COPPER_ACTIVE, and it was clear on every frame of
 * the session: the Copper had not executed a single MOVE.
 *
 * So set it here, alone, as the one thing a Kickstart would have left behind.
 * Only bit 9 and SETCLR: every channel stays off until its own owner turns it
 * on, which is what the rest of the system already expects.
 */
static void amiga_chipset_enable_dma(void)
{
    enum
    {
        CUSTOM_DMACON  = 0x00dff096u,
        DMACON_SETCLR  = 0x8000u,
        DMACON_DMAEN   = 0x0200u
    };

    amiga_bus_write(0, CUSTOM_DMACON, 2, DMACON_SETCLR | DMACON_DMAEN);
    kprintf("[BELLATRIX:RIGEL] DMA master enable set"
            " (no Kickstart does it here)\n");
}

static void amiga_bus_write(const MachineRegion *region, uint32_t address,
                            int size, uint32_t value)
{
    rigel_mmio_result_t result;

    /* Watch DMACON on its way past; see amiga_frame_note_dmacon(). */
    if (address == 0x00dff096u && size == 2)
        amiga_frame_note_dmacon(value);

    (void)region;
    if (rigel == 0)
        return;

    amiga_clock_observe(address, 1);
    amiga_core_lock_acquire();
    result = rigel_mmio_write(rigel, address, (rigel_u8)size, value);
    if (!amiga_core_owns_chipset())
        amiga_irq_sync();
    amiga_core_lock_release();
    if (result == RIGEL_MMIO_UNSUPPORTED && !unsupported_reported)
    {
        unsupported_reported = 1;
        kprintf("[BELLATRIX:RIGEL] unsupported W%d at %08x\n",
                size * 8, address);
    }

}

const MachineRegionOps amiga_bus_ops =
{
    .read = amiga_bus_read,
    .write = amiga_bus_write,
};

#if defined(CONFIG_RIGEL_SELFTEST) && CONFIG_RIGEL_SELFTEST
/*
 * Wait for the chipset to reach a point in its own time.
 *
 * Both selftests below used to say "publish N CPU cycles" and read the answer
 * on the next line. That worked while chipset time was minted from CPU cycles
 * by the very thread that published them. It is not any more: the chipset runs
 * against the wall on a core of its own (ISSUE-0075), so publishing progress
 * advances nothing and the read lands wherever the other core happens to be.
 *
 * Every FAIL these tests reported after that change was this and not the
 * hardware -- the beam had moved, the CIA had counted, INTREQ held VERTB and
 * the IPL was 3 in all of them; only "time >= 1000" was short, at 877 to 950.
 * The display test was worse off, because it is silent: it composed 33112 CCK
 * in one boot and 397440 in another, so the frame it left in the aperture was
 * half a picture one time and six pictures the next, and the census had no way
 * to say which.
 *
 * So ask for the time and wait for it. A ceiling in real time keeps a chipset
 * that is genuinely stuck from hanging the boot instead of reporting.
 */
static rigel_cycle_t amiga_selftest_time(void)
{
    rigel_cycle_t now;

    amiga_core_lock_acquire();
    now = rigel_get_time(rigel);
    amiga_core_lock_release();
    return now;
}

static rigel_cycle_t amiga_selftest_wait_cck(rigel_cycle_t target,
    uint32_t timeout_ms)
{
    uint64_t freq, deadline;
    rigel_cycle_t now;

    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
    deadline = amiga_perf_now() + (freq / 1000ull) * (uint64_t)timeout_ms;

    for (;;)
    {
        now = amiga_selftest_time();
        if (now >= target || amiga_perf_now() >= deadline)
            return now;
        __asm__ volatile("yield");
    }
}

static void amiga_bus_selftest(void)
{
    enum
    {
        CIAA_TALO = 0x00bfe401u,
        CIAA_TAHI = 0x00bfe501u,
        CIAA_CRA = 0x00bfee01u,
        CUSTOM_VHPOSR = 0x00dff006u,
        CUSTOM_INTENA = 0x00dff09au,
        INT_MASTER_VERTB = 0xc020u,
        INTREQ_VERTB = 0x0020u
    };
    uint32_t beam_before;
    uint32_t beam_after;
    uint32_t timer_after;
    int passed;

    kprintf("[BELLATRIX:RIGEL:SELFTEST] starting\n");
    amiga_bus_write(0, CUSTOM_INTENA, 2, INT_MASTER_VERTB);
    amiga_bus_write(0, CIAA_TALO, 1, 0xffu);
    amiga_bus_write(0, CIAA_TAHI, 1, 0x00u);
    amiga_bus_write(0, CIAA_CRA, 1, 0x11u); /* force load + start */
    beam_before = amiga_bus_read(0, CUSTOM_VHPOSR, 2);

    amiga_selftest_wait_cck(1000u, 500u);

    beam_after = amiga_bus_read(0, CUSTOM_VHPOSR, 2);
    timer_after = amiga_bus_read(0, CIAA_TALO, 1);
    passed = rigel_get_time(rigel) >= 1000u &&
        beam_after != beam_before && timer_after != 0xffu &&
        (rigel_get_intreq(rigel) & INTREQ_VERTB) != 0u &&
        amiga_irq_get_ipl() == 3u;

    kprintf("[BELLATRIX:RIGEL:SELFTEST] %s time=%llu beam=%04x->%04x "
            "cia_ta=%02x intreq=%04x ipl=%u\n",
        passed ? "PASS" : "FAIL",
        (unsigned long long)rigel_get_time(rigel),
        beam_before, beam_after, timer_after,
        rigel_get_intreq(rigel), amiga_irq_get_ipl());
    rigel_reset(rigel);
    amiga_irq_init(rigel);
}
#endif

#if defined(CONFIG_RIGEL_SELFTEST) && CONFIG_RIGEL_SELFTEST
/*
 * A known producer for the display path.
 *
 * Phase 1 of the bring-up in AI_context/issues/ISSUE-0068.md. Nothing on this
 * machine programs Denise, so a census of a running chipset reports an empty
 * frame forever and there is no way to tell that from a broken one. This
 * programs the smallest complete display -- one bitplane, two colours, a
 * pattern whose result is known in advance -- so that the census has something
 * to be right or wrong about.
 *
 * It runs before AROS is loaded, so chip RAM is ours.
 */
static void amiga_bus_display_selftest(void)
{
    enum
    {
        CUSTOM_DIWSTRT  = 0x00dff08eu,
        CUSTOM_DIWSTOP  = 0x00dff090u,
        CUSTOM_DDFSTRT  = 0x00dff092u,
        CUSTOM_DDFSTOP  = 0x00dff094u,
        CUSTOM_DMACON   = 0x00dff096u,
        CUSTOM_COP1LCH  = 0x00dff080u,
        CUSTOM_COP1LCL  = 0x00dff082u,
        CUSTOM_COPJMP1  = 0x00dff088u,
        CUSTOM_BPL1PTH  = 0x00dff0e0u,
        CUSTOM_BPL1PTL  = 0x00dff0e2u,
        CUSTOM_BPLCON0  = 0x00dff100u,
        CUSTOM_BPLCON1  = 0x00dff102u,
        CUSTOM_BPL1MOD  = 0x00dff108u,
        CUSTOM_COLOR00  = 0x00dff180u,
        CUSTOM_COLOR01  = 0x00dff182u,

        BITPLANE_BASE   = 0x00010000u,  /* clear of the vectors, AROS not loaded */
        COPPERLIST_BASE = 0x00020000u,
        BITPLANE_WIDTH  = 320u,
        BITPLANE_HEIGHT = 256u,
        BITPLANE_STRIDE = BITPLANE_WIDTH / 8u,

        /* SET | DMAEN | BPLEN | COPEN */
        DMACON_ENABLE   = 0x8380u,
        /* one bitplane, colour burst on */
        BPLCON0_1BPL    = 0x1200u,
        /* the standard PAL full-screen window */
        DIWSTRT_STD     = 0x2c81u,
        DIWSTOP_STD     = 0x2cc1u,
        DDFSTRT_LORES   = 0x0038u,
        DDFSTOP_LORES   = 0x00d0u
    };
    uint32_t y, x;

    kprintf("[BELLATRIX:RIGEL:DISPLAY] programming one bitplane\n");

    /* Vertical stripes: every other word set, so half the pixels are COLOR01. */
    for (y = 0; y < BITPLANE_HEIGHT; ++y)
    {
        uint32_t row = BITPLANE_BASE + y * BITPLANE_STRIDE;

        for (x = 0; x < BITPLANE_STRIDE; x += 2u)
            machine_chip_ram_write16(row + x, (x & 2u) ? 0x0000u : 0xffffu);
    }

    amiga_bus_write(0, CUSTOM_COLOR00, 2, 0x0000u);   /* black   */
    amiga_bus_write(0, CUSTOM_COLOR01, 2, 0x0fffu);   /* white   */
    amiga_bus_write(0, CUSTOM_BPLCON0, 2, BPLCON0_1BPL);
    amiga_bus_write(0, CUSTOM_BPLCON1, 2, 0x0000u);
    amiga_bus_write(0, CUSTOM_BPL1MOD, 2, 0x0000u);
    amiga_bus_write(0, CUSTOM_DIWSTRT, 2, DIWSTRT_STD);
    amiga_bus_write(0, CUSTOM_DIWSTOP, 2, DIWSTOP_STD);
    amiga_bus_write(0, CUSTOM_DDFSTRT, 2, DDFSTRT_LORES);
    amiga_bus_write(0, CUSTOM_DDFSTOP, 2, DDFSTOP_LORES);
    amiga_bus_write(0, CUSTOM_BPL1PTH, 2, (BITPLANE_BASE >> 16) & 0xffffu);
    amiga_bus_write(0, CUSTOM_BPL1PTL, 2, BITPLANE_BASE & 0xffffu);

    /*
     * A copper list, because BPL1PT is not reloaded between frames.
     *
     * Agnus advances the bitplane pointer as it fetches and leaves it past the
     * end of the data; on real hardware it is the copper that rewrites it every
     * vertical blank, which is why every Amiga display has one. Without this
     * the first frame is correct, the second is partial and the rest are blank
     * -- which is exactly what the census reported before this was added.
     */
    machine_chip_ram_write16(COPPERLIST_BASE + 0u,  0x00e0u);  /* BPL1PTH */
    machine_chip_ram_write16(COPPERLIST_BASE + 2u,  (BITPLANE_BASE >> 16) & 0xffffu);
    machine_chip_ram_write16(COPPERLIST_BASE + 4u,  0x00e2u);  /* BPL1PTL */
    machine_chip_ram_write16(COPPERLIST_BASE + 6u,  BITPLANE_BASE & 0xffffu);
    machine_chip_ram_write16(COPPERLIST_BASE + 8u,  0xffffu);  /* end */
    machine_chip_ram_write16(COPPERLIST_BASE + 10u, 0xfffeu);

    amiga_bus_write(0, CUSTOM_COP1LCH, 2, (COPPERLIST_BASE >> 16) & 0xffffu);
    amiga_bus_write(0, CUSTOM_COP1LCL, 2, COPPERLIST_BASE & 0xffffu);
    amiga_bus_write(0, CUSTOM_DMACON,  2, DMACON_ENABLE);
    amiga_bus_write(0, CUSTOM_COPJMP1, 2, 0x0000u);

    /*
     * Two whole PAL frames, so the aperture holds a complete picture composed
     * with the display already programmed -- not the front of one.
     *
     * 227 colour clocks a line, 312 lines a frame. Counted in the chipset's
     * own time, because that is the only clock that says whether a frame
     * happened; two seconds is a generous ceiling for 141648 CCK at the rate
     * this machine actually runs, about three million a second.
     */
    {
        enum { PAL_CCK_PER_FRAME = 227u * 312u };
        rigel_cycle_t start = amiga_selftest_time();
        rigel_cycle_t want = start + 2u * PAL_CCK_PER_FRAME;
        rigel_cycle_t reached = amiga_selftest_wait_cck(want, 2000u);

        if (reached < want)
            kprintf("[BELLATRIX:RIGEL:DISPLAY] only %llu of %u CCK in 2 s; "
                    "the frame below is incomplete\n",
                (unsigned long long)(reached - start),
                (unsigned)(2u * PAL_CCK_PER_FRAME));
    }

    kprintf("[BELLATRIX:RIGEL:DISPLAY] %llu CCK stepped; census above\n",
        (unsigned long long)rigel_get_time(rigel));

    /*
     * Park the clock, keeping the frame.
     *
     * The point of this selftest is a known frame in the aperture, not a
     * chipset that keeps running: stepping Rigel costs enough under QEMU that
     * a boot which keeps doing it never reaches a Shell, and a Shell is where
     * anything can read the aperture back. The published frame stays where it
     * is; the next MMIO re-arms the clock as usual.
     */
    pending_cck = 0;
    chipset_observed = 0;
    kprintf("[BELLATRIX:RIGEL:DISPLAY] clock parked; frame kept for the guest\n");
}
#endif


/*
 * The chipset's own core.
 *
 * Runs Rigel against real time, forever, holding the lock only while stepping
 * so the CPU core can get in between quanta. It spins rather than sleeping:
 * this core exists to keep a realtime chipset, and there is nothing that could
 * usefully wake it that is not simply the passage of time.
 *
 * Before the chipset is armed there is nothing to run and the loop is idle --
 * that is the same laziness the single-core path has, and it is what keeps a
 * machine that never touches the chipset from paying for one.
 */
void amiga_clock_run_on_core(void)
{
    uint32_t reported = 0;

    for (;;)
    {
        /*
         * A software watchpoint on AbsExecBase.
         *
         * AROS m68k links SysBase and AbsExecBase as absolute address 4, so
         * the longword at chip RAM offset 4 is what every `moveal SysBase,%a6`
         * in the kernel reads. Under ISSUE-0082 it changes, and the machine
         * then dies at whatever library call comes next.
         *
         * It cannot be caught where the write happens. Chip RAM is a
         * MACHINE_REGION_DIRECT mapping (machine.c), so the CPU writes it with
         * native stores that reach no hook of ours -- which is also why the
         * chipset-side guard in amiga_chip_ram_write16() stayed silent: the
         * write is not chipset DMA.
         *
         * What this core can do is watch. It is already spinning, the check is
         * a load and a compare, and it holds the one thing the crash report
         * cannot give: the m68k PC at the moment the value changes, sampled
         * from outside while the CPU is still running. The crash names the
         * first library call after the write; this names the write.
         *
         * The latch takes the first non-zero value seen, which is exec
         * installing itself. That value is also worth printing on its own --
         * it is the true SysBase, and three readings of the corrupted one all
         * kept the high word 0x0200 and changed only the low, which says the
         * damage is a word-sized store to address 6 rather than a longword.
         */
        {
            static uint32_t absexec_latch;
            static uint32_t absexec_reports;
            uint32_t absexec =
                ((uint32_t)machine_chip_ram_read16(4) << 16) |
                machine_chip_ram_read16(6);

            if (absexec_latch == 0 && absexec != 0)
            {
                absexec_latch = absexec;
                kprintf("[BELLATRIX:RIGEL:ABSEXEC] AbsExecBase is $%08x\n",
                        (unsigned)absexec);
            }
            else if (absexec != absexec_latch && absexec_reports < 8u)
            {
                absexec_reports++;
                /*
                 * Say whether the vector page is trapped, here, on the line
                 * that always prints beside the crash.
                 *
                 * The banner for that lives in parse_cmdline, which runs
                 * before anything a serial capture started by hand is likely
                 * to contain -- so a run came back with no [VEC-W] lines and
                 * no way to tell an unarmed trap from a silent one. Restating
                 * it where the interesting output is makes that unreadable
                 * result impossible.
                 */
                kprintf("[BELLATRIX:RIGEL:ABSEXEC] $%08x -> $%08x"
                        " with the m68k at pc=%08x sr=%04x (vecpage %s)\n",
                        (unsigned)absexec_latch, (unsigned)absexec,
                        (unsigned)(__m68k_state != 0 ? __m68k_state->PC : 0),
                        (unsigned)(__m68k_state != 0 ? __m68k_state->SR : 0),
                        machine_vecpage_trapped() ? "ON" : "OFF");
                absexec_latch = absexec;
            }
        }

        /*
         * The only observer that survives an m68k that has stopped.
         *
         * A boot whose clock freezes while the serial log keeps flowing is a
         * CPU that stopped taking interrupts, and nothing on the CPU's own
         * side can report that -- it is the thing that is not running. This
         * core is, so it says what the CPU's interrupt state looks like from
         * outside, once a second, for free.
         *
         * INTF.ARM is the platform latch: 1 with the machine dead means the
         * interrupt was delivered and never taken. INTF.IPL is the chipset
         * level. SR carries the guest's own mask, and a stopped machine
         * holding IPL 7 is a Disable() that never ended rather than a lost
         * wakeup. The three together separate every candidate we have.
         */
        {
            static uint64_t live_last;
            static uint32_t live_reports;
            uint64_t freq, now;

            __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(freq));
            now = amiga_perf_now();
            if (freq != 0 && (live_last == 0 || now - live_last >= freq))
            {
                live_last = now;
                if (__m68k_state != 0 && ++live_reports > 8)
                {
                    /*
                     * PC is the datum that ends the guessing, and it took too
                     * long to print. sr=2610 says the CPU is in supervisor
                     * mode at IPL 6 and says nothing about where; three
                     * mechanisms were proposed and rejected on that alone.
                     *
                     * The JIT keeps PC in a register and writes it to the
                     * context on every save, so this is the last address the
                     * CPU left translated code at. A machine that is stuck
                     * shows the same handful of addresses forever; a machine
                     * that is merely busy does not.
                     */
                    kprintf("[BELLATRIX:LIVE] pc=%08x sr=%04x arm=%u ipl=%u\n",
                        (unsigned)__m68k_state->PC,
                        (unsigned)__m68k_state->SR,
                        (unsigned)__m68k_state->INTF.ARM,
                        (unsigned)__m68k_state->INTF.IPL);
                }
            }
        }

        if (rigel == 0 || !chipset_observed)
        {
            __asm__ volatile("yield" ::: "memory");
            continue;
        }

        if (!reported)
        {
            reported = 1;
            kprintf("[BELLATRIX:RIGEL:CORE] chipset running here now\n");
        }

        /*
         * A budget of work per acquisition, not a whole drain and not one
         * quantum.
         *
         * Draining the whole backlog holds the lock for up to 80000 colour
         * clocks -- 20 ms at the measured 250 ns each -- and every MMIO the
         * CPU issues waits behind it, which at a serial console is
         * indistinguishable from a deadlock.
         *
         * One quantum per acquisition is worse, and hardware said so. The
         * quantum is the distance to the next observable deadline, and when
         * that is close -- which it is whenever anything is programmed --
         * amiga_clock_quantum() returns 1. The machine then paid a full lock
         * round trip per colour clock and never advanced: the log reported
         * "1 CCK/call" where it had been 227.
         *
         * So keep the inner loop, and bound it by work instead: about a
         * millisecond of chipset time per acquisition, which is far below what
         * an MMIO can wait for and far above the cost of taking the lock.
         */
        enum { AMIGA_LOCK_BUDGET_CCK = 4096 };
        rigel_cycle_t budget = AMIGA_LOCK_BUDGET_CCK;

        amiga_core_lock_acquire();
        amiga_clock_advance_wall();
        while (pending_cck != 0 && budget != 0)
        {
            rigel_cycle_t quantum = amiga_clock_quantum();

            if (pending_cck < quantum)
                quantum = (rigel_cycle_t)pending_cck;
            if (budget < quantum)
                quantum = budget;
            amiga_clock_step(quantum);
            pending_cck -= quantum;
            budget -= quantum;
        }
        amiga_core_lock_release();
    }
}
