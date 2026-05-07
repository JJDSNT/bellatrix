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
 * GPIO 14/15 mux: kprintf uses PL011 via the firmware's UART initialisation.
 * We re-init the PL011 here; kprintf continues to write the same registers so
 * boot messages still arrive.  Once Kickstart/DiagROM takes over serial,
 * kprintf goes quiet and the terminal becomes a clean Amiga serial console.
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
#define PL011_BASE         (ARM_PERI_VIRT_BASE + 0x201000UL)

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

/* 48 MHz PL011 clock */
#define PL011_CLK_HZ 48000000UL

bool pl011_backend_open(PL011Backend *b, uint32_t baud)
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

    /* Enable UART + TX + RX */
    pl_wr32(PL011_CR, CR_UARTEN | CR_TXE | CR_RXE);

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
    /* For the bridged Amiga serial path, prefer forward progress over TXFF
     * polling. We emit at a very low rate compared to the PL011 FIFO depth,
     * and the existing TXFF gate appears to wedge the first byte on bare
     * metal even though Emu68's own PL011 console keeps working. */
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

#endif
