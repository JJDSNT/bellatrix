// src/chipset/paula/paula.c
//
// Paula — INTREQ/INTENA, UART (SERDAT/SERPER/SERDATR), disk DMA.

#include "paula.h"
#include "debug/cpu_pc.h"
#include "support.h"

#include <stdio.h>
#include <string.h>

/* Custom chip register addresses */
#define REG_SERDATR 0xDFF018u
#define REG_ADKCONR 0xDFF010u
#define REG_JOY0DAT 0xDFF00Au
#define REG_JOY1DAT 0xDFF00Cu
#define REG_DSKDATR 0xDFF008u
#define REG_POT0DAT 0xDFF012u
#define REG_POT1DAT 0xDFF014u
#define REG_DSKBYTR 0xDFF01Au
#define REG_INTENAR 0xDFF01Cu
#define REG_INTREQR 0xDFF01Eu
#define REG_DSKPTH 0xDFF020u
#define REG_DSKPTL 0xDFF022u
#define REG_DSKLEN 0xDFF024u
#define REG_SERDAT 0xDFF030u
#define REG_SERPER 0xDFF032u
#define REG_POTGO  0xDFF034u
#define REG_DSKSYNC 0xDFF07Eu
#define REG_INTENA 0xDFF09Au
#define REG_INTREQ 0xDFF09Cu
#define REG_ADKCON 0xDFF09Eu
#define REG_POTGOR 0xDFF016u
#define REG_AUD0LCH 0xDFF0A0u
#define REG_AUD0LCL 0xDFF0A2u
#define REG_AUD0LEN 0xDFF0A4u
#define REG_AUD0PER 0xDFF0A6u
#define REG_AUD0VOL 0xDFF0A8u
#define REG_AUD0DAT 0xDFF0AAu
#define REG_AUD3DAT 0xDFF0DAu

/* ---------------------------------------------------------------------------
 * Serial / IRQ callbacks
 * ------------------------------------------------------------------------- */

static void serial_irq_cb(void *opaque, uint16_t mask)
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

static void audio_intreq_cb(void *opaque, uint16_t bits)
{
    Paula *p = (Paula *)opaque;
    paula_irq_raise(p, bits);
}

static int paula_is_audio_reg(uint32_t addr)
{
    if (addr < REG_AUD0LCH || addr > REG_AUD3DAT)
        return 0;

    return ((addr - REG_AUD0LCH) & 0x0Fu) <= 0x0Au;
}

static void paula_write_audio_reg(Paula *p, uint32_t addr, uint16_t raw)
{
    uint32_t slot = addr - REG_AUD0LCH;
    int channel = (int)(slot >> 4);
    uint32_t reg = slot & 0x0Fu;

    switch (reg)
    {
    case 0x0u:
        paula_audio_write_lch(&p->audio, channel, raw);
        kprintf("[AUD%d] LCH=%04x  lc=0x%06x\n",
                channel, (unsigned)raw,
                (unsigned)p->audio.ch[channel].audlc);
        break;
    case 0x2u:
        paula_audio_write_lcl(&p->audio, channel, raw);
        kprintf("[AUD%d] LCL=%04x  lc=0x%06x\n",
                channel, (unsigned)raw,
                (unsigned)p->audio.ch[channel].audlc);
        break;
    case 0x4u:
        paula_audio_write_len(&p->audio, channel, raw);
        kprintf("[AUD%d] LEN=%u words\n",
                channel, (unsigned)p->audio.ch[channel].audlen);
        break;
    case 0x6u:
        paula_audio_write_per(&p->audio, channel, raw);
        kprintf("[AUD%d] PER=%u\n",
                channel, (unsigned)p->audio.ch[channel].audper);
        break;
    case 0x8u:
        paula_audio_write_vol(&p->audio, channel, raw);
        kprintf("[AUD%d] VOL=%u\n",
                channel, (unsigned)p->audio.ch[channel].audvol);
        break;
    case 0xAu:
        paula_audio_write_dat(&p->audio, channel, raw);
        kprintf("[AUD%d] DAT=%04x\n", channel, (unsigned)raw);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void paula_init(Paula *p)
{
    memset(p, 0, sizeof(*p));
    paula_interrupt_init(&p->irq);
    paula_input_init(&p->input);
    paula_serial_init(&p->serial, p, serial_irq_cb);
    paula_disk_init(&p->disk);
    paula_disk_set_intreq_callback(&p->disk, disk_intreq_cb, p);
    paula_audio_init(&p->audio);
    p->audio.irq_opaque = p;
    p->audio.irq_raise_cb = audio_intreq_cb;
}

void paula_reset(Paula *p)
{
    PaulaSerial saved_serial = p->serial;
    PaulaDisk saved_disk = p->disk;
    PaulaAudio saved_audio = p->audio;

    paula_interrupt_reset(&p->irq);
    p->irq_line_level = 0;
    paula_input_reset(&p->input);

    paula_serial_reset(&p->serial);
    /* restore wiring */
    p->serial.opaque = saved_serial.opaque;
    p->serial.irq_raise_cb = saved_serial.irq_raise_cb;
    p->serial.tx_instant = saved_serial.tx_instant;

    paula_disk_init(&p->disk);
    /* restore wiring */
    p->disk.chipram = saved_disk.chipram;
    p->disk.chipram_size = saved_disk.chipram_size;
    p->disk.drive = saved_disk.drive;
    p->disk.intreq_cb = saved_disk.intreq_cb;
    p->disk.intreq_user = saved_disk.intreq_user;

    paula_audio_reset(&p->audio);
    p->audio.irq_opaque = saved_audio.irq_opaque;
    p->audio.irq_raise_cb = saved_audio.irq_raise_cb;
}

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */

void paula_attach_memory(Paula *p, uint8_t *chipram, size_t size)
{
    paula_disk_attach_memory(&p->disk, chipram, size);
}

void paula_attach_drive(Paula *p, FloppyDrive *drive)
{
    paula_disk_attach_drive(&p->disk, drive);
}

void paula_set_joydat(Paula *p, unsigned port, uint16_t value)
{
    paula_input_set_joydat(&p->input, port, value);
}

void paula_set_pot_button_x(Paula *p, unsigned port, int pressed)
{
    paula_input_set_pot_button_x(&p->input, port, pressed);
}

void paula_set_pot_button_y(Paula *p, unsigned port, int pressed)
{
    paula_input_set_pot_button_y(&p->input, port, pressed);
}

/* ---------------------------------------------------------------------------
 * IRQ interface
 * ------------------------------------------------------------------------- */

void paula_irq_raise(Paula *p, uint16_t bits)
{
    uint16_t b = (uint16_t)(bits & 0x3FFFu);

    paula_interrupt_raise(&p->irq, b);

    /*
     * Only external interrupt input lines are level-like here.
     * VBL, disk, blitter, copper, audio, etc. are normal INTREQ latch bits.
     */
    p->irq_line_level |= (uint16_t)(b & (PAULA_INT_PORTS | PAULA_INT_EXTER));
}

void paula_irq_clear(Paula *p, uint16_t bits)
{
    uint16_t b = (uint16_t)(bits & 0x3FFFu);

    p->irq_line_level &= (uint16_t)~(b & (PAULA_INT_PORTS | PAULA_INT_EXTER));
    paula_interrupt_clear(&p->irq, b);
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
    return paula_interrupt_current_ipl(&p->irq);
}

/* ---------------------------------------------------------------------------
 * Step
 * ------------------------------------------------------------------------- */

void paula_step(Paula *p, uint32_t ticks)
{
    paula_disk_step(&p->disk, ticks);
    paula_audio_step(&p->audio, ticks);
    paula_serial_step(&p->serial, ticks);
}

/* ---------------------------------------------------------------------------
 * Bus protocol
 * ------------------------------------------------------------------------- */

int paula_handles_read(const Paula *p, uint32_t addr)
{
    (void)p;
    return addr == REG_DSKDATR || addr == REG_JOY0DAT ||
           addr == REG_JOY1DAT || addr == REG_POT0DAT ||
           addr == REG_POT1DAT || addr == REG_SERDATR ||
           addr == REG_ADKCONR ||
           addr == REG_DSKBYTR || addr == REG_INTENAR ||
           addr == REG_INTREQR || addr == REG_POTGOR;
}

int paula_handles_write(const Paula *p, uint32_t addr)
{
    (void)p;
    return addr == REG_DSKPTH || addr == REG_DSKPTL || addr == REG_DSKLEN ||
           addr == REG_SERDAT || addr == REG_SERPER || addr == REG_POTGO ||
           addr == REG_DSKSYNC || addr == REG_INTENA || addr == REG_INTREQ ||
           addr == REG_ADKCON || paula_is_audio_reg(addr);
}

uint32_t paula_read(Paula *p, uint32_t addr, unsigned int size)
{
    uint32_t ret = 0;
    switch (addr)
    {
    case REG_DSKDATR:
        ret = paula_disk_read_dskdatr(&p->disk);
        break;
    case REG_JOY0DAT:
        ret = paula_input_read_joydat(&p->input, 0u);
        break;
    case REG_JOY1DAT:
        ret = paula_input_read_joydat(&p->input, 1u);
        break;
    case REG_POT0DAT:
        ret = paula_input_read_potdat(&p->input, 0u);
        break;
    case REG_POT1DAT:
        ret = paula_input_read_potdat(&p->input, 1u);
        break;
    case REG_SERDATR:
        ret = paula_serial_read_serdatr(&p->serial);
        {
            static int s_first_serdatr_logged = 0;
            if (!s_first_serdatr_logged) {
                s_first_serdatr_logged = 1;
                kprintf("[SERIAL-RX] first SERDATR=%04x TBE=%u TSRE=%u RBF=%u\n",
                        (unsigned)ret,
                        (unsigned)((ret & 0x2000u) ? 1u : 0u),
                        (unsigned)((ret & 0x1000u) ? 1u : 0u),
                        (unsigned)((ret & 0x4000u) ? 1u : 0u));
            }
        }
#ifdef BELLATRIX_HARNESS
        if (p->serial.rx_buffer_full) {
            kprintf("[UART-RX] pc=%08x read SERDATR=%04x byte=%02x\n",
                    (unsigned)bellatrix_debug_cpu_pc(),
                    (unsigned)ret,
                    (unsigned)(ret & 0xFFu));
        }
#endif
        paula_serial_clear_rbf(&p->serial);
        break;
    case REG_ADKCONR:
        ret = p->disk.adkcon;
        break;
    case REG_POTGOR:
        ret = paula_input_read_potgor(&p->input);
        break;
    case REG_DSKBYTR:
        ret = paula_disk_read_dskbytr(&p->disk);
        break;
    case REG_INTENAR:
        ret = paula_interrupt_read_intena(&p->irq);
        kprintf("[PAULA-R] INTENAR -> %04x  (intreq=%04x)\n",
                (unsigned)ret,
                (unsigned)paula_interrupt_read_intreq(&p->irq));
        break;
    case REG_INTREQR:
        ret = paula_interrupt_read_intreq(&p->irq);
        kprintf("[PAULA-R] INTREQR -> %04x  (intena=%04x)\n",
                (unsigned)ret,
                (unsigned)paula_interrupt_read_intena(&p->irq));
        break;
    default:
        break;
    }
    if (size == 1)
        return (addr & 1u) ? (ret & 0xFFu) : ((ret >> 8) & 0xFFu);
    return ret;
}

void paula_write(Paula *p, uint32_t addr, uint32_t value, unsigned int size)
{
    (void)size;
    uint16_t raw = (uint16_t)value;

    switch (addr)
    {
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
    {
        static int s_first_serdat_logged = 0;
        if (!s_first_serdat_logged) {
            s_first_serdat_logged = 1;
            kprintf("[SERIAL-TX] first SERDAT=%04x byte=%02x\n",
                    (unsigned)raw,
                    (unsigned)(raw & 0xFFu));
        }
        paula_serial_write_serdat(&p->serial, raw);
        break;
    }

    case REG_SERPER:
    {
        static int s_first_serper_logged = 0;
        if (!s_first_serper_logged) {
            s_first_serper_logged = 1;
            kprintf("[SERIAL-CFG] first SERPER=%04x\n", (unsigned)raw);
        }
        paula_serial_write_serper(&p->serial, raw);
        break;
    }

    case REG_POTGO:
        paula_input_write_potgo(&p->input, raw);
        break;

    case REG_ADKCON:
        paula_disk_write_adkcon(&p->disk, raw);
        break;

    case REG_INTENA:
        paula_interrupt_write_intena(&p->irq, raw);
        {
            uint16_t intena = paula_interrupt_read_intena(&p->irq);
            uint16_t intreq = paula_interrupt_read_intreq(&p->irq);
            int inten = !!(intena & PAULA_INT_MASTER);
            uint16_t pd = (uint16_t)(intena & intreq & 0x3FFFu);
            kprintf("[PAULA-W] INTENA raw=%04x -> intena=%04x intreq=%04x pending=%04x%s\n",
                    (unsigned)raw, (unsigned)intena,
                    (unsigned)intreq, (unsigned)pd,
                    inten ? "" : " (INTEN OFF)");
        }
        break;

    case REG_INTREQ:
    {
        paula_interrupt_write_intreq(&p->irq, raw);

        if (!(raw & 0x8000u) && p->irq_line_level != 0u) {
            /*
             * Reassert level-triggered sources after ACK.
             */
            paula_interrupt_raise(&p->irq, p->irq_line_level);
        }

        {
        uint16_t intena = paula_interrupt_read_intena(&p->irq);
        uint16_t intreq = paula_interrupt_read_intreq(&p->irq);
        int inten = !!(intena & PAULA_INT_MASTER);
        uint16_t pd = (uint16_t)(intena & intreq & 0x3FFFu);

        kprintf("[PAULA-W] INTREQ raw=%04x -> intreq=%04x intena=%04x pending=%04x line=%04x%s\n",
                (unsigned)raw,
                (unsigned)intreq,
                (unsigned)intena,
                (unsigned)pd,
                (unsigned)p->irq_line_level,
                inten ? "" : " (INTEN OFF)");
        }
    }
    break;

    default:
        if (paula_is_audio_reg(addr))
        {
            paula_write_audio_reg(p, addr, raw);
            break;
        }
        break;
    }
}
