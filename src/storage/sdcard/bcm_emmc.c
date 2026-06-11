// src/storage/sdcard/bcm_emmc.c
// BCM2837 Arasan SDHCI driver for Raspberry Pi 3.
// Polling mode, read-only, supports SDSC/SDHC/SDXC.
//
// Emu68 maps peripherals at virtual 0xF2000000 (physical 0x3F000000).
// EMMC controller is at peripheral offset 0x300000.
//
// Endianness: control registers use bswap32 (same as pl011_backend.c).
// Data register: raw 32-bit reads — byte order is correct without swapping.

#include "storage/sdcard/bcm_emmc.h"
#include "support_rpi.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static inline void nop_delay(volatile uint32_t n) { while (n--) asm volatile("nop"); }

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline void     wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = __builtin_bswap32(v); }
static inline uint32_t rd32(uintptr_t a)             { return __builtin_bswap32(*(volatile uint32_t *)a); }
#else
static inline void     wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t rd32(uintptr_t a)             { return *(volatile uint32_t *)a; }
#endif

/* Raw 32-bit read for DATA FIFO — no bswap; byte order is correct as-is */
static inline uint32_t rd32_data(uintptr_t a)
{
    return *(volatile uint32_t *)a;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

#define ARM_PERI_VIRT_BASE  0xF2000000UL
#define EMMC_BASE           (ARM_PERI_VIRT_BASE + 0x300000UL)

#define EMMC_ARG2           (EMMC_BASE + 0x00UL)
#define EMMC_BLKSIZECNT     (EMMC_BASE + 0x04UL)
#define EMMC_ARG1           (EMMC_BASE + 0x08UL)
#define EMMC_CMDTM          (EMMC_BASE + 0x0CUL)
#define EMMC_RESP0          (EMMC_BASE + 0x10UL)
#define EMMC_RESP1          (EMMC_BASE + 0x14UL)
#define EMMC_RESP2          (EMMC_BASE + 0x18UL)
#define EMMC_RESP3          (EMMC_BASE + 0x1CUL)
#define EMMC_DATA           (EMMC_BASE + 0x20UL)
#define EMMC_STATUS         (EMMC_BASE + 0x24UL)
#define EMMC_CONTROL0       (EMMC_BASE + 0x28UL)
#define EMMC_CONTROL1       (EMMC_BASE + 0x2CUL)
#define EMMC_INTERRUPT      (EMMC_BASE + 0x30UL)
#define EMMC_IRPT_MASK      (EMMC_BASE + 0x34UL)
#define EMMC_IRPT_EN        (EMMC_BASE + 0x38UL)
#define EMMC_SLOTISR_VER    (EMMC_BASE + 0xFCUL)

// CONTROL1 bits
#define CTRL1_CLK_INTLEN    (1u << 0)
#define CTRL1_CLK_STABLE    (1u << 1)
#define CTRL1_CLK_EN        (1u << 2)
#define CTRL1_SRST_HC       (1u << 24)
#define CTRL1_SRST_CMD      (1u << 25)
#define CTRL1_SRST_DATA     (1u << 26)

// INTERRUPT bits
#define INT_CMD_DONE        (1u << 0)
#define INT_DATA_DONE       (1u << 1)
#define INT_WRITE_RDY       (1u << 4)
#define INT_READ_RDY        (1u << 5)
#define INT_ERR             (1u << 15)
#define INT_CTO_ERR         (1u << 16)
#define INT_CCRC_ERR        (1u << 17)
#define INT_CEND_ERR        (1u << 18)
#define INT_CBAD_ERR        (1u << 19)
#define INT_DTO_ERR         (1u << 20)
#define INT_DCRC_ERR        (1u << 21)
#define INT_DEND_ERR        (1u << 22)

#define INT_ERR_MASK        (INT_CTO_ERR | INT_CCRC_ERR | INT_CEND_ERR | \
                             INT_CBAD_ERR | INT_DTO_ERR | INT_DCRC_ERR | \
                             INT_DEND_ERR | INT_ERR)

// STATUS bits
#define STATUS_CMD_INHIBIT  (1u << 0)
#define STATUS_DAT_INHIBIT  (1u << 1)

// CMDTM fields
#define CMDTM_RESP_NONE     (0u << 16)
#define CMDTM_RESP_136      (1u << 16)
#define CMDTM_RESP_48       (2u << 16)
#define CMDTM_RESP_48_BUSY  (3u << 16)
#define CMDTM_CRCCHK_EN     (1u << 19)
#define CMDTM_IXCHK_EN      (1u << 20)
#define CMDTM_ISDATA        (1u << 21)
#define CMDTM_DAT_READ      (1u << 4)
#define CMDTM_BLKCNT_EN     (1u << 1)
#define CMDTM_MULTI         (1u << 5)
#define CMDTM_AUTO_CMD12    (1u << 2)
#define CMDTM_IDX(n)        ((uint32_t)(n) << 24)

// Clock ID for EMMC (VideoCore mailbox)
#define CLOCK_ID_EMMC       1u

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool     s_initialized  = false;
static bool     s_is_hc        = false;   // SDHC/SDXC — block addressing
static uint32_t s_rca          = 0;

// ---------------------------------------------------------------------------
// Interrupt helpers
// ---------------------------------------------------------------------------

// Wait for one or more interrupt flags; returns the INTERRUPT register value.
// Returns 0xFFFFFFFF on timeout.
static uint32_t emmc_wait_int(uint32_t mask)
{
    uint32_t v;
    uint32_t timeout = 1000000u;

    while (timeout--) {
        v = rd32(EMMC_INTERRUPT);
        if (v & mask) {
            return v;
        }
        if (v & INT_ERR_MASK) {
            wr32(EMMC_INTERRUPT, v);
            return 0xFFFFFFFFu;
        }
        nop_delay(4);
    }

    return 0xFFFFFFFFu;
}

static void emmc_clear_int(uint32_t mask)
{
    wr32(EMMC_INTERRUPT, mask);
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

static bool emmc_set_clock(uint32_t target_hz)
{
    uint32_t base = get_clock_rate(CLOCK_ID_EMMC);
    if (base == 0u) {
        base = 100000000u;   // assume 100 MHz if mailbox fails
    }

    // Disable SD clock before changing divider
    uint32_t ctrl1 = rd32(EMMC_CONTROL1);
    ctrl1 &= ~CTRL1_CLK_EN;
    wr32(EMMC_CONTROL1, ctrl1);
    nop_delay(100);

    // Compute 10-bit divider (bits[15:8] = low 8, bits[7:6] = high 2)
    uint32_t div = base / (target_hz * 2u);
    if (div == 0u) div = 1u;
    if (div > 0x3FFu) div = 0x3FFu;

    uint32_t div_lo = div & 0xFFu;
    uint32_t div_hi = (div >> 8u) & 0x3u;

    // DATA_TOUNIT = 0xE (max timeout), CLK_INTLEN=1
    ctrl1  = (0xEu << 16) | (div_lo << 8) | (div_hi << 6) | CTRL1_CLK_INTLEN;
    wr32(EMMC_CONTROL1, ctrl1);
    nop_delay(100);

    // Wait for clock to stabilise
    uint32_t retries = 100000u;
    while (!(rd32(EMMC_CONTROL1) & CTRL1_CLK_STABLE) && retries--) {
        nop_delay(4);
    }
    if (!retries) {
        kprintf("[EMMC] clock stable timeout (target %u Hz, div=%u base=%u)\n",
                (unsigned)target_hz, (unsigned)div, (unsigned)base);
        return false;
    }

    // Enable SD clock
    ctrl1 = rd32(EMMC_CONTROL1);
    ctrl1 |= CTRL1_CLK_EN;
    wr32(EMMC_CONTROL1, ctrl1);
    nop_delay(100);

    return true;
}

// ---------------------------------------------------------------------------
// Command issue
// ---------------------------------------------------------------------------

static bool emmc_send_cmd(uint32_t cmdtm, uint32_t arg)
{
    // Wait for CMD line free
    uint32_t retries = 500000u;
    while ((rd32(EMMC_STATUS) & STATUS_CMD_INHIBIT) && retries--) {
        nop_delay(4);
    }
    if (!retries) {
        kprintf("[EMMC] CMD inhibit timeout\n");
        return false;
    }

    // If data command, wait for DAT line free too
    if (cmdtm & CMDTM_ISDATA) {
        retries = 500000u;
        while ((rd32(EMMC_STATUS) & STATUS_DAT_INHIBIT) && retries--) {
            nop_delay(4);
        }
        if (!retries) {
            kprintf("[EMMC] DAT inhibit timeout\n");
            return false;
        }
    }

    emmc_clear_int(0xFFFFFFFFu);

    wr32(EMMC_ARG1,  arg);
    wr32(EMMC_CMDTM, cmdtm);
    dsb();

    uint32_t irq = emmc_wait_int(INT_CMD_DONE);
    if (irq == 0xFFFFFFFFu) {
        kprintf("[EMMC] cmd 0x%08x arg 0x%08x no response (INT=0x%08x)\n",
                (unsigned)cmdtm, (unsigned)arg, (unsigned)rd32(EMMC_INTERRUPT));
        return false;
    }

    emmc_clear_int(INT_CMD_DONE);
    return true;
}

static bool emmc_send_app_cmd(uint32_t cmdtm, uint32_t arg)
{
    if (!emmc_send_cmd(CMDTM_IDX(55u) | CMDTM_RESP_48 | CMDTM_IXCHK_EN | CMDTM_CRCCHK_EN,
                       s_rca << 16)) {
        return false;
    }
    return emmc_send_cmd(cmdtm, arg);
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

bool bcm_emmc_init(void)
{
    s_initialized = false;
    s_is_hc       = false;
    s_rca         = 0;

    // Reset controller
    uint32_t ctrl1 = rd32(EMMC_CONTROL1);
    ctrl1 |= CTRL1_SRST_HC;
    wr32(EMMC_CONTROL1, ctrl1);
    nop_delay(1000);

    uint32_t retries = 100000u;
    while ((rd32(EMMC_CONTROL1) & CTRL1_SRST_HC) && retries--) {
        nop_delay(4);
    }
    if (!retries) {
        kprintf("[EMMC] reset timeout\n");
        return false;
    }

    // Set identification clock (400 kHz)
    if (!emmc_set_clock(400000u)) {
        return false;
    }

    // Enable all interrupts in mask and enable registers
    wr32(EMMC_IRPT_EN,   0xFFFFFFFFu);
    wr32(EMMC_IRPT_MASK, 0xFFFFFFFFu);
    nop_delay(100);

    // CMD0 — go idle
    emmc_send_cmd(CMDTM_IDX(0u), 0u);
    nop_delay(100);

    // CMD8 — send interface condition (3.3V, check pattern 0xAA)
    bool v2_card = false;
    if (emmc_send_cmd(CMDTM_IDX(8u) | CMDTM_RESP_48 | CMDTM_IXCHK_EN | CMDTM_CRCCHK_EN,
                      0x000001AAu)) {
        uint32_t r0 = rd32(EMMC_RESP0);
        v2_card = ((r0 & 0xFFu) == 0xAAu);
    }

    // ACMD41 — SD_SEND_OP_COND, loop until card powered (bit 31 set)
    uint32_t ocr = 0;
    {
        // HCS=1 (SDHC support), XPC=1, FPIC=1, voltage 3.2-3.4V
        uint32_t acmd41_arg = v2_card ? 0x51FF8000u : 0x00FF8000u;
        uint32_t attempts   = 500u;

        do {
            nop_delay(10000u);
            if (!emmc_send_app_cmd(
                        CMDTM_IDX(41u) | CMDTM_RESP_48,   // no CRC/IDX check for ACMD41
                        acmd41_arg)) {
                kprintf("[EMMC] ACMD41 failed\n");
                return false;
            }
            ocr = rd32(EMMC_RESP0);
        } while (!(ocr & (1u << 31)) && --attempts);

        if (!attempts) {
            kprintf("[EMMC] card init timeout (OCR=0x%08x)\n", (unsigned)ocr);
            return false;
        }
    }

    // CCS bit indicates SDHC/SDXC (block addressing)
    s_is_hc = (ocr & (1u << 30)) != 0u;

    // CMD2 — get CID (136-bit response, no check)
    if (!emmc_send_cmd(CMDTM_IDX(2u) | CMDTM_RESP_136, 0u)) {
        kprintf("[EMMC] CMD2 (ALL_SEND_CID) failed\n");
        return false;
    }

    // CMD3 — get RCA
    if (!emmc_send_cmd(CMDTM_IDX(3u) | CMDTM_RESP_48 | CMDTM_IXCHK_EN | CMDTM_CRCCHK_EN, 0u)) {
        kprintf("[EMMC] CMD3 (SEND_RELATIVE_ADDR) failed\n");
        return false;
    }
    s_rca = rd32(EMMC_RESP0) >> 16;

    // Increase clock to 25 MHz
    if (!emmc_set_clock(25000000u)) {
        return false;
    }

    // CMD7 — select card
    if (!emmc_send_cmd(CMDTM_IDX(7u) | CMDTM_RESP_48_BUSY | CMDTM_IXCHK_EN | CMDTM_CRCCHK_EN,
                       s_rca << 16)) {
        kprintf("[EMMC] CMD7 (SELECT_CARD) failed\n");
        return false;
    }

    // CMD16 — set block length to 512
    if (!emmc_send_cmd(CMDTM_IDX(16u) | CMDTM_RESP_48 | CMDTM_IXCHK_EN | CMDTM_CRCCHK_EN,
                       512u)) {
        kprintf("[EMMC] CMD16 (SET_BLOCKLEN) failed\n");
        return false;
    }

    s_initialized = true;
    kprintf("[EMMC] init OK: %s, RCA=0x%04x\n",
            s_is_hc ? "SDHC/SDXC" : "SDSC", (unsigned)s_rca);
    return true;
}

// ---------------------------------------------------------------------------
// Block read
// ---------------------------------------------------------------------------

bool bcm_emmc_read_block(uint32_t lba, void *buf)
{
    if (!s_initialized || !buf) {
        return false;
    }

    // SDSC uses byte addressing; SDHC/SDXC uses block addressing
    uint32_t arg = s_is_hc ? lba : (lba * 512u);

    wr32(EMMC_BLKSIZECNT, (1u << 16) | 512u);  // 1 block, 512 bytes

    uint32_t cmdtm = CMDTM_IDX(17u) | CMDTM_RESP_48 |
                     CMDTM_IXCHK_EN | CMDTM_CRCCHK_EN |
                     CMDTM_ISDATA   | CMDTM_DAT_READ;

    if (!emmc_send_cmd(cmdtm, arg)) {
        return false;
    }

    // Wait for data ready
    uint32_t irq = emmc_wait_int(INT_READ_RDY);
    if (irq == 0xFFFFFFFFu) {
        kprintf("[EMMC] read_rdy timeout (lba=%u)\n", (unsigned)lba);
        return false;
    }
    emmc_clear_int(INT_READ_RDY);

    // Read 128 words (512 bytes) — raw reads, no bswap
    uint32_t *dst = (uint32_t *)buf;
    for (unsigned i = 0; i < 128u; i++) {
        dst[i] = rd32_data(EMMC_DATA);
    }
    dsb();

    // Wait for data transfer complete
    irq = emmc_wait_int(INT_DATA_DONE);
    if (irq == 0xFFFFFFFFu) {
        kprintf("[EMMC] data_done timeout (lba=%u)\n", (unsigned)lba);
        return false;
    }
    emmc_clear_int(INT_DATA_DONE);

    return true;
}

// ---------------------------------------------------------------------------
// Block write (CMD24 — WRITE_SINGLE_BLOCK)
// ---------------------------------------------------------------------------

bool bcm_emmc_write_block(uint32_t lba, const void *buf)
{
    if (!s_initialized || !buf) {
        return false;
    }

    uint32_t arg = s_is_hc ? lba : (lba * 512u);

    wr32(EMMC_BLKSIZECNT, (1u << 16) | 512u);  // 1 block, 512 bytes

    uint32_t cmdtm = CMDTM_IDX(24u) | CMDTM_RESP_48 |
                     CMDTM_IXCHK_EN | CMDTM_CRCCHK_EN |
                     CMDTM_ISDATA;              // direction: host → card

    if (!emmc_send_cmd(cmdtm, arg)) {
        return false;
    }

    uint32_t irq = emmc_wait_int(INT_WRITE_RDY);
    if (irq == 0xFFFFFFFFu) {
        kprintf("[EMMC] write_rdy timeout (lba=%u)\n", (unsigned)lba);
        return false;
    }
    emmc_clear_int(INT_WRITE_RDY);

    // Write 128 words (512 bytes) — raw writes, no bswap (mirrors read path)
    const uint32_t *src = (const uint32_t *)buf;
    for (unsigned i = 0; i < 128u; i++) {
        *(volatile uint32_t *)EMMC_DATA = src[i];
    }
    dsb();

    irq = emmc_wait_int(INT_DATA_DONE);
    if (irq == 0xFFFFFFFFu) {
        kprintf("[EMMC] write data_done timeout (lba=%u)\n", (unsigned)lba);
        return false;
    }
    emmc_clear_int(INT_DATA_DONE);

    return true;
}

bool bcm_emmc_read_blocks(uint32_t lba, void *buf, uint32_t nsectors)
{
    uint8_t *p = (uint8_t *)buf;

    for (uint32_t i = 0; i < nsectors; i++) {
        if (!bcm_emmc_read_block(lba + i, p)) {
            return false;
        }
        p += 512u;
    }

    return true;
}
