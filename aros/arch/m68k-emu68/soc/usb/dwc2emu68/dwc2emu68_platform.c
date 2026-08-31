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

    /* Power precedes identification: an unpowered core still answers on the
     * register bus, so probing first would prove nothing about the part of it
     * this driver actually needs. */
    if (!dwc2_platform_power_on(device))
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

/*
 * One round trip on the VideoCore property channel.
 *
 * The firmware answers in place: the tag's value slots carry the request on
 * the way in and the response on the way out, and it marks the tag's length
 * word to say it understood. Both halves matter. A tag the firmware does not
 * recognise comes back successfully with the request untouched, so a caller
 * that reads the values without checking that mark cannot tell an answer from
 * its own echo -- which is how a driver ends up believing a state it never
 * learned.
 *
 * MBoxCall invalidates the reply a cache line at a time, so the message has
 * to own its line rather than share one with whatever AllocMem put next door.
 */
static BOOL mbox_property(struct DWC2Device *device, ULONG tag,
    ULONG *values, ULONG slots)
{
    ULONG words = 6 + slots;
    ULONG size = words * sizeof(ULONG);
    APTR raw;
    ULONG *message;
    ULONG i;
    BOOL answered = FALSE;

    if (device->mbox_base == NULL)
        return FALSE;
    raw = AllocMem(size + 63, MEMF_PUBLIC | MEMF_CLEAR);
    if (raw == NULL)
        return FALSE;
    message = (ULONG *)(((IPTR)raw + 63) & ~(IPTR)63);

    message[0] = AROS_LONG2LE(size);
    message[1] = AROS_LONG2LE(VCTAG_REQ);
    message[2] = AROS_LONG2LE(tag);
    message[3] = AROS_LONG2LE(slots * sizeof(ULONG));
    message[4] = 0;
    for (i = 0; i < slots; i++)
        message[5 + i] = AROS_LONG2LE(values[i]);
    message[5 + slots] = 0;

    if (MBoxCall((APTR)(device->peripheral_base + VCMB_OFFSET),
        VCMB_PROPCHAN, message) == message)
    {
        answered = (AROS_LE2LONG(message[4]) & 0x80000000UL) != 0;
        for (i = 0; i < slots; i++)
            values[i] = AROS_LE2LONG(message[5 + i]);
    }
    FreeMem(raw, size + 63);
    return answered;
}

static ULONG query_clock(struct DWC2Device *device, ULONG clock_id)
{
    ULONG values[2];

    values[0] = clock_id;
    values[1] = 0;
    if (!mbox_property(device, VCTAG_GETCLKRATE, values, 2))
        return 0;
    return values[1];
}

/*
 * Ask the firmware to power the USB host controller's domain on.
 *
 * The ARM does not own that domain on this SoC; the VideoCore firmware does,
 * and it powers it on request rather than by default. What makes this worth
 * spelling out is that a DWC2 whose domain is off does not look off. Its
 * register file answers, its DMA master fetches, its frame counter runs, and
 * its root port still reports a connection, accepts a reset and comes up
 * enabled -- because each of those is either an AHB access or passive sensing
 * on the line, and neither needs the analogue transmitter.
 *
 * What does not happen is transmission, and the shape that leaves is a
 * channel armed with its data already fetched and its request already queued,
 * followed by nothing: no completion, no NAK, not even the core's own
 * transaction timeout, because no transaction was ever started. The port
 * speed is the second witness -- a high-speed device downstream negotiates
 * full speed, since the chirp handshake needs a transmitter too.
 *
 * Hence: before the core is touched at all, and verified rather than assumed.
 */
BOOL dwc2_platform_power_on(struct DWC2Device *device)
{
    ULONG values[2];

    values[0] = VCPOWER_USBHCD;
    values[1] = VCPOWER_STATE_ON | VCPOWER_STATE_WAIT;
    if (!mbox_property(device, VCTAG_SETPOWER, values, 2))
    {
        /*
         * Nobody answered, so nothing is known -- which is not the same as
         * knowing the domain is off, and is not grounds for refusing to run.
         * A platform without this firmware (QEMU is one) has no such domain
         * to switch and every reason to carry on.
         */
        bug("[DWC2/Emu68] no answer to VCTAG_SETPOWER; USB power state "
            "unknown, continuing\n");
        return TRUE;
    }

    /*
     * The state word is not symmetric between request and response. Going in,
     * bit 1 asks the firmware to hold its reply until the domain has settled;
     * coming back, the same bit means the device does not exist. Bit 0 is the
     * power state in both directions.
     */
    if (values[1] & VCPOWER_STATE_WAIT)
    {
        bug("[DWC2/Emu68] firmware reports no USB host controller\n");
        return FALSE;
    }
    if (!(values[1] & VCPOWER_STATE_ON))
    {
        bug("[DWC2/Emu68] USB power domain stayed off (state %08lx)\n",
            values[1]);
        return FALSE;
    }
    bug("[DWC2/Emu68] USB power domain on\n");
    return TRUE;
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
