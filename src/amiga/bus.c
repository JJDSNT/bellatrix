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
#include "rigel/rigel.h"
#include "tlsf.h"

extern void *tlsf;

static RigelContext *rigel;
static uint8_t unsupported_reported;
static uint64_t last_cpu_cycles;
static uint64_t pending_cck;
static uint8_t cpu_cycle_remainder;
static uint8_t clock_reported;
static uint8_t vblank_reported;
static uint8_t chipset_observed;

enum { AMIGA_MAX_STEP_CCK = 512 };

#if defined(CONFIG_RIGEL_SELFTEST) && CONFIG_RIGEL_SELFTEST
static void amiga_bus_selftest(void);
#endif

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
    if (!vblank_reported && (result.events & RIGEL_EVENT_VBLANK) != 0)
    {
        kprintf("[BELLATRIX:RIGEL] first VBLANK at %llu CCK\n",
            (unsigned long long)result.time);
        vblank_reported = 1;
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
