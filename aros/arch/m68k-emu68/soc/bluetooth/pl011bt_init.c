/*
    Copyright (C) 2026, The Bellatrix Project. All rights reserved.

    Raspberry Pi onboard Bluetooth UART boundary for AROS/m68k-emu68.

    Emu68 routes its diagnostic console through AUX mini-UART. PL011 belongs
    exclusively to this resource, but is programmed only after a client claims
    it; resident initialisation remains side-effect free.
*/

#define DEBUG 0

#include <aros/debug.h>
#include <aros/kernel.h>
#include <aros/libcall.h>
#include <aros/macros.h>
#include <aros/symbolsets.h>
#include <proto/pl011bt.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <exec/interrupts.h>
#include <proto/mbox.h>

#include <hardware/bcm2708.h>
#include <hardware/videocore.h>

#include "pl011bt_private.h"

#define GPIO_OFFSET 0x00200000UL
#define GPIO_OFFSET 0x00200000UL

#define GPIO_GPFSEL3 0x0c
#define GPIO_GPFSEL4 0x10






#define GPIO_ALT3 7UL
#define GPIO_ALT0 4UL
#define UART_CLOCK_FALLBACK_HZ 48000000UL

#define CLOCK_GP2CTL_OFFSET 0x00101080UL
#define CLOCK_GP2DIV_OFFSET 0x00101084UL
#define CLOCK_PASSWORD 0x5a000000UL
/* The password field is write-only and reads back as zero; mask it out of a
 * read-modify-write so the stored value cannot carry stale bits into it. */
#define CLOCK_PASSWORD_MASK 0xff000000UL
/* The only fields worth carrying across a stop: clock source and MASH order.
 * Everything else in CM_GP2CTL is status (BUSY), a one-shot (KILL, FLIP) or
 * undefined, and writing back whatever a read returned is how this hung under
 * emulation, where the register is not modelled and reads are not meaningful. */
#define CLOCK_CTL_SRC_MASK  0x0000000fUL
#define CLOCK_CTL_MASH_MASK 0x00000600UL
#define CLOCK_CTL_ENABLE (1UL << 4)
#define CLOCK_CTL_BUSY (1UL << 7)
#define CLOCK_CTL_MASH1 (1UL << 9)
#define CLOCK_SOURCE_OSCILLATOR 1UL

#define FW_SET_GPIO_STATE 0x00038041UL
#define FW_SET_GPIO_CONFIG 0x00038043UL
#define FW_GPIO_BT_REG_EN 128UL
#define FW_GPIO_DIRECTION_OUTPUT 1UL

/* proto/mbox.h and proto/kernel.h declare the conventional library bases as
 * extern APTR.  Define those globals here so their inline calls use the
 * resources opened by pl011bt_init(). */
APTR MBoxBase;
APTR KernelBase;

static ULONG mmio_read(IPTR base, ULONG offset)
{
    return AROS_LE2LONG(*(volatile ULONG *)(base + offset));
}

static void mmio_write(IPTR base, ULONG offset, ULONG value)
{
    *(volatile ULONG *)(base + offset) = AROS_LONG2LE(value);
}

static ULONG gpio_function(ULONG value, ULONG gpio, ULONG function)
{
    ULONG shift = (gpio % 10UL) * 3UL;

    value &= ~(7UL << shift);
    value |= (function & 7UL) << shift;
    return value;
}

static void route_bluetooth_uart(struct PL011BTBase *base)
{
    ULONG fsel3 = mmio_read(base->gpio_base, GPIO_GPFSEL3);

    fsel3 = gpio_function(fsel3, 30, GPIO_ALT3);
    fsel3 = gpio_function(fsel3, 31, GPIO_ALT3);
    fsel3 = gpio_function(fsel3, 32, GPIO_ALT3);
    fsel3 = gpio_function(fsel3, 33, GPIO_ALT3);
    mmio_write(base->gpio_base, GPIO_GPFSEL3, fsel3);
}

static LONG setup_lpo_clock(struct PL011BTBase *base)
{
    ULONG fsel4 = mmio_read(base->gpio_base, GPIO_GPFSEL4);
    ULONG wait = PL011_WAIT_LIMIT;
    ULONG ctl;

    fsel4 = gpio_function(fsel4, 43, GPIO_ALT0);
    mmio_write(base->gpio_base, GPIO_GPFSEL4, fsel4);

    /*
     * Stop the generator by clearing ENAB *and nothing else*.
     *
     * This used to write the password alone, which clears ENAB but in the same
     * store also drives SRC to 0 (GND) and MASH to 0. The BCM283x peripherals
     * datasheet is explicit that the source and divisor must not be changed
     * while the generator is running: clear ENAB, wait for BUSY to fall, and
     * only then reprogram. Changing SRC in the stop write leaves BUSY set, and
     * the wait below -- a million iterations, so not a short timeout -- never
     * completes.
     *
     * It passes under emulation because nothing is driving GPCLK2 there and
     * BUSY reads 0 immediately. On a real Pi the firmware has already started
     * it for the Bluetooth LPO, so the bad write lands on a running generator.
     */
    ctl = mmio_read(base->peripheral_base, CLOCK_GP2CTL_OFFSET) &
          (CLOCK_CTL_SRC_MASK | CLOCK_CTL_MASH_MASK);
    mmio_write(base->peripheral_base, CLOCK_GP2CTL_OFFSET, CLOCK_PASSWORD | ctl);
    while ((mmio_read(base->peripheral_base, CLOCK_GP2CTL_OFFSET) &
            CLOCK_CTL_BUSY) && --wait != 0)
        ;
    if (wait == 0)
        return PL011BT_ERR_TIMEOUT;

    /* 19.2 MHz / 585.9375 = 32768 Hz. The fractional field is /4096. */
    mmio_write(base->peripheral_base, CLOCK_GP2DIV_OFFSET,
               CLOCK_PASSWORD | (585UL << 12) | 3840UL);
    mmio_write(base->peripheral_base, CLOCK_GP2CTL_OFFSET,
               CLOCK_PASSWORD | CLOCK_CTL_MASH1 |
               CLOCK_SOURCE_OSCILLATOR | CLOCK_CTL_ENABLE);
    return PL011BT_OK;
}

static LONG firmware_gpio_set(ULONG gpio, ULONG enabled)
{
    ULONG *raw;
    ULONG *msg;
    LONG result = PL011BT_ERR_UNAVAILABLE;

    if (MBoxBase == NULL)
    {
        bug("[PL011BT] power: mbox.resource unavailable\n");
        return result;
    }
    raw = AllocMem(12 * sizeof(ULONG) + 15, MEMF_PUBLIC | MEMF_CLEAR);
    if (raw == NULL)
    {
        bug("[PL011BT] power: mailbox allocation failed\n");
        return result;
    }
    msg = (ULONG *)(((IPTR)raw + 15) & ~(IPTR)15);

    msg[0] = AROS_LONG2LE(12 * 4);
    msg[1] = AROS_LONG2LE(VCTAG_REQ);
    msg[2] = AROS_LONG2LE(FW_SET_GPIO_CONFIG);
    msg[3] = AROS_LONG2LE(24);
    msg[4] = 0;
    msg[5] = AROS_LONG2LE(gpio);
    msg[6] = AROS_LONG2LE(FW_GPIO_DIRECTION_OUTPUT);
    msg[7] = 0;
    msg[8] = 0;
    msg[9] = 0;
    msg[10] = AROS_LONG2LE(enabled != 0);
    msg[11] = 0;
    if (MBoxCall((APTR)(KrnGetSystemAttr(KATTR_PeripheralBase) + VCMB_OFFSET),
                 VCMB_PROPCHAN, msg) == msg &&
        (AROS_LE2LONG(msg[1]) & VCTAG_RESP))
        result = PL011BT_OK;

    if (result == PL011BT_OK)
    {
        msg[0] = AROS_LONG2LE(8 * 4);
        msg[1] = AROS_LONG2LE(VCTAG_REQ);
        msg[2] = AROS_LONG2LE(FW_SET_GPIO_STATE);
        msg[3] = AROS_LONG2LE(8);
        msg[4] = 0;
        msg[5] = AROS_LONG2LE(gpio);
        msg[6] = AROS_LONG2LE(enabled != 0);
        msg[7] = 0;
        if (MBoxCall((APTR)(KrnGetSystemAttr(KATTR_PeripheralBase) +
                            VCMB_OFFSET), VCMB_PROPCHAN, msg) != msg ||
            !(AROS_LE2LONG(msg[1]) & VCTAG_RESP))
            result = PL011BT_ERR_UNAVAILABLE;
    }

    FreeMem(raw, 12 * sizeof(ULONG) + 15);
    bug("[PL011BT] power: GPIO %lu -> %lu, result %d\n",
        gpio, enabled != 0, (int)result);
    return result;
}

static ULONG query_uart_clock(struct PL011BTBase *base)
{
    ULONG raw[12];
    ULONG *msg = (ULONG *)(((IPTR)raw + 15) & ~(IPTR)15);

    (void)base;
    if (MBoxBase == NULL)
        return UART_CLOCK_FALLBACK_HZ;

    msg[0] = AROS_LONG2LE(8 * 4);
    msg[1] = AROS_LONG2LE(VCTAG_REQ);
    msg[2] = AROS_LONG2LE(VCTAG_GETCLKRATE);
    msg[3] = AROS_LONG2LE(8);
    msg[4] = AROS_LONG2LE(4);
    msg[5] = AROS_LONG2LE(VCCLOCK_UART);
    msg[6] = 0;
    msg[7] = 0;

    if (MBoxCall((APTR)(base->peripheral_base + VCMB_OFFSET),
                 VCMB_PROPCHAN, msg) != msg)
        return UART_CLOCK_FALLBACK_HZ;
    if (AROS_LE2LONG(msg[6]) == 0)
        return UART_CLOCK_FALLBACK_HZ;
    return AROS_LE2LONG(msg[6]);
}

static int pl011bt_init(struct PL011BTBase *PL011BTBase)
{
    InitSemaphore(&PL011BTBase->lock);
    PL011BTBase->owner = NULL;
    PL011BTBase->capabilities = 0;

    KernelBase = OpenResource("kernel.resource");
    MBoxBase = OpenResource("mbox.resource");
    if (KernelBase == NULL)
        return FALSE;

    PL011BTBase->peripheral_base = KrnGetSystemAttr(KATTR_PeripheralBase);
    if (PL011BTBase->peripheral_base == 0 ||
        PL011BTBase->peripheral_base == (IPTR)-1)
        return FALSE;

    PL011BTBase->gpio_base = PL011BTBase->peripheral_base + GPIO_OFFSET;
    PL011BTBase->uart_base = PL011BTBase->peripheral_base + PL011_OFFSET;
    PL011BTBase->uart_clock_hz = query_uart_clock(PL011BTBase);
    PL011BTBase->capabilities = PL011BT_CAP_PRESENT |
        PL011BT_CAP_BAUD_CHANGE | PL011BT_CAP_POWER_CONTROL;

    bug("[PL011BT] ready: PL011=0x%lx clock=%lu mbox=%s\n",
        (ULONG)PL011BTBase->uart_base, PL011BTBase->uart_clock_hz,
        MBoxBase != NULL ? "yes" : "no");

    return TRUE;
}

/*
 * Drain the FIFO into the ring. Runs in interrupt context.
 *
 * Plain void(void *, void *), which is what KrnAddIRQHandler() calls -- see
 * platform/bcm283x/system_timer.c:113. Writing it as AROS_INTH1 produces the
 * struct Interrupt server convention instead, and being called as a plain
 * pointer through that mismatch corrupted registers: the first attempt died
 * three boots out of three, with the serial output turning to garbage.
 *
 * Everything here is a store to MMIO or to the ring: no allocation, no lock,
 * no call back into AROS. The reader synchronises on rx_head alone, which this
 * is the only writer of, so no interlock is needed for a single producer and a
 * single consumer.
 */
static void pl011bt_rx_handler(void *data, void *unused)
{
    struct PL011BTBase *PL011BTBase = data;
    ULONG head;

    (void)unused;
    head = PL011BTBase->rx_head;

    while (!(mmio_read(PL011BTBase->uart_base, PL011_FR) & PL011_FR_RXFE))
    {
        ULONG dr = mmio_read(PL011BTBase->uart_base, PL011_DR);
        ULONG next = (head + 1) & (PL011BT_RX_RING - 1);

        if ((dr & PL011_DR_ERR) != 0 && PL011BTBase->rx_errors < 8)
        {
            PL011BTBase->rx_errors++;
            bug("[PL011BT] rx status 0x%lx%s\n", (dr >> 8) & 0xf,
                (dr & PL011_DR_OE) ? " OVERRUN" : "");
        }
        if (next == PL011BTBase->rx_tail)
        {
            /* Reader is too far behind. Dropping here is still better than the
             * FIFO overrunning, because it is counted. */
            PL011BTBase->rx_dropped++;
            break;
        }
        PL011BTBase->rx_ring[head] = (UBYTE)(dr & 0xff);
        head = next;
    }
    PL011BTBase->rx_head = head;

    /* Acknowledge receive and receive-timeout; overrun is cleared with them so
     * a single lost byte does not latch the condition forever. */
    mmio_write(PL011BTBase->uart_base, PL011_ICR,
               PL011_INT_RX | PL011_INT_RT | PL011_INT_OE);
}

AROS_LH1(long, PL011BTClaim,
    AROS_LHA(void *, owner, A0),
    struct PL011BTBase *, PL011BTBase, 3, Pl011bt)
{
    AROS_LIBFUNC_INIT

    LONG result = PL011BT_OK;

    if (owner == NULL)
        return PL011BT_ERR_ARGUMENT;
    ObtainSemaphore(&PL011BTBase->lock);
    if (PL011BTBase->owner != NULL)
        result = PL011BT_ERR_BUSY;
    else
        PL011BTBase->owner = owner;
    ReleaseSemaphore(&PL011BTBase->lock);
    return result;

    AROS_LIBFUNC_EXIT
}

AROS_LH1(void, PL011BTRelease,
    AROS_LHA(void *, owner, A0),
    struct PL011BTBase *, PL011BTBase, 4, Pl011bt)
{
    AROS_LIBFUNC_INIT

    ObtainSemaphore(&PL011BTBase->lock);
    if (owner != NULL && PL011BTBase->owner == owner)
    {
        mmio_write(PL011BTBase->uart_base, PL011_IMSC, 0);
        mmio_write(PL011BTBase->uart_base, PL011_CR, 0);
        PL011BTBase->owner = NULL;
        PL011BTBase->baud = 0;
        PL011BTBase->config_flags = 0;
    }
    ReleaseSemaphore(&PL011BTBase->lock);

    AROS_LIBFUNC_EXIT
}

AROS_LH3(long, PL011BTConfigure,
    AROS_LHA(void *, owner, A0),
    AROS_LHA(unsigned long, baud, D0),
    AROS_LHA(unsigned long, flags, D1),
    struct PL011BTBase *, PL011BTBase, 5, Pl011bt)
{
    AROS_LIBFUNC_INIT

    ULONG divisor;
    ULONG integer;
    ULONG fraction;
    ULONG control;
    ULONG wait = PL011_WAIT_LIMIT;

    if (owner == NULL || baud == 0)
        return PL011BT_ERR_ARGUMENT;
    ObtainSemaphore(&PL011BTBase->lock);
    if (PL011BTBase->owner != owner)
    {
        ReleaseSemaphore(&PL011BTBase->lock);
        return PL011BT_ERR_NOT_OWNER;
    }

    if (setup_lpo_clock(PL011BTBase) != PL011BT_OK)
    {
        bug("[PL011BT] configure: LPO clock timeout\n");
        ReleaseSemaphore(&PL011BTBase->lock);
        return PL011BT_ERR_TIMEOUT;
    }
    route_bluetooth_uart(PL011BTBase);
    mmio_write(PL011BTBase->uart_base, PL011_CR, 0);
    while ((mmio_read(PL011BTBase->uart_base, PL011_FR) & PL011_FR_BUSY) &&
           --wait != 0)
        ;
    if (wait == 0)
    {
        bug("[PL011BT] configure: PL011 busy timeout\n");
        ReleaseSemaphore(&PL011BTBase->lock);
        return PL011BT_ERR_TIMEOUT;
    }

    mmio_write(PL011BTBase->uart_base, PL011_LCRH, 0);
    mmio_write(PL011BTBase->uart_base, PL011_IMSC, 0);
    mmio_write(PL011BTBase->uart_base, PL011_ICR, 0x7ff);

    /*
     * Start from an empty receive path, not from whatever the line held.
     *
     * Clearing the interrupt status says nothing about the FIFO's contents.
     * Powering the controller through BT_REG_EN and reprogramming the baud
     * divisor both put transitions on the wire, and whatever the receiver made
     * of those sits in the FIFO waiting to be read as if it were HCI. On
     * hardware that shows up as "rx status 0x8 OVERRUN" before a single
     * command has been sent, followed by the H4 framer reporting unknown
     * packet types -- it is not desynchronised by a lost reply, it never had
     * synchronisation to begin with.
     *
     * Drain the FIFO and reset the ring so the first byte the framer sees is
     * the first byte the controller actually sent.
     */
    {
        ULONG guard = PL011BT_RX_RING;

        while (guard-- &&
               !(mmio_read(PL011BTBase->uart_base, PL011_FR) & PL011_FR_RXFE))
            (void)mmio_read(PL011BTBase->uart_base, PL011_DR);

        PL011BTBase->rx_head = 0;
        PL011BTBase->rx_tail = 0;
        PL011BTBase->rx_dropped = 0;
        mmio_write(PL011BTBase->uart_base, PL011_ICR, 0x7ff);
    }

    divisor = (PL011BTBase->uart_clock_hz * 4UL + baud / 2UL) / baud;
    integer = divisor >> 6;
    fraction = divisor & 0x3f;
    if (integer == 0 || integer > 0xffff)
    {
        ReleaseSemaphore(&PL011BTBase->lock);
        return PL011BT_ERR_ARGUMENT;
    }
    mmio_write(PL011BTBase->uart_base, PL011_IBRD, integer);
    mmio_write(PL011BTBase->uart_base, PL011_FBRD, fraction);
    mmio_write(PL011BTBase->uart_base, PL011_LCRH,
               PL011_LCRH_WLEN8 | PL011_LCRH_FEN);

    control = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
    if (flags & PL011BT_CONFIG_RTS_CTS)
        control |= PL011_CR_RTSEN | PL011_CR_CTSEN;
    mmio_write(PL011BTBase->uart_base, PL011_CR, control);

    /*
     * Arm receive interrupts, once, after the port is configured.
     *
     * Order matters: the watermark and the mask are cleared by the LCRH write
     * above, and enabling the interrupt before UARTEN would deliver on a port
     * that is not receiving. Registration is idempotent because a second
     * Configure on the same owner is legitimate -- a baud change, for one.
     */
    mmio_write(PL011BTBase->uart_base, PL011_IFLS, PL011_IFLS_RX18);
    if (!PL011BTBase->rx_irq_armed)
    {
        if (KrnAddIRQHandler(IRQ_VC_UART, pl011bt_rx_handler, PL011BTBase, NULL))
        {
            PL011BTBase->rx_irq_armed = TRUE;
            PL011BTBase->capabilities |= PL011BT_CAP_RX_INTERRUPT;
            bug("[PL011BT] rx interrupt armed on irq %lu\n",
                (ULONG)IRQ_VC_UART);
        }
        else
            bug("[PL011BT] KrnAddIRQHandler(%lu) failed -- receive stays polled\n",
                (ULONG)IRQ_VC_UART);
    }
    mmio_write(PL011BTBase->uart_base, PL011_ICR, 0x7ff);
    if (PL011BTBase->rx_irq_armed)
        mmio_write(PL011BTBase->uart_base, PL011_IMSC,
                   PL011_INT_RX | PL011_INT_RT);
    PL011BTBase->baud = baud;
    PL011BTBase->config_flags = flags;
    bug("[PL011BT] configure: baud=%lu clock=%lu divisor=%lu/%lu flags=0x%lx\n",
        baud, PL011BTBase->uart_clock_hz, integer, fraction, flags);
    ReleaseSemaphore(&PL011BTBase->lock);
    return PL011BT_OK;

    AROS_LIBFUNC_EXIT
}

AROS_LH2(long, PL011BTSetPower,
    AROS_LHA(void *, owner, A0),
    AROS_LHA(unsigned long, enabled, D0),
    struct PL011BTBase *, PL011BTBase, 6, Pl011bt)
{
    AROS_LIBFUNC_INIT

    if (owner == NULL)
        return PL011BT_ERR_ARGUMENT;
    if (PL011BTBase->owner != owner)
        return PL011BT_ERR_NOT_OWNER;
    return firmware_gpio_set(FW_GPIO_BT_REG_EN, enabled);

    AROS_LIBFUNC_EXIT
}

AROS_LH3(long, PL011BTWrite,
    AROS_LHA(void *, owner, A0),
    AROS_LHA(const void *, data, A1),
    AROS_LHA(unsigned long, length, D0),
    struct PL011BTBase *, PL011BTBase, 7, Pl011bt)
{
    AROS_LIBFUNC_INIT

    const UBYTE *bytes = data;
    ULONG written = 0;

    if (owner == NULL || (data == NULL && length != 0))
        return PL011BT_ERR_ARGUMENT;
    if (PL011BTBase->owner != owner)
        return PL011BT_ERR_NOT_OWNER;
    while (written < length &&
           !(mmio_read(PL011BTBase->uart_base, PL011_FR) & PL011_FR_TXFF))
    {
        mmio_write(PL011BTBase->uart_base, PL011_DR, bytes[written]);
        written++;
    }
    return (LONG)written;

    AROS_LIBFUNC_EXIT
}

AROS_LH3(long, PL011BTRead,
    AROS_LHA(void *, owner, A0),
    AROS_LHA(void *, data, A1),
    AROS_LHA(unsigned long, capacity, D0),
    struct PL011BTBase *, PL011BTBase, 8, Pl011bt)
{
    AROS_LIBFUNC_INIT

    UBYTE *bytes = data;
    ULONG read = 0;

    if (owner == NULL || (data == NULL && capacity != 0))
        return PL011BT_ERR_ARGUMENT;
    if (PL011BTBase->owner != owner)
        return PL011BT_ERR_NOT_OWNER;
    /*
     * From the ring, not from the FIFO.
     *
     * Reading the FIFO here made the caller's scheduling the deadline for the
     * hardware, and no schedule was fast enough: OVERRUN was reported on real
     * hardware even at a one-millisecond poll. The interrupt owns the FIFO now
     * and this owns rx_tail, so a slow reader costs latency rather than data.
     */
    while (read < capacity && PL011BTBase->rx_tail != PL011BTBase->rx_head)
    {
        bytes[read] = PL011BTBase->rx_ring[PL011BTBase->rx_tail];
        PL011BTBase->rx_tail = (PL011BTBase->rx_tail + 1) & (PL011BT_RX_RING - 1);
        read++;
    }
    if (PL011BTBase->rx_dropped != 0)
    {
        bug("[PL011BT] rx ring overflow, %lu byte(s) dropped\n",
            PL011BTBase->rx_dropped);
        PL011BTBase->rx_dropped = 0;
    }
    return (LONG)read;

    AROS_LIBFUNC_EXIT
}

AROS_LH0(unsigned int, PL011BTGetAPIVersion,
    struct PL011BTBase *, PL011BTBase, 1, Pl011bt)
{
    AROS_LIBFUNC_INIT

    return PL011BT_API_VERSION;

    AROS_LIBFUNC_EXIT
}

AROS_LH0(unsigned long, PL011BTGetCapabilities,
    struct PL011BTBase *, PL011BTBase, 2, Pl011bt)
{
    AROS_LIBFUNC_INIT

    return PL011BTBase->capabilities;

    AROS_LIBFUNC_EXIT
}

/*
 * Register the init with genmodule's INITLIB set.
 *
 * Without this line pl011bt_init() is a static function nobody references, so
 * the compiler drops it along with everything only it calls -- and genmodule,
 * having no init to run, adds the resource anyway. OpenResource() then hands
 * out a base whose fields are all zero, which is worse than a missing resource
 * because it looks like a working one: PL011BTGetAPIVersion() answers from a
 * constant, and the first call that touches the base hangs. ObtainSemaphore()
 * on a zeroed SignalSemaphore blocks forever, because InitSemaphore() sets
 * ss_QueueCount to -1 and zero reads as "already owned by someone else".
 *
 * arch/m68k-emu68/soc/mbox/mbox_init.c:241 does the same for the same reason.
 */
ADD2INITLIB(pl011bt_init, 0)
