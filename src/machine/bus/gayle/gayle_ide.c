// src/machine/bus/gayle/gayle_ide.c

#include "machine/bus/gayle/gayle_ide.h"
#include "support.h"

#include <string.h>

static void set_status(GayleIde *ide, uint8_t status)
{
    ide->status = status;
}

static void set_error(GayleIde *ide, uint8_t error)
{
    ide->error = error;
    if (error)
        ide->status |= ATA_STATUS_ERR;
    else
        ide->status &= ~ATA_STATUS_ERR;
}

static void write_identify_word(uint8_t *buf, unsigned word, uint16_t value)
{
    /*
     * m68k reads IDENTIFY words with move.w (big-endian) then AROS does
     * SWAP_LE_WORD.  We store big-endian here so that after read16 and
     * the AROS swap the host sees the intended little-endian value.
     */
    buf[word * 2 + 0] = (uint8_t)((value >> 8) & 0xff);
    buf[word * 2 + 1] = (uint8_t)(value & 0xff);
}

static void write_identify_string(uint8_t *buf, unsigned word, unsigned words, const char *text)
{
    char tmp[64];

    memset(tmp, ' ', sizeof(tmp));

    size_t max = words * 2;
    size_t len = strlen(text);
    if (len > max)
        len = max;

    memcpy(tmp, text, len);

    for (unsigned i = 0; i < words; i++) {
        buf[(word + i) * 2 + 0] = (uint8_t)tmp[i * 2 + 1];
        buf[(word + i) * 2 + 1] = (uint8_t)tmp[i * 2 + 0];
    }
}

static void build_identify_packet(GayleIde *ide)
{
    memset(ide->identify, 0, sizeof(ide->identify));

    /*
     * ATAPI removable CD-ROM.
     *
     * Word 0:
     *  bit 15 = 1  non-ATA device
     *  bits 12-8 = 5 CD-ROM
     *  bit 7 = 1 removable
     */
    write_identify_word(ide->identify, 0, 0x8580);

    write_identify_string(ide->identify, 10, 10, "BELLATRIX0001");
    write_identify_string(ide->identify, 23, 4,  "0.1");
    write_identify_string(ide->identify, 27, 20, "Bellatrix Virtual ATAPI CD-ROM");

    /*
     * Capabilities.
     * Keep this conservative: PIO only, no DMA requirement.
     */
    write_identify_word(ide->identify, 49, 0x0200);
    write_identify_word(ide->identify, 51, 0x0200);
    write_identify_word(ide->identify, 52, 0x0000);

    /*
     * Major/minor version loosely ATA/ATAPI-4-ish.
     */
    write_identify_word(ide->identify, 80, 0x001e);
    write_identify_word(ide->identify, 81, 0x001c);

    /*
     * Command sets: packet feature present enough for old code.
     */
    write_identify_word(ide->identify, 82, 0x4000);
    write_identify_word(ide->identify, 83, 0x0000);
    write_identify_word(ide->identify, 84, 0x0000);
}

static void begin_identify(GayleIde *ide)
{
    build_identify_packet(ide);
    ide->identify_pos = 0;
    ide->identify_active = true;
    set_error(ide, 0);
    set_status(ide, ATA_STATUS_DRDY | ATA_STATUS_DRQ);
}

static void begin_packet(GayleIde *ide)
{
    /* Host writes BCL into cyl_hi:cyl_lo before issuing PACKET command. */
    uint16_t bcl = ((uint16_t)ide->cyl_high << 8) | ide->cyl_low;
    bcl &= ~1u; /* ATAPI: BCL must be even */
    if (bcl == 0)
        bcl = 0xfffe;
    ide->byte_count_limit = bcl;

    ide->identify_active = false;

    atapi_cd_begin_packet(&ide->cd);

    set_error(ide, 0);
    set_status(ide, ATA_STATUS_DRDY | ATA_STATUS_DRQ);

    /*
     * ATAPI Interrupt Reason: C/D=1, I/O=0 → device expects CDB.
     * cyl_low/cyl_high will be updated to the actual DRQ block size
     * after the CDB is received and the command dispatched.
     */
    ide->sector_count = 0x01;
    ide->cyl_low  = 0x00;
    ide->cyl_high = 0x00;
    ide->drq_block_end = 0;
}

static void device_reset(GayleIde *ide)
{
    gayle_ide_reset(ide);
}

void gayle_ide_init(
    GayleIde *ide,
    IsoImage *iso,
    uint8_t *atapi_buffer,
    size_t atapi_buffer_size
)
{
    if (!ide)
        return;

    memset(ide, 0, sizeof(*ide));
    atapi_cd_init(&ide->cd, iso, atapi_buffer, atapi_buffer_size);
    gayle_ide_reset(ide);
}

void gayle_ide_reset(GayleIde *ide)
{
    if (!ide)
        return;

    ide->status = ATA_STATUS_DRDY;
    ide->error = 0;
    ide->features = 0;
    ide->sector_count = 1;
    ide->sector_number = 1;

    /*
     * ATAPI signature after reset:
     * sector count = 1
     * sector number = 1
     * cyl low = 0x14
     * cyl high = 0xeb
     */
    ide->cyl_low = 0x14;
    ide->cyl_high = 0xeb;
    ide->dev_head = 0xa0;

    ide->identify_pos = 0;
    ide->identify_active = false;

    ide->byte_count_limit = 0xfffe;
    ide->drq_block_end = 0;

    atapi_cd_reset(&ide->cd);
}

uint8_t gayle_ide_read8(GayleIde *ide, uint32_t reg)
{
    if (!ide)
        return 0xff;

    /* Only device 0 is present. Return "no device" values when device 1
     * is selected so AROS does not create a spurious slave unit. */
    if (ide->dev_head & 0x10u) {
        if (reg == GAYLE_IDE_REG_STATUS) return 0x00u;
        if (reg == GAYLE_IDE_REG_ERROR)  return 0x01u;
        return 0x00u;
    }

    uint8_t val;
    switch (reg) {
        case GAYLE_IDE_REG_ERROR:
            val = ide->error;
            kprintf("[GAYLE-IDE-R] ERROR=%02x\n", (unsigned)val);
            return val;

        case GAYLE_IDE_REG_SECCNT:
            val = ide->sector_count;
            kprintf("[GAYLE-IDE-R] SECCNT=%02x\n", (unsigned)val);
            return val;

        case GAYLE_IDE_REG_SECNUM:
            val = ide->sector_number;
            kprintf("[GAYLE-IDE-R] SECNUM=%02x\n", (unsigned)val);
            return val;

        case GAYLE_IDE_REG_CYLLO:
            val = ide->cyl_low;
            kprintf("[GAYLE-IDE-R] CYLLO=%02x\n", (unsigned)val);
            return val;

        case GAYLE_IDE_REG_CYLHI:
            val = ide->cyl_high;
            kprintf("[GAYLE-IDE-R] CYLHI=%02x\n", (unsigned)val);
            return val;

        case GAYLE_IDE_REG_DEVHEAD:
            val = ide->dev_head;
            kprintf("[GAYLE-IDE-R] DEVHEAD=%02x\n", (unsigned)val);
            return val;

        case GAYLE_IDE_REG_STATUS: {
            uint8_t st;
            if (ide->cd.phase == ATAPI_PHASE_DATA_IN)
                st = ATA_STATUS_DRDY | ATA_STATUS_DRQ;
            else if (ide->cd.phase == ATAPI_PHASE_PACKET_IN)
                st = ATA_STATUS_DRDY | ATA_STATUS_DRQ;
            else
                st = ide->status;
            kprintf("[GAYLE-IDE-R] STATUS=%02x phase=%d\n",
                    (unsigned)st, (int)ide->cd.phase);
            return st;
        }

        default:
            return 0xff;
    }
}

void gayle_ide_write8(GayleIde *ide, uint32_t reg, uint8_t value)
{
    if (!ide)
        return;

    switch (reg) {
        case GAYLE_IDE_REG_FEATURES:
            kprintf("[GAYLE-IDE-W] FEATURES=%02x\n", (unsigned)value);
            ide->features = value;
            break;

        case GAYLE_IDE_REG_SECCNT:
            kprintf("[GAYLE-IDE-W] SECCNT=%02x\n", (unsigned)value);
            ide->sector_count = value;
            break;

        case GAYLE_IDE_REG_SECNUM:
            kprintf("[GAYLE-IDE-W] SECNUM=%02x\n", (unsigned)value);
            ide->sector_number = value;
            break;

        case GAYLE_IDE_REG_CYLLO:
            kprintf("[GAYLE-IDE-W] CYLLO=%02x\n", (unsigned)value);
            ide->cyl_low = value;
            break;

        case GAYLE_IDE_REG_CYLHI:
            kprintf("[GAYLE-IDE-W] CYLHI=%02x\n", (unsigned)value);
            ide->cyl_high = value;
            break;

        case GAYLE_IDE_REG_DEVHEAD:
            kprintf("[GAYLE-IDE-W] DEVHEAD=%02x\n", (unsigned)value);
            ide->dev_head = value;
            break;

        case GAYLE_IDE_REG_COMMAND:
            kprintf("[GAYLE-IDE-W] CMD=%02x\n", (unsigned)value);
            switch (value) {
                case ATA_CMD_DEVICE_RESET:
                    device_reset(ide);
                    break;

                case ATA_CMD_EXECUTE_DEVICE_DIAG:
                    /* ATA spec §9.10: restore ATAPI signature regardless of
                     * what the host wrote to the cylinder registers before
                     * issuing this command. */
                    ide->cyl_low  = 0x14;
                    ide->cyl_high = 0xEB;
                    ide->sector_count  = 0x01;
                    ide->sector_number = 0x01;
                    set_error(ide, 0x01);
                    set_status(ide, ATA_STATUS_DRDY);
                    break;

                case ATA_CMD_IDENTIFY_PACKET_DEVICE:
                    begin_identify(ide);
                    break;

                case ATA_CMD_PACKET:
                    begin_packet(ide);
                    break;

                case ATA_CMD_IDENTIFY_DEVICE:
                    /*
                     * We are not a normal ATA disk.
                     * Signal error so the driver can try IDENTIFY PACKET.
                     */
                    set_error(ide, 0x04); /* ABRT */
                    set_status(ide, ATA_STATUS_DRDY | ATA_STATUS_ERR);
                    break;

                default:
                    set_error(ide, 0x04); /* ABRT */
                    set_status(ide, ATA_STATUS_DRDY | ATA_STATUS_ERR);
                    break;
            }
            break;

        default:
            break;
    }
}

uint16_t gayle_ide_read16(GayleIde *ide)
{
    if (!ide)
        return 0xffff;

    if (ide->identify_active) {
        if (ide->identify_pos >= sizeof(ide->identify)) {
            ide->identify_active = false;
            set_status(ide, ATA_STATUS_DRDY);
            return 0xffff;
        }

        uint16_t v =
            ((uint16_t)ide->identify[ide->identify_pos + 1] << 8) |
            ((uint16_t)ide->identify[ide->identify_pos + 0]);

        ide->identify_pos += 2;

        if (ide->identify_pos >= sizeof(ide->identify)) {
            ide->identify_active = false;
            set_status(ide, ATA_STATUS_DRDY);
        }

        return v;
    }

    if (ide->cd.phase == ATAPI_PHASE_DATA_IN) {
        uint16_t v = atapi_cd_read_data_word(&ide->cd);

        if (!atapi_cd_has_data(&ide->cd)) {
            /* All data consumed — command complete. */
            ide->sector_count = 0x03; /* I/O=1, C/D=1 */
            set_status(ide, ATA_STATUS_DRDY);
            ide->irq_pending = true;
        } else if (ide->cd.data_pos >= ide->drq_block_end) {
            /* DRQ block exhausted; more data remains — set up next block. */
            size_t remaining = ide->cd.data_len - ide->cd.data_pos;
            size_t next = remaining < (size_t)ide->byte_count_limit
                          ? remaining
                          : (size_t)ide->byte_count_limit;
            ide->drq_block_end = ide->cd.data_pos + next;
            uint16_t bc = (uint16_t)next;
            ide->sector_count = 0x02;
            ide->cyl_low  = (uint8_t)(bc & 0xff);
            ide->cyl_high = (uint8_t)((bc >> 8) & 0xff);
            set_status(ide, ATA_STATUS_DRDY | ATA_STATUS_DRQ);
            ide->irq_pending = true;
        }

        return v;
    }

    return 0xffff;
}

void gayle_ide_write16(GayleIde *ide, uint16_t value)
{
    if (!ide)
        return;

    if (ide->cd.phase != ATAPI_PHASE_PACKET_IN)
        return;

    atapi_cd_write_packet_word(&ide->cd, value);

    /* Only update status registers once the full 12-byte CDB has been
     * received and handle_packet() has run (phase is no longer PACKET_IN). */
    if (ide->cd.phase == ATAPI_PHASE_DATA_IN) {
        /*
         * Device has data for the host.
         * Interrupt Reason: I/O=1 C/D=0 → 0x02.
         * First DRQ block size = min(data_len, BCL).
         */
        size_t first = ide->cd.data_len < (size_t)ide->byte_count_limit
                       ? ide->cd.data_len
                       : (size_t)ide->byte_count_limit;
        ide->drq_block_end = first; /* data_pos starts at 0 */
        uint16_t bc = (uint16_t)first;
        ide->sector_count = 0x02;
        ide->cyl_low  = (uint8_t)(bc & 0xff);
        ide->cyl_high = (uint8_t)((bc >> 8) & 0xff);
        set_error(ide, atapi_cd_error(&ide->cd));
        set_status(ide, ATA_STATUS_DRDY | ATA_STATUS_DRQ);
        ide->irq_pending = true;

    } else if (ide->cd.phase == ATAPI_PHASE_STATUS) {
        /*
         * No data transfer — command complete.
         * Interrupt Reason: I/O=1 C/D=1 → 0x03.
         */
        ide->sector_count = 0x03;
        set_error(ide, atapi_cd_error(&ide->cd));
        set_status(ide, ATA_STATUS_DRDY |
                        (atapi_cd_error(&ide->cd) ? ATA_STATUS_ERR : 0));
        ide->irq_pending = true;
    }
    /* ATAPI_PHASE_PACKET_IN: CDB not complete yet — don't change status */
}

uint32_t gayle_ide_read32(GayleIde *ide)
{
    uint16_t hi = gayle_ide_read16(ide);
    uint16_t lo = gayle_ide_read16(ide);

    return ((uint32_t)hi << 16) | lo;
}

void gayle_ide_write32(GayleIde *ide, uint32_t value)
{
    gayle_ide_write16(ide, (uint16_t)((value >> 16) & 0xffff));
    gayle_ide_write16(ide, (uint16_t)(value & 0xffff));
}

bool gayle_ide_irq_pending(const GayleIde *ide)
{
    return ide ? ide->irq_pending : false;
}

void gayle_ide_clear_irq(GayleIde *ide)
{
    if (ide)
        ide->irq_pending = false;
}