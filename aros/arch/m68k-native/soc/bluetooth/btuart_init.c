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
#include <proto/btuart.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <exec/interrupts.h>
#include <proto/mbox.h>

#include <hardware/bcm2708.h>
#include <hardware/videocore.h>

#include "btuart_private.h"

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
 * resources opened by btuart_init(). */
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

static void route_bluetooth_uart(struct BTUARTBase *base)
{
    ULONG fsel3 = mmio_read(base->gpio_base, GPIO_GPFSEL3);

    fsel3 = gpio_function(fsel3, 30, GPIO_ALT3);
    fsel3 = gpio_function(fsel3, 31, GPIO_ALT3);
    fsel3 = gpio_function(fsel3, 32, GPIO_ALT3);
    fsel3 = gpio_function(fsel3, 33, GPIO_ALT3);
    mmio_write(base->gpio_base, GPIO_GPFSEL3, fsel3);
}

static LONG setup_lpo_clock(struct BTUARTBase *base)
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
        return BTUART_ERR_TIMEOUT;

    /* 19.2 MHz / 585.9375 = 32768 Hz. The fractional field is /4096. */
    mmio_write(base->peripheral_base, CLOCK_GP2DIV_OFFSET,
               CLOCK_PASSWORD | (585UL << 12) | 3840UL);
    mmio_write(base->peripheral_base, CLOCK_GP2CTL_OFFSET,
               CLOCK_PASSWORD | CLOCK_CTL_MASH1 |
               CLOCK_SOURCE_OSCILLATOR | CLOCK_CTL_ENABLE);
    return BTUART_OK;
}

static LONG firmware_gpio_set(ULONG gpio, ULONG enabled)
{
    ULONG *raw;
    ULONG *msg;
    LONG result = BTUART_ERR_UNAVAILABLE;

    if (MBoxBase == NULL)
    {
        bug("[BTUART] power: mbox.resource unavailable\n");
        return result;
    }
    raw = AllocMem(12 * sizeof(ULONG) + 15, MEMF_PUBLIC | MEMF_CLEAR);
    if (raw == NULL)
    {
        bug("[BTUART] power: mailbox allocation failed\n");
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
        result = BTUART_OK;

    if (result == BTUART_OK)
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
            result = BTUART_ERR_UNAVAILABLE;
    }

    FreeMem(raw, 12 * sizeof(ULONG) + 15);
    bug("[BTUART] power: GPIO %lu -> %lu, result %d\n",
        gpio, enabled != 0, (int)result);
    return result;
}

static ULONG query_uart_clock(struct BTUARTBase *base)
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

static int btuart_init(struct BTUARTBase *BTUARTBase)
{
    InitSemaphore(&BTUARTBase->lock);
    BTUARTBase->owner = NULL;
    BTUARTBase->capabilities = 0;

    KernelBase = OpenResource("kernel.resource");
    MBoxBase = OpenResource("mbox.resource");
    if (KernelBase == NULL)
        return FALSE;

    BTUARTBase->peripheral_base = KrnGetSystemAttr(KATTR_PeripheralBase);
    if (BTUARTBase->peripheral_base == 0 ||
        BTUARTBase->peripheral_base == (IPTR)-1)
        return FALSE;

    BTUARTBase->gpio_base = BTUARTBase->peripheral_base + GPIO_OFFSET;
    BTUARTBase->uart_base = BTUARTBase->peripheral_base + PL011_OFFSET;
    BTUARTBase->uart_clock_hz = query_uart_clock(BTUARTBase);
    BTUARTBase->capabilities = BTUART_CAP_PRESENT |
        BTUART_CAP_BAUD_CHANGE | BTUART_CAP_POWER_CONTROL;

    bug("[BTUART] ready: PL011=0x%lx clock=%lu mbox=%s\n",
        (ULONG)BTUARTBase->uart_base, BTUARTBase->uart_clock_hz,
        MBoxBase != NULL ? "yes" : "no");

    return TRUE;
}

/*
 * Drain the FIFO into the ring. Runs in interrupt context.
 *
 * Everything here is a store to MMIO or to the ring: no allocation, no lock,
 * no call back into AROS. The reader synchronises on rx_head alone, which this
 * is the only writer of, so no interlock is needed for a single producer and a
 * single consumer.
 */
AROS_INTH1(btuart_rx_handler, struct BTUARTBase *, BTUARTBase)
{
    AROS_INTFUNC_INIT

    ULONG head = BTUARTBase->rx_head;

    while (!(mmio_read(BTUARTBase->uart_base, PL011_FR) & PL011_FR_RXFE))
    {
        ULONG dr = mmio_read(BTUARTBase->uart_base, PL011_DR);
        ULONG next = (head + 1) & (BTUART_RX_RING - 1);

        if ((dr & PL011_DR_ERR) != 0 && BTUARTBase->rx_errors < 8)
        {
            BTUARTBase->rx_errors++;
            bug("[BTUART] rx status 0x%lx%s\n", (dr >> 8) & 0xf,
                (dr & PL011_DR_OE) ? " OVERRUN" : "");
        }
        if (next == BTUARTBase->rx_tail)
        {
            /* Reader is too far behind. Dropping here is still better than the
             * FIFO overrunning, because it is counted. */
            BTUARTBase->rx_dropped++;
            break;
        }
        BTUARTBase->rx_ring[head] = (UBYTE)(dr & 0xff);
        head = next;
    }
    BTUARTBase->rx_head = head;

    /* Acknowledge receive and receive-timeout; overrun is cleared with them so
     * a single lost byte does not latch the condition forever. */
    mmio_write(BTUARTBase->uart_base, PL011_ICR,
               PL011_INT_RX | PL011_INT_RT | PL011_INT_OE);
    return FALSE;

    AROS_INTFUNC_EXIT
}

AROS_LH1(long, BTUARTClaim,
    AROS_LHA(void *, owner, A0),
    struct BTUARTBase *, BTUARTBase, 3, Btuart)
{
    AROS_LIBFUNC_INIT

    LONG result = BTUART_OK;

    if (owner == NULL)
        return BTUART_ERR_ARGUMENT;
    ObtainSemaphore(&BTUARTBase->lock);
    if (BTUARTBase->owner != NULL)
        result = BTUART_ERR_BUSY;
    else
        BTUARTBase->owner = owner;
    ReleaseSemaphore(&BTUARTBase->lock);
    return result;

    AROS_LIBFUNC_EXIT
}

AROS_LH1(void, BTUARTRelease,
    AROS_LHA(void *, owner, A0),
    struct BTUARTBase *, BTUARTBase, 4, Btuart)
{
    AROS_LIBFUNC_INIT

    ObtainSemaphore(&BTUARTBase->lock);
    if (owner != NULL && BTUARTBase->owner == owner)
    {
        mmio_write(BTUARTBase->uart_base, PL011_IMSC, 0);
        mmio_write(BTUARTBase->uart_base, PL011_CR, 0);
        BTUARTBase->owner = NULL;
        BTUARTBase->baud = 0;
        BTUARTBase->config_flags = 0;
    }
    ReleaseSemaphore(&BTUARTBase->lock);

    AROS_LIBFUNC_EXIT
}

AROS_LH3(long, BTUARTConfigure,
    AROS_LHA(void *, owner, A0),
    AROS_LHA(unsigned long, baud, D0),
    AROS_LHA(unsigned long, flags, D1),
    struct BTUARTBase *, BTUARTBase, 5, Btuart)
{
    AROS_LIBFUNC_INIT

    ULONG divisor;
    ULONG integer;
    ULONG fraction;
    ULONG control;
    ULONG wait = PL011_WAIT_LIMIT;

    if (owner == NULL || baud == 0)
        return BTUART_ERR_ARGUMENT;
    ObtainSemaphore(&BTUARTBase->lock);
    if (BTUARTBase->owner != owner)
    {
        ReleaseSemaphore(&BTUARTBase->lock);
        return BTUART_ERR_NOT_OWNER;
    }

    if (setup_lpo_clock(BTUARTBase) != BTUART_OK)
    {
        bug("[BTUART] configure: LPO clock timeout\n");
        ReleaseSemaphore(&BTUARTBase->lock);
        return BTUART_ERR_TIMEOUT;
    }
    route_bluetooth_uart(BTUARTBase);
    mmio_write(BTUARTBase->uart_base, PL011_CR, 0);
    while ((mmio_read(BTUARTBase->uart_base, PL011_FR) & PL011_FR_BUSY) &&
           --wait != 0)
        ;
    if (wait == 0)
    {
        bug("[BTUART] configure: PL011 busy timeout\n");
        ReleaseSemaphore(&BTUARTBase->lock);
        return BTUART_ERR_TIMEOUT;
    }

    mmio_write(BTUARTBase->uart_base, PL011_LCRH, 0);
    mmio_write(BTUARTBase->uart_base, PL011_IMSC, 0);
    mmio_write(BTUARTBase->uart_base, PL011_ICR, 0x7ff);

    divisor = (BTUARTBase->uart_clock_hz * 4UL + baud / 2UL) / baud;
    integer = divisor >> 6;
    fraction = divisor & 0x3f;
    if (integer == 0 || integer > 0xffff)
    {
        ReleaseSemaphore(&BTUARTBase->lock);
        return BTUART_ERR_ARGUMENT;
    }
    mmio_write(BTUARTBase->uart_base, PL011_IBRD, integer);
    mmio_write(BTUARTBase->uart_base, PL011_FBRD, fraction);
    mmio_write(BTUARTBase->uart_base, PL011_LCRH,
               PL011_LCRH_WLEN8 | PL011_LCRH_FEN);

    control = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
    if (flags & BTUART_CONFIG_RTS_CTS)
        control |= PL011_CR_RTSEN | PL011_CR_CTSEN;
    mmio_write(BTUARTBase->uart_base, PL011_CR, control);

    /*
     * Arm receive interrupts, once, after the port is configured.
     *
     * Order matters: the watermark and the mask are cleared by the LCRH write
     * above, and enabling the interrupt before UARTEN would deliver on a port
     * that is not receiving. Registration is idempotent because a second
     * Configure on the same owner is legitimate -- a baud change, for one.
     */
    mmio_write(BTUARTBase->uart_base, PL011_IFLS, PL011_IFLS_RX18);
    if (!BTUARTBase->rx_irq_armed)
    {
        BTUARTBase->rx_irq.is_Node.ln_Name = "btuart";
        BTUARTBase->rx_irq.is_Node.ln_Pri = 0;
        BTUARTBase->rx_irq.is_Code = (APTR)btuart_rx_handler;
        BTUARTBase->rx_irq.is_Data = BTUARTBase;
        if (KrnAddIRQHandler(IRQ_VC_UART, btuart_rx_handler, BTUARTBase, NULL))
        {
            BTUARTBase->rx_irq_armed = TRUE;
            BTUARTBase->capabilities |= BTUART_CAP_RX_INTERRUPT;
            bug("[BTUART] rx interrupt armed on irq %lu\n",
                (ULONG)IRQ_VC_UART);
        }
        else
            bug("[BTUART] KrnAddIRQHandler(%lu) failed -- receive stays polled\n",
                (ULONG)IRQ_VC_UART);
    }
    mmio_write(BTUARTBase->uart_base, PL011_ICR, 0x7ff);
    if (BTUARTBase->rx_irq_armed)
        mmio_write(BTUARTBase->uart_base, PL011_IMSC,
                   PL011_INT_RX | PL011_INT_RT);
    BTUARTBase->baud = baud;
    BTUARTBase->config_flags = flags;
    bug("[BTUART] configure: baud=%lu clock=%lu divisor=%lu/%lu flags=0x%lx\n",
        baud, BTUARTBase->uart_clock_hz, integer, fraction, flags);
    ReleaseSemaphore(&BTUARTBase->lock);
    return BTUART_OK;

    AROS_LIBFUNC_EXIT
}

AROS_LH2(long, BTUARTSetPower,
    AROS_LHA(void *, owner, A0),
    AROS_LHA(unsigned long, enabled, D0),
    struct BTUARTBase *, BTUARTBase, 6, Btuart)
{
    AROS_LIBFUNC_INIT

    if (owner == NULL)
        return BTUART_ERR_ARGUMENT;
    if (BTUARTBase->owner != owner)
        return BTUART_ERR_NOT_OWNER;
    return firmware_gpio_set(FW_GPIO_BT_REG_EN, enabled);

    AROS_LIBFUNC_EXIT
}

AROS_LH3(long, BTUARTWrite,
    AROS_LHA(void *, owner, A0),
    AROS_LHA(const void *, data, A1),
    AROS_LHA(unsigned long, length, D0),
    struct BTUARTBase *, BTUARTBase, 7, Btuart)
{
    AROS_LIBFUNC_INIT

    const UBYTE *bytes = data;
    ULONG written = 0;

    if (owner == NULL || (data == NULL && length != 0))
        return BTUART_ERR_ARGUMENT;
    if (BTUARTBase->owner != owner)
        return BTUART_ERR_NOT_OWNER;
    while (written < length &&
           !(mmio_read(BTUARTBase->uart_base, PL011_FR) & PL011_FR_TXFF))
    {
        mmio_write(BTUARTBase->uart_base, PL011_DR, bytes[written]);
        written++;
    }
    return (LONG)written;

    AROS_LIBFUNC_EXIT
}

AROS_LH3(long, BTUARTRead,
    AROS_LHA(void *, owner, A0),
    AROS_LHA(void *, data, A1),
    AROS_LHA(unsigned long, capacity, D0),
    struct BTUARTBase *, BTUARTBase, 8, Btuart)
{
    AROS_LIBFUNC_INIT

    UBYTE *bytes = data;
    ULONG read = 0;

    if (owner == NULL || (data == NULL && capacity != 0))
        return BTUART_ERR_ARGUMENT;
    if (BTUARTBase->owner != owner)
        return BTUART_ERR_NOT_OWNER;
    /*
     * From the ring, not from the FIFO.
     *
     * Reading the FIFO here made the caller's scheduling the deadline for the
     * hardware, and no schedule was fast enough: OVERRUN was reported on real
     * hardware even at a one-millisecond poll. The interrupt owns the FIFO now
     * and this owns rx_tail, so a slow reader costs latency rather than data.
     */
    while (read < capacity && BTUARTBase->rx_tail != BTUARTBase->rx_head)
    {
        bytes[read] = BTUARTBase->rx_ring[BTUARTBase->rx_tail];
        BTUARTBase->rx_tail = (BTUARTBase->rx_tail + 1) & (BTUART_RX_RING - 1);
        read++;
    }
    if (BTUARTBase->rx_dropped != 0)
    {
        bug("[BTUART] rx ring overflow, %lu byte(s) dropped\n",
            BTUARTBase->rx_dropped);
        BTUARTBase->rx_dropped = 0;
    }
    return (LONG)read;

    AROS_LIBFUNC_EXIT
}

AROS_LH0(unsigned int, BTUARTGetAPIVersion,
    struct BTUARTBase *, BTUARTBase, 1, Btuart)
{
    AROS_LIBFUNC_INIT

    return BTUART_API_VERSION;

    AROS_LIBFUNC_EXIT
}

AROS_LH0(unsigned long, BTUARTGetCapabilities,
    struct BTUARTBase *, BTUARTBase, 2, Btuart)
{
    AROS_LIBFUNC_INIT

    return BTUARTBase->capabilities;

    AROS_LIBFUNC_EXIT
}

/*
 * Register the init with genmodule's INITLIB set.
 *
 * Without this line btuart_init() is a static function nobody references, so
 * the compiler drops it along with everything only it calls -- and genmodule,
 * having no init to run, adds the resource anyway. OpenResource() then hands
 * out a base whose fields are all zero, which is worse than a missing resource
 * because it looks like a working one: BTUARTGetAPIVersion() answers from a
 * constant, and the first call that touches the base hangs. ObtainSemaphore()
 * on a zeroed SignalSemaphore blocks forever, because InitSemaphore() sets
 * ss_QueueCount to -1 and zero reads as "already owned by someone else".
 *
 * arch/m68k-native/soc/mbox/mbox_init.c:241 does the same for the same reason.
 */
ADD2INITLIB(btuart_init, 0)
