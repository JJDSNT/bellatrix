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
    config.chip_ram_size = BELLATRIX_CHIP_RAM_SIZE;
    config.chip_ram.read16 = amiga_chip_ram_read16;
    config.chip_ram.write16 = amiga_chip_ram_write16;

    rigel = rigel_create(&config);
    if (rigel == 0)
        kprintf("[BELLATRIX:RIGEL] initialization failed\n");
    else
    {
        amiga_irq_init(rigel);
        kprintf("[BELLATRIX:RIGEL] enabled; address decode owned by Rigel\n");
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
