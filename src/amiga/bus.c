/*
 * Classic Amiga compatibility bus.
 *
 * Bellatrix translates an Emu68 fault into one public Rigel transaction.
 * Address decoding and all classic hardware semantics stay inside Rigel.
 */

#include "amiga/bus.h"
#include "amiga/irq.h"
#include "machine/memory.h"

#include "A64.h"
#include "M68k.h"
#include "rigel/rigel.h"
#include "tlsf.h"

extern void *tlsf;
extern struct M68KState *__m68k_state;

static RigelContext *rigel;
static uint8_t unsupported_reported;
static uint64_t last_cpu_cycles;
static uint64_t pending_cck;
static uint8_t cpu_cycle_remainder;
static uint8_t clock_reported;
static uint8_t stopped_reported;
static uint8_t census_reports;
static uint32_t vblank_count;
static uint8_t vblank_reports;
static uint8_t chipset_observed;

enum { AMIGA_MAX_STEP_CCK = 512 };

#if defined(CONFIG_RIGEL_SELFTEST) && CONFIG_RIGEL_SELFTEST
static void amiga_bus_selftest(void);
static void amiga_bus_display_selftest(void);
#endif

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
    enum { AMIGA_CENSUS_REPORTS = 6, AMIGA_CENSUS_STRIDE = 7 };
    rigel_frame_t frame;
    const uint32_t *row;
    uint32_t background;
    uint32_t non_background = 0;
    uint32_t checksum = 0;
    uint32_t sampled = 0;
    uint32_t x, y;

    if (census_reports >= AMIGA_CENSUS_REPORTS)
        return;
    if (!rigel_get_frame(rigel, &frame))
        return;
    if (frame.pixels == 0 || frame.width == 0 || frame.height == 0)
        return;
    if (frame.format != RIGEL_PIXEL_RGBA8888)
    {
        census_reports = AMIGA_CENSUS_REPORTS;
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

    census_reports++;
    kprintf("[BELLATRIX:RIGEL:CENSUS] frame=%llu %ux%u pitch=%u bg=%08x "
            "non-bg=%u/%u sum=%08x flags=%02x\n",
        (unsigned long long)frame.frame_count,
        (unsigned)frame.width, (unsigned)frame.height, (unsigned)frame.pitch,
        (unsigned)background, (unsigned)non_background, (unsigned)sampled,
        (unsigned)checksum, (unsigned)frame.flags);
}

static void amiga_clock_step(rigel_cycle_t cycles)
{
    rigel_step_result_t result;

    if (rigel == 0 || cycles == 0)
        return;

    result = rigel_step(rigel, cycles);
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
        amiga_frame_census();
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

static void amiga_clock_consume(int flush)
{
    while (pending_cck != 0)
    {
        rigel_cycle_t quantum = amiga_clock_quantum();

        if (!flush && pending_cck < quantum)
            break;
        if (pending_cck < quantum)
            quantum = (rigel_cycle_t)pending_cck;
        amiga_clock_step(quantum);
        pending_cck -= quantum;
    }
}

void bellatrix_emu68_publish_cpu_progress(uint64_t cycles)
{
    uint64_t delta;
    uint64_t scaled;

    if (rigel == 0)
        return;
    if (cycles < last_cpu_cycles)
        last_cpu_cycles = cycles;
    delta = cycles - last_cpu_cycles;
    last_cpu_cycles = cycles;
    scaled = delta + cpu_cycle_remainder;
    pending_cck += scaled / 2u;
    cpu_cycle_remainder = (uint8_t)(scaled & 1u);
    if (!stopped_reported && __m68k_state != 0 && __m68k_state->STOPPED)
    {
        /*
         * The CPU is parked on a STOP and is still handing us time. That only
         * happens through the EMIT_STOP yield path, so this line is the direct
         * evidence that idle time reaches the chipset -- and its absence, on a
         * machine with an armed chipset that has idled, is the direct evidence
         * that it does not.
         */
        stopped_reported = 1;
        kprintf("[BELLATRIX:RIGEL] chipset advancing from a stopped CPU\n");
    }
    if (chipset_observed)
        amiga_clock_consume(0);
}

static void amiga_clock_flush(void)
{
    amiga_clock_consume(1);
}

static void amiga_clock_observe(void)
{
    if (!chipset_observed)
    {
        /*
         * Before the first MMIO transaction the chipset has no observer and
         * no programmed asynchronous work. Start its time domain here rather
         * than replaying the whole CPU boot as an expensive catch-up burst.
         */
        pending_cck = 0;
        cpu_cycle_remainder = 0;
        chipset_observed = 1;
        /*
         * Tell the CPU that idling now has a cost. A stopped m68k retires no
         * cycles, so with a chipset running it must not sleep: EMIT_STOP reads
         * this field and yields to MainLoop instead, which advances Rigel.
         * Until it is set the guest can only be woken by a platform interrupt,
         * and parking the core is the right thing to do.
         */
        if (__m68k_state != 0)
            __m68k_state->CHIPSET_ACTIVE = 1;
        kprintf("[BELLATRIX:RIGEL] clock armed by first MMIO\n");
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
    rigel_u8 i;

    (void)opaque;
    if (event == 0)
        return;

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

static void amiga_chip_ram_write16(void *opaque, rigel_u32 address,
                                   rigel_u16 value)
{
    (void)opaque;
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

    rigel = rigel_create(&config);
    if (rigel == 0)
        kprintf("[BELLATRIX:RIGEL] initialization failed\n");
    else
    {
        amiga_irq_init(rigel);
        kprintf("[BELLATRIX:RIGEL] enabled; address decode owned by Rigel\n");
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

    amiga_clock_observe();
    result = rigel_mmio_read(rigel, address, (rigel_u8)size, &value);
    amiga_irq_sync();

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

static void amiga_bus_write(const MachineRegion *region, uint32_t address,
                            int size, uint32_t value)
{
    rigel_mmio_result_t result;

    (void)region;
    if (rigel == 0)
        return;

    amiga_clock_observe();
    result = rigel_mmio_write(rigel, address, (rigel_u8)size, value);
    amiga_irq_sync();
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

    bellatrix_emu68_publish_cpu_progress(2000u); /* 1000 CCK */

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
     * Two frames' worth of colour clocks, so a whole frame is composed with
     * the display already programmed rather than half-way through it.
     */
    bellatrix_emu68_publish_cpu_progress(last_cpu_cycles + 300000u);

    kprintf("[BELLATRIX:RIGEL:DISPLAY] %llu CCK stepped; census above\n",
        (unsigned long long)rigel_get_time(rigel));
}
#endif
