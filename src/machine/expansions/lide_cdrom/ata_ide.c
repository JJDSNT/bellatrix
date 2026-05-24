#include "machine/expansions/lide_cdrom/ata_ide.h"

#include "support.h"

#include <string.h>

/* ATA status register bits */
#define ATA_STATUS_BSY   0x80u
#define ATA_STATUS_RDY   0x40u
#define ATA_STATUS_DRQ   0x08u
#define ATA_STATUS_ERR   0x01u

/* ATA error register bits */
#define ATA_ERR_ABRT     0x04u

/* ATAPI Interrupt Reason (sector_count register during packet phase) */
#define IR_COD  0x01u  /* 1 = command, 0 = data */
#define IR_IO   0x02u  /* 1 = device-to-host, 0 = host-to-device */
#define IR_REL  0x04u  /* release */

/* ATAPI signature values for IDENTIFY check */
#define ATAPI_SIG_LBA_MID   0x14u
#define ATAPI_SIG_LBA_HIGH  0xEBu

/* ATA command codes */
#define ATA_CMD_IDENTIFY_DEVICE  0xECu
#define ATA_CMD_IDENTIFY_PACKET  0xA1u
#define ATA_CMD_PACKET           0xA0u
#define ATA_CMD_DEVICE_RESET     0x08u

/* RIPPLE board CS1 offset and register stride */
#define RIPPLE_CS1_OFFSET  0x1000u
#define RIPPLE_NEXT_REG    0x0200u

/* ------------------------------------------------------------------ */

static void build_identify(AtaIdeChannel *ch)
{
    uint8_t *buf = ch->xfer_buf;
    memset(buf, 0, 512);

    /*
     * IDENTIFY PACKET DEVICE response (256 words = 512 bytes).
     * Stored as ATA-native little-endian words.
     * The RIPPLE hardware byte-swaps these when the CPU reads them;
     * lide.device re-swaps them back to get the correct values.
     *
     * Helper macro: store ATA word W at word index I (as little-endian):
     */
#define WORD_LE(i, w)  do { buf[(i)*2] = (uint8_t)((w)&0xFF); \
                             buf[(i)*2+1] = (uint8_t)((w)>>8); } while(0)

    WORD_LE(0,  0x85C0u);  /* word 0: ATAPI device, packet cmd 12 bytes    */
    WORD_LE(1,  0x0000u);  /* cylinders (unused for ATAPI)                 */
    WORD_LE(2,  0xC837u);  /* specific config                              */
    WORD_LE(49, 0x0200u);  /* capabilities: LBA supported                  */
    WORD_LE(51, 0x0200u);  /* PIO cycle timing                             */
    WORD_LE(53, 0x0006u);  /* words 64-70 and 88 valid                     */
    WORD_LE(63, 0x0007u);  /* multiword DMA modes supported                */
    WORD_LE(64, 0x0001u);  /* advanced PIO modes supported                 */

    /* Serial number (words 10-19): "00000000000000000001"
     * Each word: high byte = char[0], low byte = char[1] of the pair.
     * lide swaps back, giving correct char order in memory. */
    static const char serial[] = "00000000000000000001";
    {
        int i;
        for (i = 0; i < 10; i++) {
            /* high byte (lo in buf) = second char; low byte (hi in buf) = first */
            buf[(10 + i) * 2]     = (uint8_t)serial[i * 2 + 1];
            buf[(10 + i) * 2 + 1] = (uint8_t)serial[i * 2];
        }
    }

    /* Firmware revision (words 23-26): "1.00    " */
    static const char fw[] = "1.00    ";
    {
        int i;
        for (i = 0; i < 4; i++) {
            buf[(23 + i) * 2]     = (uint8_t)fw[i * 2 + 1];
            buf[(23 + i) * 2 + 1] = (uint8_t)fw[i * 2];
        }
    }

    /* Model (words 27-46): 40 chars, space-padded */
    static const char model[] = "BELLATRIX VIRTUAL CD-ROM        "
                                "        ";
    {
        int i;
        for (i = 0; i < 20; i++) {
            buf[(27 + i) * 2]     = (uint8_t)model[i * 2 + 1];
            buf[(27 + i) * 2 + 1] = (uint8_t)model[i * 2];
        }
    }

    WORD_LE(47, 0x8001u);  /* max sectors per IRQ                         */
    WORD_LE(80, 0x00F0u);  /* major version: ATA-4 through ATA-7          */
    WORD_LE(81, 0x0022u);  /* minor version                               */

#undef WORD_LE

    ch->xfer_len = 512;
    ch->xfer_pos = 0;
}

/* ------------------------------------------------------------------ */

void ata_ide_channel_init(AtaIdeChannel *ch)
{
    memset(ch, 0, sizeof(*ch));
    ata_ide_channel_reset(ch);
}

void ata_ide_channel_reset(AtaIdeChannel *ch)
{
    ch->error        = 0x01u;              /* DIAG OK after reset */
    ch->status       = ATA_STATUS_RDY;
    ch->lba_mid      = ATAPI_SIG_LBA_MID;
    ch->lba_high     = ATAPI_SIG_LBA_HIGH;
    ch->dev_head     = 0x00u;
    ch->features     = 0;
    ch->sector_count = 0;
    ch->lba_low      = 0;
    ch->state        = ATA_IDLE;
    ch->xfer_len     = 0;
    ch->xfer_pos     = 0;
    ch->atapi_pkt_pos = 0;
}

/* ------------------------------------------------------------------ */

int ata_ide_offset_to_reg(uint32_t board_offset)
{
    uint32_t rel;
    uint32_t reg;

    if (board_offset < RIPPLE_CS1_OFFSET)
        return -1;
    if (board_offset >= RIPPLE_CS1_OFFSET + 8u * RIPPLE_NEXT_REG + 2u)
        return -1;

    rel = board_offset - RIPPLE_CS1_OFFSET;
    reg = rel / RIPPLE_NEXT_REG;

    if (reg >= 8u) {
        /* Byte offset 0-1 from CS1 decodes as reg 0 (DATA) */
        if (rel <= 1u)
            reg = 0u;
        else
            return -1;
    }

    return (int)reg;
}

/* ------------------------------------------------------------------ */

static void execute_command(AtaIdeChannel *ch, uint8_t cmd)
{
    kprintf("[ATA-IDE] cmd %02x\n", (unsigned)cmd);
    switch (cmd) {
    case ATA_CMD_DEVICE_RESET:
        ata_ide_channel_reset(ch);
        break;

    case ATA_CMD_IDENTIFY_DEVICE:
        /*
         * ATAPI devices should respond to IDENTIFY DEVICE by setting the
         * ATAPI signature and asserting ABRT so the host falls back to
         * IDENTIFY PACKET DEVICE.
         */
        ch->lba_mid  = ATAPI_SIG_LBA_MID;
        ch->lba_high = ATAPI_SIG_LBA_HIGH;
        ch->error    = ATA_ERR_ABRT;
        ch->status   = ATA_STATUS_RDY | ATA_STATUS_ERR;
        break;

    case ATA_CMD_IDENTIFY_PACKET:
        build_identify(ch);
        ch->state  = ATA_IDENTIFY;
        ch->status = ATA_STATUS_RDY | ATA_STATUS_DRQ;
        ch->error  = 0;
        /* Byte count registers indicate transfer size */
        ch->lba_mid  = (uint8_t)(512u & 0xFFu);
        ch->lba_high = (uint8_t)(512u >> 8);
        break;

    case ATA_CMD_PACKET:
        ch->state         = ATA_ATAPI_CMD;
        ch->atapi_pkt_pos = 0;
        ch->status        = ATA_STATUS_RDY | ATA_STATUS_DRQ;
        ch->error         = 0;
        /* Interrupt reason: COD=1, IO=0 (host sends packet) */
        ch->sector_count  = IR_COD;
        break;

    default:
        kprintf("[ATA-IDE] unknown command %02x\n", (unsigned)cmd);
        ch->error  = ATA_ERR_ABRT;
        ch->status = ATA_STATUS_RDY | ATA_STATUS_ERR;
        break;
    }
}

static void execute_atapi_packet(AtaIdeChannel *ch)
{
    size_t data_len = 0;
    int rc;

    if (!ch->atapi_exec) {
        ch->error  = ATA_ERR_ABRT;
        ch->status = ATA_STATUS_RDY | ATA_STATUS_ERR;
        return;
    }

    rc = ch->atapi_exec(ch->atapi_ctx,
                        ch->atapi_pkt, 12,
                        ch->xfer_buf, ATA_XFER_BUF_SIZE,
                        &data_len);

    if (rc != 0 || data_len == 0) {
        /* Error or no-data command */
        ch->state  = ATA_IDLE;
        ch->status = ATA_STATUS_RDY | (rc ? ATA_STATUS_ERR : 0);
        if (rc) ch->error = ATA_ERR_ABRT;
        /* Interrupt reason: IO=1, COD=1 = status phase */
        ch->sector_count = IR_IO | IR_COD;
    } else {
        ch->xfer_len     = data_len;
        ch->xfer_pos     = 0;
        ch->state        = ATA_ATAPI_DATA_IN;
        ch->status       = ATA_STATUS_RDY | ATA_STATUS_DRQ;
        ch->error        = 0;
        /* Byte count = transfer size (capped at 0xFFFE) */
        uint16_t bc = (data_len > 0xFFFEu) ? (uint16_t)0xFFFEu : (uint16_t)data_len;
        ch->lba_mid  = (uint8_t)(bc & 0xFFu);
        ch->lba_high = (uint8_t)(bc >> 8);
        /* Interrupt reason: IO=1, COD=0 = data to host */
        ch->sector_count = IR_IO;
    }
}

/* ------------------------------------------------------------------ */

uint8_t ata_ide_read8(AtaIdeChannel *ch, uint8_t reg)
{
    switch (reg) {
    case 0:  /* DATA — byte read not the normal path, but handle it */
        return 0xFFu;
    case 1:  return ch->error;
    case 2:  return ch->sector_count;
    case 3:  return ch->lba_low;
    case 4:  return ch->lba_mid;
    case 5:  return ch->lba_high;
    case 6:  return ch->dev_head | 0xA0u;
    case 7:  return ch->status;
    default: return 0xFFu;
    }
}

uint16_t ata_ide_read16(AtaIdeChannel *ch, uint8_t reg)
{
    uint16_t word;
    uint8_t  lo, hi;

    if (reg != 0)
        return (uint16_t)ata_ide_read8(ch, reg);

    /* DATA register: return next 16-bit word from transfer buffer. */
    if (ch->state == ATA_IDENTIFY || ch->state == ATA_ATAPI_DATA_IN) {
        if (ch->xfer_pos + 1u < ch->xfer_len) {
            lo = ch->xfer_buf[ch->xfer_pos];
            hi = ch->xfer_buf[ch->xfer_pos + 1u];
            ch->xfer_pos += 2u;

            /*
             * Simulate the RIPPLE hardware byte-swap: the CPU sees the
             * bytes in reversed order.  lide.device re-swaps them.
             *
             * For IDENTIFY: lo=ATA_low_byte, hi=ATA_high_byte
             *   → return (lo<<8)|hi; lide re-swaps → (hi<<8)|lo = ATA_word ✓
             *
             * For sector data: natural byte pair [B0, B1]
             *   → return (B0<<8)|B1; MOVEM stores B0, B1 in memory ✓
             */
            word = (uint16_t)(((uint16_t)lo << 8) | hi);

            if (ch->xfer_pos >= ch->xfer_len) {
                /* Transfer complete */
                if (ch->state == ATA_ATAPI_DATA_IN) {
                    ch->state        = ATA_IDLE;
                    ch->status       = ATA_STATUS_RDY;
                    ch->sector_count = IR_IO | IR_COD;
                } else {
                    ch->state  = ATA_IDLE;
                    ch->status = ATA_STATUS_RDY;
                }
                ch->xfer_len = 0;
                ch->xfer_pos = 0;
            }

            return word;
        }
    }

    return 0xFFFFu;
}

void ata_ide_write8(AtaIdeChannel *ch, uint8_t reg, uint8_t value)
{
    switch (reg) {
    case 0:  break;                         /* DATA byte write — ignore      */
    case 1:  ch->features    = value; break;
    case 2:  ch->sector_count = value; break;
    case 3:  ch->lba_low     = value; break;
    case 4:  ch->lba_mid     = value; break;
    case 5:  ch->lba_high    = value; break;
    case 6:  ch->dev_head    = value; break;
    case 7:
        /* COMMAND register */
        execute_command(ch, value);
        break;
    default:
        break;
    }
}

void ata_ide_write16(AtaIdeChannel *ch, uint8_t reg, uint16_t value)
{
    uint8_t lo = (uint8_t)(value & 0xFFu);
    uint8_t hi = (uint8_t)(value >> 8);

    if (reg != 0) {
        ata_ide_write8(ch, reg, lo);
        return;
    }

    /* DATA register write: ATAPI packet bytes come in here */
    if (ch->state == ATA_ATAPI_CMD && ch->atapi_pkt_pos < 12u) {
        /*
         * Undo the byte-swap before storing the packet byte:
         * the CPU writes a byte-swapped word, so we reverse it.
         */
        ch->atapi_pkt[ch->atapi_pkt_pos++] = hi;
        if (ch->atapi_pkt_pos < 12u)
            ch->atapi_pkt[ch->atapi_pkt_pos++] = lo;

        if (ch->atapi_pkt_pos >= 12u)
            execute_atapi_packet(ch);
    }
}
