/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * Emu68 platform boundary for DWC2. No controller code reaches through this
 * file for MMIO, DMA aliases, cache maintenance or interrupt registration.
 */

#include <aros/debug.h>
#include <aros/macros.h>

#include <proto/exec.h>

#include <exec/memory.h>

#include <asm/cpu.h>
#include <hardware/videocore.h>

#include "dwc2emu68_intern.h"
#include "dwc2emu68_regs.h"

/* kernel.resource inline calls use this conventional base macro. Keeping the
 * value in our device base avoids a writable process-global library base. */
#define KernelBase device->kernel_base
#include <proto/kernel.h>
#define MBoxBase device->mbox_base
#include <proto/mbox.h>

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
    /* Match the ordering used by the AROS AArch64 BCM2708 driver: all prior
     * cache and memory operations must be complete before an MMIO command is
     * made visible to the DWC2 bus master. */
    dsb();
    *(volatile ULONG *)(device->register_base + offset) = AROS_LONG2LE(value);
    dmb();
}

BOOL dwc2_platform_probe(struct DWC2Device *device)
{
    ULONG id;

    device->kernel_base = OpenResource("kernel.resource");
    if (device->kernel_base == NULL)
        return FALSE;
    device->mbox_base = OpenResource("mbox.resource");

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

static ULONG query_clock(struct DWC2Device *device, ULONG clock_id)
{
    APTR raw;
    ULONG *message;
    ULONG rate = 0;

    if (device->mbox_base == NULL)
        return 0;
    raw = AllocMem(64 + 63, MEMF_PUBLIC | MEMF_CLEAR);
    if (raw == NULL)
        return 0;
    message = (ULONG *)(((IPTR)raw + 63) & ~(IPTR)63);
    message[0] = AROS_LONG2LE(8 * 4);
    message[1] = AROS_LONG2LE(VCTAG_REQ);
    message[2] = AROS_LONG2LE(VCTAG_GETCLKRATE);
    message[3] = AROS_LONG2LE(8);
    message[4] = AROS_LONG2LE(4);
    message[5] = AROS_LONG2LE(clock_id);
    message[6] = 0;
    message[7] = 0;
    if (MBoxCall((APTR)(device->peripheral_base + VCMB_OFFSET),
        VCMB_PROPCHAN, message) == message)
        rate = AROS_LE2LONG(message[6]);
    FreeMem(raw, 64 + 63);
    return rate;
}

void dwc2_platform_log_clocks(struct DWC2Device *device)
{
    ULONG arm_rate = query_clock(device, VCCLOCK_ARM);
    ULONG core_rate = query_clock(device, VCCLOCK_CORE);
    ULONG sdram_rate = query_clock(device, VCCLOCK_SDRAM);
    ULONG uart_rate = query_clock(device, VCCLOCK_UART);
    ULONG sd_emmc_rate = query_clock(device, VCCLOCK_EMMC);
    ULONG usb_host_rate = (dwc2_readl(device, DWC2_HFIR) & 0xffffUL) * 1000;

    bug("[DWC2/Emu68] clocks: arm=%lu core=%lu sdram=%lu uart=%lu "
        "sd/emmc=%lu usb-host=%lu Hz mbox=%s\n", arm_rate, core_rate,
        sdram_rate, uart_rate, sd_emmc_rate, usb_host_rate,
        device->mbox_base != NULL ? "yes" : "no");
}

void dwc2_platform_cpu0_mask(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;

    unit->affinity = 0;
    KrnGetCPUMask(0, &unit->affinity);
}
