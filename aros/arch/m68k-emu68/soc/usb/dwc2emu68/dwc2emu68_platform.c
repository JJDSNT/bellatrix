/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * Emu68 platform boundary for DWC2. No controller code reaches through this
 * file for MMIO, DMA aliases, cache maintenance or interrupt registration.
 */

#include <aros/debug.h>
#include <aros/macros.h>

#include <proto/exec.h>

#include <asm/cpu.h>

#include "dwc2emu68_intern.h"

/* kernel.resource inline calls use this conventional base macro. Keeping the
 * value in our device base avoids a writable process-global library base. */
#define KernelBase device->kernel_base
#include <proto/kernel.h>

#define DWC2_REGISTER_OFFSET    0x00980000UL
#define DWC2_GSNPSID            0x0040UL
#define DWC2_GSNPSID_MASK       0xfffff000UL
#define DWC2_GSNPSID_OT2        0x4f542000UL

ULONG dwc2_readl(const struct DWC2Device *device, ULONG offset)
{
    ULONG value;

    dmb();
    value = AROS_LE2LONG(*(volatile ULONG *)(device->register_base + offset));
    dsb();
    return value;
}

void dwc2_writel(const struct DWC2Device *device, ULONG offset, ULONG value)
{
    dmb();
    *(volatile ULONG *)(device->register_base + offset) = AROS_LONG2LE(value);
    dsb();
}

BOOL dwc2_platform_probe(struct DWC2Device *device)
{
    ULONG id;

    device->kernel_base = OpenResource("kernel.resource");
    if (device->kernel_base == NULL)
        return FALSE;

    device->peripheral_base = KrnGetSystemAttr(KATTR_PeripheralBase);
    if (device->peripheral_base == 0)
        return FALSE;

    device->register_base = device->peripheral_base + DWC2_REGISTER_OFFSET;
    id = dwc2_readl(device, DWC2_GSNPSID);
    if ((id & DWC2_GSNPSID_MASK) != DWC2_GSNPSID_OT2)
    {
        bug("[DWC2/Emu68] no Synopsys OT2 core at %p (GSNPSID=%08lx)\n",
            (APTR)device->register_base, id);
        return FALSE;
    }

    bug("[DWC2/Emu68] OT2 core %c%c%lx.%lx%lx%lx at %p\n",
        (int)((id >> 24) & 0xff), (int)((id >> 16) & 0xff),
        (id >> 12) & 0xf, (id >> 8) & 0xf, (id >> 4) & 0xf, id & 0xf,
        (APTR)device->register_base);
    return TRUE;
}

void dwc2_platform_cpu0_mask(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;

    unit->affinity = 0;
    KrnGetCPUMask(0, &unit->affinity);
}
