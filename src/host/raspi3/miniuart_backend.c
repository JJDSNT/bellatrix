#include "io/serial/miniuart_backend.h"

#if defined(BELLATRIX_ENABLE_MINIUART_BACKEND)

/*
 * BCM2835 AUX mini-UART (UART1).
 *
 * Emu68 runs in big-endian AArch64; all peripheral registers are little-endian.
 * Define endianness-aware accessors locally to avoid pulling in support.h
 * (which drags in A64.h and other Emu68 internals).
 *
 * Bellatrix runs under Emu68's MMU layout, so use the same 0xf2000000
 * peripheral alias Emu68 uses itself, not the raw 0x3f21xxxx physical range.
 *
 * On QEMU raspi3b the second -serial argument maps to this peripheral.
 * On real hardware GPIO 14/15 must be muxed to ALT5 before opening the
 * mini-UART. Bellatrix does this from Emu68's setup_serial() path so there
 * is no PL011-to-mini-UART handoff later in boot; QEMU ignores GPIO mux.
 */

#include <stdint.h>

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline void     mu_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = __builtin_bswap32(v); }
static inline uint32_t mu_rd32(uintptr_t a)             { return __builtin_bswap32(*(volatile uint32_t *)a); }
#else
static inline void     mu_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t mu_rd32(uintptr_t a)             { return *(volatile uint32_t *)a; }
#endif

#define ARM_PERI_VIRT_BASE 0xF2000000UL
#define AUX_ENABLES_ADDR    (ARM_PERI_VIRT_BASE + 0x215004UL)
#define AUX_MU_IO_ADDR      (ARM_PERI_VIRT_BASE + 0x215040UL)
#define AUX_MU_IER_ADDR     (ARM_PERI_VIRT_BASE + 0x215044UL)
#define AUX_MU_IIR_ADDR     (ARM_PERI_VIRT_BASE + 0x215048UL)
#define AUX_MU_LCR_ADDR     (ARM_PERI_VIRT_BASE + 0x21504CUL)
#define AUX_MU_MCR_ADDR     (ARM_PERI_VIRT_BASE + 0x215050UL)
#define AUX_MU_LSR_ADDR     (ARM_PERI_VIRT_BASE + 0x215054UL)
#define AUX_MU_CNTL_ADDR    (ARM_PERI_VIRT_BASE + 0x215060UL)
#define AUX_MU_BAUD_ADDR    (ARM_PERI_VIRT_BASE + 0x215068UL)

/* raspi3 / BCM2837 system clock: 250 MHz */
#define SYS_CLK_HZ 250000000UL

bool miniuart_backend_open_clk(MiniUartBackend *m, uint32_t baud, uint32_t clk_hz)
{
    if (!m || !baud || !clk_hz) return false;

    mu_wr32(AUX_ENABLES_ADDR, mu_rd32(AUX_ENABLES_ADDR) | 1u);
    mu_wr32(AUX_MU_CNTL_ADDR, 0);
    mu_wr32(AUX_MU_IER_ADDR,  0);
    mu_wr32(AUX_MU_LCR_ADDR,  3);
    mu_wr32(AUX_MU_MCR_ADDR,  0);
    mu_wr32(AUX_MU_IIR_ADDR,  0xC6u);
    mu_wr32(AUX_MU_BAUD_ADDR, clk_hz / (8u * baud) - 1u);
    mu_wr32(AUX_MU_CNTL_ADDR, 3);

    m->baud = baud;
    m->open = true;
    return true;
}

bool miniuart_backend_set_baud_clk(MiniUartBackend *m, uint32_t baud, uint32_t clk_hz)
{
    if (!m || !m->open || !baud || !clk_hz) return false;
    mu_wr32(AUX_MU_BAUD_ADDR, clk_hz / (8u * baud) - 1u);
    m->baud = baud;
    return true;
}

bool miniuart_backend_open(MiniUartBackend *m, uint32_t baud)
{
    return miniuart_backend_open_clk(m, baud, SYS_CLK_HZ);
}

uint32_t miniuart_backend_read_lsr(void)
{
    return mu_rd32(AUX_MU_LSR_ADDR);
}

void miniuart_backend_close(MiniUartBackend *m)
{
    if (!m || !m->open) return;
    mu_wr32(AUX_MU_CNTL_ADDR, 0);
    m->open = false;
}

bool miniuart_backend_is_open(const MiniUartBackend *m)
{
    return m && m->open;
}

bool miniuart_backend_read_byte(MiniUartBackend *m, uint8_t *byte_out)
{
    if (!m || !m->open || !byte_out) return false;
    if (!(mu_rd32(AUX_MU_LSR_ADDR) & 0x01u)) return false;
    *byte_out = (uint8_t)(mu_rd32(AUX_MU_IO_ADDR) & 0xFFu);
    return true;
}

bool miniuart_backend_write_byte(MiniUartBackend *m, uint8_t byte)
{
    if (!m || !m->open) return false;
    /* LSR bit 5 = TX FIFO has space. Return false instead of spinning so
     * shared users can retry without stalling the machine step loop. */
    if (!(mu_rd32(AUX_MU_LSR_ADDR) & 0x20u))
        return false;
    mu_wr32(AUX_MU_IO_ADDR, byte);
    return true;
}

#else  /* stubs — compiled in harness and non-raspi3 builds */

bool miniuart_backend_open(MiniUartBackend *m, uint32_t baud)
{
    (void)baud;
    if (m) m->open = false;
    return false;
}

bool miniuart_backend_open_clk(MiniUartBackend *m, uint32_t baud, uint32_t clk_hz)
{
    (void)baud; (void)clk_hz;
    if (m) m->open = false;
    return false;
}

bool miniuart_backend_set_baud_clk(MiniUartBackend *m, uint32_t baud, uint32_t clk_hz)
{
    (void)m; (void)baud; (void)clk_hz;
    return false;
}

uint32_t miniuart_backend_read_lsr(void)
{
    return 0;
}

void miniuart_backend_close(MiniUartBackend *m)
{
    if (m) m->open = false;
}

bool miniuart_backend_is_open(const MiniUartBackend *m)
{
    return m && m->open;
}

bool miniuart_backend_read_byte(MiniUartBackend *m, uint8_t *byte_out)
{
    (void)m; (void)byte_out;
    return false;
}

bool miniuart_backend_write_byte(MiniUartBackend *m, uint8_t byte)
{
    (void)m; (void)byte;
    return false;
}

#endif
