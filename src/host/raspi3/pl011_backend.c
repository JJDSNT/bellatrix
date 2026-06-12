#include "host/raspi3/pl011_backend.h"

#if defined(BELLATRIX_ENABLE_PL011_BACKEND)

/*
 * BCM2835 PL011 (UART0) — bidirectional Amiga serial backend.
 *
 * Emu68 runs in big-endian AArch64; peripheral registers are little-endian.
 * Use self-contained bswap helpers — no support.h dependency.
 *
 * Important: Bellatrix runs under Emu68's MMU layout, so peripheral MMIO must
 * use the same 0xf2000000 virtual alias as Emu68's own kprintf/setup_serial
 * path. Accessing the raw 0x3f20xxxx physical window here can hang/fault once
 * the Amiga serial bridge starts touching the UART.
 *
 * Clock: 48 MHz (BCM2837 PL011 UART clock, set by firmware via mailbox).
 * 115200 baud: IBRD = 26, FBRD = 3  (error < 0.04 %).
 *
 * The same PL011 can be routed either to the 40-pin header (GPIO 14/15) or to
 * the Pi 3B's on-board Bluetooth path (GPIO 32/33 plus GPCLK2 on GPIO 43).
 * Bellatrix switches between those routes explicitly depending on ownership.
 */

#include <stdint.h>

int kprintf(const char *fmt, ...);

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline void     pl_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = __builtin_bswap32(v); }
static inline uint32_t pl_rd32(uintptr_t a)             { return __builtin_bswap32(*(volatile uint32_t *)a); }
#else
static inline void     pl_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t pl_rd32(uintptr_t a)             { return *(volatile uint32_t *)a; }
#endif

#define ARM_PERI_VIRT_BASE 0xF2000000UL
#define GPIO_BASE          (ARM_PERI_VIRT_BASE + 0x200000UL)
#define PL011_BASE         (ARM_PERI_VIRT_BASE + 0x201000UL)

#define GPFSEL1    (GPIO_BASE + 0x004UL)
#define GPFSEL3    (GPIO_BASE + 0x00CUL)
#define GPFSEL4    (GPIO_BASE + 0x010UL)
#define GPPUD      (GPIO_BASE + 0x094UL)
#define GPPUDCLK0  (GPIO_BASE + 0x098UL)
#define GPPUDCLK1  (GPIO_BASE + 0x09CUL)

#define PL011_DR      (PL011_BASE + 0x000UL)  /* data register            */
#define PL011_FR      (PL011_BASE + 0x018UL)  /* flag register            */
#define PL011_IBRD    (PL011_BASE + 0x024UL)  /* integer baud divisor     */
#define PL011_FBRD    (PL011_BASE + 0x028UL)  /* fractional baud divisor  */
#define PL011_LCRH    (PL011_BASE + 0x02CUL)  /* line control             */
#define PL011_CR      (PL011_BASE + 0x030UL)  /* control                  */
#define PL011_IMSC    (PL011_BASE + 0x038UL)  /* interrupt mask           */
#define PL011_ICR     (PL011_BASE + 0x044UL)  /* interrupt clear          */

/* FR bits */
#define FR_BUSY  (1u << 3)
#define FR_RXFE  (1u << 4)   /* RX FIFO empty */
#define FR_TXFF  (1u << 5)   /* TX FIFO full  */

/* LCRH bits */
#define LCRH_FEN    (1u << 4)          /* enable FIFOs          */
#define LCRH_WLEN8  (3u << 5)          /* 8-bit word length     */

/* CR bits */
#define CR_UARTEN (1u << 0)
#define CR_TXE    (1u << 8)
#define CR_RXE    (1u << 9)
#define CR_RTSEN  (1u << 14)   /* hardware nUARTRTS from RX FIFO level */
#define CR_CTSEN  (1u << 15)   /* gate TX on nUARTCTS                  */

/* 48 MHz PL011 clock */
#define PL011_CLK_HZ 48000000UL

#define GPIO_FSEL_INPUT 0u
#define GPIO_FSEL_ALT0  4u
#define GPIO_FSEL_ALT3  7u

static void gpio_wait_cycles(void)
{
    int i;
    for (i = 0; i < 150; ++i) {
        asm volatile ("nop");
    }
}

static void gpio_set_pull_none(uint32_t mask0, uint32_t mask1)
{
    pl_wr32(GPPUD, 0);
    gpio_wait_cycles();
    if (mask0) {
        pl_wr32(GPPUDCLK0, mask0);
    }
    if (mask1) {
        pl_wr32(GPPUDCLK1, mask1);
    }
    gpio_wait_cycles();
    if (mask0) {
        pl_wr32(GPPUDCLK0, 0);
    }
    if (mask1) {
        pl_wr32(GPPUDCLK1, 0);
    }
}

static uint32_t gpio_fsel_update(uint32_t reg, unsigned gpio, uint32_t func)
{
    uint32_t shift = (gpio % 10u) * 3u;
    reg &= ~(7u << shift);
    reg |= (func & 7u) << shift;
    return reg;
}

bool pl011_backend_route_header_console(void)
{
    uint32_t sel1 = pl_rd32(GPFSEL1);
    uint32_t sel3 = pl_rd32(GPFSEL3);
    uint32_t sel4 = pl_rd32(GPFSEL4);

    sel1 = gpio_fsel_update(sel1, 14u, GPIO_FSEL_ALT0);
    sel1 = gpio_fsel_update(sel1, 15u, GPIO_FSEL_ALT0);
    sel3 = gpio_fsel_update(sel3, 32u, GPIO_FSEL_INPUT);
    sel3 = gpio_fsel_update(sel3, 33u, GPIO_FSEL_INPUT);
    sel4 = gpio_fsel_update(sel4, 43u, GPIO_FSEL_INPUT);

    pl_wr32(GPFSEL1, sel1);
    pl_wr32(GPFSEL3, sel3);
    pl_wr32(GPFSEL4, sel4);
    gpio_set_pull_none((1u << 14) | (1u << 15), (1u << 0) | (1u << 1) | (1u << 11));
    return true;
}

bool pl011_backend_route_bluetooth_pi3(void)
{
    uint32_t sel1 = pl_rd32(GPFSEL1);
    uint32_t sel3 = pl_rd32(GPFSEL3);
    uint32_t sel4 = pl_rd32(GPFSEL4);

    sel1 = gpio_fsel_update(sel1, 14u, GPIO_FSEL_INPUT);
    sel1 = gpio_fsel_update(sel1, 15u, GPIO_FSEL_INPUT);
    /* GPIO 30/31 carry the BCM4343x CTS0/RTS0 pair on the Pi 3B — hardware
     * flow control is what keeps the 16-byte RX FIFO from overrunning at the
     * 921600-baud H5 transport between polls. */
    sel3 = gpio_fsel_update(sel3, 30u, GPIO_FSEL_ALT3);
    sel3 = gpio_fsel_update(sel3, 31u, GPIO_FSEL_ALT3);
    sel3 = gpio_fsel_update(sel3, 32u, GPIO_FSEL_ALT3);
    sel3 = gpio_fsel_update(sel3, 33u, GPIO_FSEL_ALT3);
    sel4 = gpio_fsel_update(sel4, 43u, GPIO_FSEL_ALT0);

    pl_wr32(GPFSEL1, sel1);
    pl_wr32(GPFSEL3, sel3);
    pl_wr32(GPFSEL4, sel4);
    gpio_set_pull_none((1u << 14) | (1u << 15) | (1u << 30) | (1u << 31),
                       (1u << 0) | (1u << 1) | (1u << 11));
    return true;
}

/*
 * BT LPO clock: the BCM4343x sleep/radio timing domain needs a 32.768 kHz
 * reference, fed on the Pi 3B from GPCLK2 via GPIO 43.  Routing the pin is
 * not enough — the Clock Manager GP2 generator must be programmed and
 * enabled.  19.2 MHz osc / 585.9375 = 32768.0 Hz exactly (MASH-1).
 * Symptoms of a missing LPO: host commands fine, radio ops (inquiry
 * complete, paging) hang or crash the firmware.
 */
#define CM_GP2CTL  (ARM_PERI_VIRT_BASE + 0x101080UL)
#define CM_GP2DIV  (ARM_PERI_VIRT_BASE + 0x101084UL)
#define CM_PASSWD  0x5A000000u
#define CM_CTL_ENAB (1u << 4)
#define CM_CTL_BUSY (1u << 7)

uint32_t pl011_backend_setup_bt_lpo(uint32_t *old_ctl, uint32_t *old_div)
{
    uint32_t ctl = pl_rd32(CM_GP2CTL);
    uint32_t div = pl_rd32(CM_GP2DIV);
    int timeout = 1000000;

    if (old_ctl) *old_ctl = ctl;
    if (old_div) *old_div = div;

    /* route GPIO 43 to GPCLK2 (ALT0) before the chip leaves reset */
    pl_wr32(GPFSEL4, gpio_fsel_update(pl_rd32(GPFSEL4), 43u, GPIO_FSEL_ALT0));

    pl_wr32(CM_GP2CTL, CM_PASSWD | (ctl & ~(CM_CTL_ENAB | (0xFFu << 24))));
    while ((pl_rd32(CM_GP2CTL) & CM_CTL_BUSY) && --timeout > 0) {
        ;
    }

    /* DIVI=585, DIVF=3840/4096 (= .9375); MASH stage 1; source 1 = 19.2 MHz osc */
    pl_wr32(CM_GP2DIV, CM_PASSWD | (585u << 12) | 3840u);
    pl_wr32(CM_GP2CTL, CM_PASSWD | (1u << 9) | 1u);
    pl_wr32(CM_GP2CTL, CM_PASSWD | (1u << 9) | 1u | CM_CTL_ENAB);

    return pl_rd32(CM_GP2CTL);
}

void pl011_backend_wait_idle(void)
{
    int timeout = 1000000;

    while ((pl_rd32(PL011_FR) & FR_BUSY) && --timeout > 0) {
        ;
    }
}

bool pl011_backend_open(PL011Backend *b, uint32_t baud)
{
    return pl011_backend_open_flow(b, baud, false);
}

bool pl011_backend_open_flow(PL011Backend *b, uint32_t baud, bool flow)
{
    if (!b || baud == 0) return false;

    /* Disable UART */
    pl_wr32(PL011_CR, 0);

    /* Spin until not busy */
    int timeout = 1000000;
    while ((pl_rd32(PL011_FR) & FR_BUSY) && --timeout > 0)
        ;

    /* Flush TX FIFO by clearing FEN */
    pl_wr32(PL011_LCRH, 0);

    /* Clear + disable all interrupts */
    pl_wr32(PL011_ICR,  0x7FFu);
    pl_wr32(PL011_IMSC, 0);

    /* Baud rate: BAUDDIV = CLK / (16 * baud) */
    uint32_t bauddiv_int  = PL011_CLK_HZ / (16u * baud);
    uint32_t bauddiv_frac = (uint32_t)(((uint64_t)(PL011_CLK_HZ % (16u * baud)) * 64u
                                        + (8u * baud)) / (16u * baud));

    pl_wr32(PL011_IBRD, bauddiv_int);
    pl_wr32(PL011_FBRD, bauddiv_frac);

    /* 8N1, FIFOs enabled */
    pl_wr32(PL011_LCRH, LCRH_WLEN8 | LCRH_FEN);

    /* Enable UART + TX + RX (+ hardware flow control when requested) */
    pl_wr32(PL011_CR, CR_UARTEN | CR_TXE | CR_RXE |
                      (flow ? (CR_RTSEN | CR_CTSEN) : 0u));

    b->baud = baud;
    b->open = true;
    return true;
}

void pl011_backend_close(PL011Backend *b)
{
    if (!b || !b->open) return;
    pl_wr32(PL011_CR, 0);
    b->open = false;
}

bool pl011_backend_is_open(const PL011Backend *b)
{
    return b && b->open;
}

bool pl011_backend_read_byte(PL011Backend *b, uint8_t *byte_out)
{
    if (!b || !b->open || !byte_out) return false;
    if (pl_rd32(PL011_FR) & FR_RXFE) return false;
    *byte_out = (uint8_t)(pl_rd32(PL011_DR) & 0xFFu);
    return true;
}

bool pl011_backend_write_byte(PL011Backend *b, uint8_t byte)
{
    if (!b || !b->open) return false;
    static int s_first_pl011_write_logged = 0;
    if (!s_first_pl011_write_logged) {
        s_first_pl011_write_logged = 1;
        kprintf("[PL011-WRITE] first byte=%02x CR=%08x FR=%08x\n",
                (unsigned)byte,
                (unsigned)pl_rd32(PL011_CR),
                (unsigned)pl_rd32(PL011_FR));
    }
    /* The PL011 TX FIFO is only 16 bytes deep; writes while TXFF is set are
     * silently dropped by the hardware.  Every HCI command <= 16 bytes worked
     * and the first one above (74-byte PatchRAM Write_RAM) died truncated —
     * the caller must treat 'false' as "FIFO full, retry on next poll". */
    if (pl_rd32(PL011_FR) & FR_TXFF) {
        return false;
    }
    pl_wr32(PL011_DR, byte);
    return true;
}

#else  /* stubs — harness and non-raspi3 builds */

bool pl011_backend_open(PL011Backend *b, uint32_t baud)
{
    (void)baud;
    if (b) b->open = false;
    return false;
}

bool pl011_backend_open_flow(PL011Backend *b, uint32_t baud, bool flow)
{
    (void)baud; (void)flow;
    if (b) b->open = false;
    return false;
}

void pl011_backend_close(PL011Backend *b)
{
    if (b) b->open = false;
}

bool pl011_backend_is_open(const PL011Backend *b)
{
    return b && b->open;
}

bool pl011_backend_read_byte(PL011Backend *b, uint8_t *byte_out)
{
    (void)b; (void)byte_out;
    return false;
}

bool pl011_backend_write_byte(PL011Backend *b, uint8_t byte)
{
    (void)b; (void)byte;
    return false;
}

uint32_t pl011_backend_setup_bt_lpo(uint32_t *old_ctl, uint32_t *old_div)
{
    if (old_ctl) *old_ctl = 0u;
    if (old_div) *old_div = 0u;
    return 0u;
}

#endif
