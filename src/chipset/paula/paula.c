// src/chipset/paula/paula.c
//
// Paula — INTREQ/INTENA, UART (SERDAT/SERPER/SERDATR), disk DMA.

#include "paula.h"
#include "support.h"

#include <stdio.h>
#include <string.h>

/* Custom chip register addresses */
#define REG_SERDATR  0xDFF018u
#define REG_ADKCONR  0xDFF010u
#define REG_DSKBYTR  0xDFF01Au
#define REG_INTENAR  0xDFF01Cu
#define REG_INTREQR  0xDFF01Eu
#define REG_DSKPTH   0xDFF020u
#define REG_DSKPTL   0xDFF022u
#define REG_DSKLEN   0xDFF024u
#define REG_SERDAT   0xDFF030u
#define REG_SERPER   0xDFF032u
#define REG_DSKSYNC  0xDFF07Eu
#define REG_INTENA   0xDFF09Au
#define REG_INTREQ   0xDFF09Cu
#define REG_ADKCON   0xDFF09Eu
#define REG_POTGOR   0xDFF016u

/* ---------------------------------------------------------------------------
 * UART callbacks
 * ------------------------------------------------------------------------- */

static void uart_tx_cb(void *opaque, uint8_t byte)
{
    (void)opaque;

    static char buf[256];
    static int pos = 0;

    if (byte == '\0') return;

    /* filtra lixo */
    if (byte < 32 && byte != '\n' && byte != '\r') {
        return;
    }

    if (byte == '\n' || byte == '\r' || pos >= (int)(sizeof(buf) - 1)) {
        buf[pos] = '\0';
        if (pos > 0) {
            kprintf("[SERIAL] %s\n", buf);
        }
        pos = 0;
        return;
    }

    buf[pos++] = (char)byte;
}

static void uart_irq_cb(void *opaque, uint16_t mask)
{
    Paula *p = (Paula *)opaque;
    paula_irq_raise(p, mask);
}

/* ---------------------------------------------------------------------------
 * PaulaDisk INTREQ callback
 * ------------------------------------------------------------------------- */

static void disk_intreq_cb(void *opaque, uint16_t bits)
{
    Paula *p = (Paula *)opaque;
    paula_irq_raise(p, bits);
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void paula_init(Paula *p)
{
    memset(p, 0, sizeof(*p));
    uart_init(&p->uart, p, uart_tx_cb, uart_irq_cb);
    paula_disk_init(&p->disk);
    paula_disk_set_intreq_callback(&p->disk, disk_intreq_cb, p);
}

void paula_reset(Paula *p)
{
    UARTState saved_uart = p->uart;
    PaulaDisk saved_disk  = p->disk;

    p->intreq = 0;
    p->intena  = 0;
    p->ipl     = 0;

    uart_reset(&p->uart);
    /* restore wiring */
    p->uart.opaque        = saved_uart.opaque;
    p->uart.tx_cb         = saved_uart.tx_cb;
    p->uart.irq_raise_cb  = saved_uart.irq_raise_cb;

    paula_disk_init(&p->disk);
    /* restore wiring */
    p->disk.chipram       = saved_disk.chipram;
    p->disk.chipram_size  = saved_disk.chipram_size;
    p->disk.drive         = saved_disk.drive;
    p->disk.intreq_cb     = saved_disk.intreq_cb;
    p->disk.intreq_user   = saved_disk.intreq_user;
}

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */

void paula_attach_agnus(Paula *p, struct AgnusState *agnus)
{
    (void)p;
    (void)agnus;
}

void paula_attach_cia_a(Paula *p, struct CIA_State *cia)
{
    (void)p;
    (void)cia;
}

void paula_attach_cia_b(Paula *p, struct CIA_State *cia)
{
    (void)p;
    (void)cia;
}

void paula_attach_memory(Paula *p, uint8_t *chipram, size_t size)
{
    paula_disk_attach_memory(&p->disk, chipram, size);
}

void paula_attach_drive(Paula *p, FloppyDrive *drive)
{
    paula_disk_attach_drive(&p->disk, drive);
}

/* ---------------------------------------------------------------------------
 * IRQ interface
 * ------------------------------------------------------------------------- */

void paula_irq_raise(Paula *p, uint16_t bits)
{
    p->intreq |= (uint16_t)(bits & 0x3FFFu);
}

void paula_irq_clear(Paula *p, uint16_t bits)
{
    p->intreq &= (uint16_t)~(bits & 0x3FFFu);
}

/* ---------------------------------------------------------------------------
 * IPL derivation — matches Amiga HRM interrupt priority table
 *
 * Level 6: EXTER (CIA-B)
 * Level 5: DSKSYN, RBF
 * Level 4: AUD0..AUD3
 * Level 3: COPER, VERTB, BLIT
 * Level 2: PORTS (CIA-A)
 * Level 1: TBE, DSKBLK, SOFT
 * ------------------------------------------------------------------------- */

uint8_t paula_compute_ipl(const Paula *p)
{
    int master = !!(p->intena & PAULA_INT_MASTER);
    uint16_t pending = (uint16_t)(p->intena & p->intreq & 0x3FFFu);

    if (!master || !pending)
        return 0;

    if (pending & PAULA_INT_EXTER)                                               return 6;
    if (pending & (PAULA_INT_DSKSYN | PAULA_INT_RBF))                            return 5;
    if (pending & (PAULA_INT_AUD0|PAULA_INT_AUD1|PAULA_INT_AUD2|PAULA_INT_AUD3)) return 4;
    if (pending & (PAULA_INT_COPER|PAULA_INT_VERTB|PAULA_INT_BLIT))              return 3;
    if (pending & PAULA_INT_PORTS)                                               return 2;
    if (pending & (PAULA_INT_TBE|PAULA_INT_DSKBLK|PAULA_INT_SOFT))              return 1;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Step
 * ------------------------------------------------------------------------- */

void paula_step(Paula *p, uint32_t ticks)
{
    uart_step(&p->uart, ticks);
    paula_disk_step(&p->disk, ticks);
}

/* ---------------------------------------------------------------------------
 * Bus protocol
 * ------------------------------------------------------------------------- */

int paula_handles_read(const Paula *p, uint32_t addr)
{
    (void)p;
    return addr == REG_SERDATR
        || addr == REG_ADKCONR
        || addr == REG_DSKBYTR
        || addr == REG_INTENAR
        || addr == REG_INTREQR
        || addr == REG_POTGOR;
}

int paula_handles_write(const Paula *p, uint32_t addr)
{
    (void)p;
    return addr == REG_DSKPTH
        || addr == REG_DSKPTL
        || addr == REG_DSKLEN
        || addr == REG_SERDAT
        || addr == REG_SERPER
        || addr == REG_DSKSYNC
        || addr == REG_INTENA
        || addr == REG_INTREQ
        || addr == REG_ADKCON;
}

uint32_t paula_read(Paula *p, uint32_t addr, unsigned int size)
{
    (void)size;
    uint32_t ret = 0;
    switch (addr) {
    case REG_SERDATR:
        ret = uart_read_serdatr(&p->uart);
        break;
    case REG_ADKCONR:
        ret = p->disk.adkcon;
        break;
    case REG_POTGOR:
        /* All buttons up, all lines pulled high */
        ret = 0xFFFFu;
        break;
    case REG_DSKBYTR:
        ret = paula_disk_read_dskbytr(&p->disk);
        break;
    case REG_INTENAR:
        ret = p->intena;
        kprintf("[PAULA-R] INTENAR -> %04x  (intreq=%04x)\n",
                (unsigned)p->intena, (unsigned)p->intreq);
        break;
    case REG_INTREQR:
        ret = p->intreq;
        kprintf("[PAULA-R] INTREQR -> %04x  (intena=%04x)\n",
                (unsigned)p->intreq, (unsigned)p->intena);
        break;
    default:
        break;
    }
    return ret;
}

void paula_write(Paula *p, uint32_t addr, uint32_t value, unsigned int size)
{
    (void)size;
    uint16_t raw = (uint16_t)value;

    switch (addr) {
    case REG_DSKPTH:
        paula_disk_write_dskpth(&p->disk, raw);
        break;

    case REG_DSKPTL:
        paula_disk_write_dskptl(&p->disk, raw);
        break;

    case REG_DSKLEN:
        paula_disk_write_dsklen(&p->disk, raw);
        break;

    case REG_DSKSYNC:
        paula_disk_write_dsksync(&p->disk, raw);
        kprintf("[PAULA-W] DSKSYNC=%04x\n", (unsigned)raw);
        break;

    case REG_SERDAT:
        uart_write_serdat(&p->uart, raw);
        break;

    case REG_SERPER:
        uart_write_serper(&p->uart, raw);
        break;

    case REG_ADKCON:
        paula_disk_write_adkcon(&p->disk, raw);
        break;

    case REG_INTENA:
        if (raw & 0x8000u)
            p->intena |= (uint16_t)(raw & 0x7FFFu);
        else
            p->intena &= (uint16_t)~(raw & 0x7FFFu);
        {
            int inten   = !!(p->intena & PAULA_INT_MASTER);
            uint16_t pd = (uint16_t)(p->intena & p->intreq & 0x3FFFu);
            kprintf("[PAULA-W] INTENA raw=%04x -> intena=%04x intreq=%04x pending=%04x%s\n",
                    (unsigned)raw, (unsigned)p->intena,
                    (unsigned)p->intreq, (unsigned)pd,
                    inten ? "" : " (INTEN OFF)");
        }
        break;

    case REG_INTREQ:
        if (raw & 0x8000u)
            p->intreq |= (uint16_t)(raw & 0x3FFFu);
        else
            p->intreq &= (uint16_t)~(raw & 0x3FFFu);
        {
            int inten   = !!(p->intena & PAULA_INT_MASTER);
            uint16_t pd = (uint16_t)(p->intena & p->intreq & 0x3FFFu);
            kprintf("[PAULA-W] INTREQ raw=%04x -> intreq=%04x intena=%04x pending=%04x%s\n",
                    (unsigned)raw, (unsigned)p->intreq,
                    (unsigned)p->intena, (unsigned)pd,
                    inten ? "" : " (INTEN OFF)");
        }
        break;

    default:
        break;
    }
}
