// src/bus/gayle/gayle_ide.c

#include "bus/gayle/gayle_ide.h"

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
    buf[word * 2 + 0] = (uint8_t)(value & 0xff);
    buf[word * 2 + 1] = (uint8_t)((value >> 8) & 0xff);
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
    ide->identify_active = false;

    /*
     * ATAPI PACKET phase:
     * AROS will write 12 packet bytes via data port.
     */
    atapi_cd_begin_packet(&ide->cd);

    set_error(ide, 0);
    set_status(ide, ATA_STATUS_DRDY | ATA_STATUS_DRQ);

    /*
     * Signature / byte count registers often used by ATAPI probing.
     * Keep 2048 as requested transfer size hint.
     */
    ide->cyl_low = 0x00;
    ide->cyl_high = 0x08;
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

    atapi_cd_reset(&ide->cd);
}

uint8_t gayle_ide_read8(GayleIde *ide, uint32_t reg)
{
    if (!ide)
        return 0xff;

    switch (reg) {
        case GAYLE_IDE_REG_ERROR:
            return ide->error;

        case GAYLE_IDE_REG_SECCNT:
            return ide->sector_count;

        case GAYLE_IDE_REG_SECNUM:
            return ide->sector_number;

        case GAYLE_IDE_REG_CYLLO:
            return ide->cyl_low;

        case GAYLE_IDE_REG_CYLHI:
            return ide->cyl_high;

        case GAYLE_IDE_REG_DEVHEAD:
            return ide->dev_head;

        case GAYLE_IDE_REG_STATUS:
            if (ide->cd.phase == ATAPI_PHASE_DATA_IN)
                return ATA_STATUS_DRDY | ATA_STATUS_DRQ;
            if (ide->cd.phase == ATAPI_PHASE_PACKET_IN)
                return ATA_STATUS_DRDY | ATA_STATUS_DRQ;
            return ide->status;

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
            ide->features = value;
            break;

        case GAYLE_IDE_REG_SECCNT:
            ide->sector_count = value;
            break;

        case GAYLE_IDE_REG_SECNUM:
            ide->sector_number = value;
            break;

        case GAYLE_IDE_REG_CYLLO:
            ide->cyl_low = value;
            break;

        case GAYLE_IDE_REG_CYLHI:
            ide->cyl_high = value;
            break;

        case GAYLE_IDE_REG_DEVHEAD:
            ide->dev_head = value;
            break;

        case GAYLE_IDE_REG_COMMAND:
            switch (value) {
                case ATA_CMD_DEVICE_RESET:
                    device_reset(ide);
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

        if (!atapi_cd_has_data(&ide->cd))
            set_status(ide, ATA_STATUS_DRDY);

        return v;
    }

    return 0xffff;
}

void gayle_ide_write16(GayleIde *ide, uint16_t value)
{
    if (!ide)
        return;

    if (ide->cd.phase == ATAPI_PHASE_PACKET_IN) {
        if (atapi_cd_write_packet_word(&ide->cd, value)) {
            if (ide->cd.phase == ATAPI_PHASE_DATA_IN) {
                set_error(ide, atapi_cd_error(&ide->cd));
                set_status(ide, ATA_STATUS_DRDY | ATA_STATUS_DRQ);
            } else {
                set_error(ide, atapi_cd_error(&ide->cd));
                set_status(ide, ATA_STATUS_DRDY |
                                (atapi_cd_error(&ide->cd) ? ATA_STATUS_ERR : 0));
            }
        }
    }
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
    (void)ide;
    return false;
}

void gayle_ide_clear_irq(GayleIde *ide)
{
    (void)ide;
}