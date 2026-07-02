#include "machine/expansions/lide_cdrom/ata_ide.h"

#include "support.h"
#include <stdlib.h>

/* Per-command trace logging: enable with HARNESS_CD_TRACE=1.
 * Off by default — during sustained CD I/O this logging is heavy enough
 * to throttle the emulator when stdout is a terminal. */
static int lide_cd_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("HARNESS_CD_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return enabled;
}
#define CD_TRACE(...) do { if (lide_cd_trace_enabled()) kprintf(__VA_ARGS__); } while (0)


#include <stdlib.h>
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
#define ATA_CMD_READ_SECTORS     0x20u
#define ATA_CMD_WRITE_SECTORS    0x30u
#define ATA_CMD_SET_FEATURES     0xEFu

/* Device selection: dev_head bit 4 (0 = master/disk, 1 = slave/CD) */
#define ATA_DEV_SLAVE_BIT        0x10u
#define ATA_SECTOR_SIZE          512u

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

static int ata_disk_present(const AtaIdeChannel *ch)
{
    return ch->disk_read != NULL;
}

/* 1 = slave (ATAPI CD), 0 = master (ATA disk) */
static int ata_selected_dev(const AtaIdeChannel *ch)
{
    return (ch->dev_head & ATA_DEV_SLAVE_BIT) ? 1 : 0;
}

static void build_identify_disk(AtaIdeChannel *ch)
{
    uint8_t *buf = ch->xfer_buf;
    uint32_t total = ch->disk_sectors;
    uint16_t cyl, heads = 16u, spt = 63u;
    uint32_t c;

    memset(buf, 0, 512);

    /* Same convention as build_identify: ATA-native little-endian words;
     * the RIPPLE byte-swap + lide.device re-swap restores them. */
#define WORD_LE(i, w)  do { buf[(i)*2] = (uint8_t)((w)&0xFF); \
                             buf[(i)*2+1] = (uint8_t)((w)>>8); } while(0)

    c = total / ((uint32_t)heads * spt);
    cyl = (c > 65535u) ? (uint16_t)65535u : (uint16_t)c;

    WORD_LE(0,  0x0040u);  /* ATA device, fixed media                      */
    WORD_LE(1,  cyl);      /* cylinders                                    */
    WORD_LE(3,  heads);    /* heads                                        */
    WORD_LE(6,  spt);      /* sectors per track                            */
    WORD_LE(47, 0x8000u);  /* READ/WRITE MULTIPLE: max 0 → not used        */
    WORD_LE(49, 0x0200u);  /* capabilities: LBA supported                  */
    WORD_LE(53, 0x0006u);  /* words 64-70 and 88 valid                     */
    WORD_LE(60, (uint16_t)(total & 0xFFFFu));   /* LBA28 total sectors lo  */
    WORD_LE(61, (uint16_t)(total >> 16));       /* LBA28 total sectors hi  */
    WORD_LE(64, 0x0001u);  /* advanced PIO modes                           */
    WORD_LE(80, 0x00F0u);  /* major version: ATA-4 through ATA-7           */
    WORD_LE(83, 0x4000u);  /* command sets: no LBA48                       */

    /* Serial (words 10-19), firmware (23-26), model (27-46): stored with
     * the char pair swapped, exactly like build_identify. */
    static const char serial[] = "00000000000000000002";
    static const char fw[]     = "1.00    ";
    static const char model[]  = "BELLATRIX VIRTUAL HARDDISK      "
                                 "        ";
    {
        int i;
        for (i = 0; i < 10; i++) {
            buf[(10 + i) * 2]     = (uint8_t)serial[i * 2 + 1];
            buf[(10 + i) * 2 + 1] = (uint8_t)serial[i * 2];
        }
        for (i = 0; i < 4; i++) {
            buf[(23 + i) * 2]     = (uint8_t)fw[i * 2 + 1];
            buf[(23 + i) * 2 + 1] = (uint8_t)fw[i * 2];
        }
        for (i = 0; i < 20; i++) {
            buf[(27 + i) * 2]     = (uint8_t)model[i * 2 + 1];
            buf[(27 + i) * 2 + 1] = (uint8_t)model[i * 2];
        }
    }
#undef WORD_LE

    ch->xfer_len = 512;
    ch->xfer_pos = 0;
}

/* ------------------------------------------------------------------ */

int ata_ide_channel_alloc(AtaIdeChannel *ch)
{
    ch->xfer_buf = (uint8_t *)calloc(1, ATA_XFER_BUF_SIZE);
    return ch->xfer_buf ? 0 : -1;
}

void ata_ide_channel_free(AtaIdeChannel *ch)
{
    free(ch->xfer_buf);
    ch->xfer_buf = NULL;
}

void ata_ide_channel_init(AtaIdeChannel *ch)
{
    uint8_t *saved_buf = ch->xfer_buf;
    memset(ch, 0, sizeof(*ch));
    ch->xfer_buf = saved_buf;
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

/* ATA disk (device 0) command execution */
static void execute_disk_command(AtaIdeChannel *ch, uint8_t cmd)
{
    uint32_t lba, count;

    switch (cmd) {
    case ATA_CMD_DEVICE_RESET: {
        /* DEV bit survives reset — lide relies on its shadow copy and
         * does not re-select after a reset. */
        uint8_t dev = ch->dev_head & ATA_DEV_SLAVE_BIT;
        ata_ide_channel_reset(ch);
        ch->dev_head = dev;
        /* ATA (non-PACKET) signature */
        ch->lba_mid  = 0;
        ch->lba_high = 0;
        break;
    }

    case ATA_CMD_IDENTIFY_DEVICE:
        build_identify_disk(ch);
        ch->state  = ATA_DATA_IN;
        ch->status = ATA_STATUS_RDY | ATA_STATUS_DRQ;
        ch->error  = 0;
        break;

    case ATA_CMD_SET_FEATURES:
        ch->status = ATA_STATUS_RDY;
        ch->error  = 0;
        break;

    case ATA_CMD_READ_SECTORS:
    case ATA_CMD_WRITE_SECTORS:
        lba = ((uint32_t)(ch->dev_head & 0x0Fu) << 24) |
              ((uint32_t)ch->lba_high << 16) |
              ((uint32_t)ch->lba_mid  << 8)  |
               (uint32_t)ch->lba_low;
        count = ch->sector_count ? ch->sector_count : 256u;

        CD_TRACE("[ATA-IDE] disk %s lba=%u count=%u\n",
                 cmd == ATA_CMD_READ_SECTORS ? "READ" : "WRITE",
                 (unsigned)lba, (unsigned)count);

        if (lba + count > ch->disk_sectors ||
            count * ATA_SECTOR_SIZE > ATA_XFER_BUF_SIZE) {
            kprintf("[ATA-IDE] disk %s out of range: lba=%u count=%u total=%u\n",
                    cmd == ATA_CMD_READ_SECTORS ? "READ" : "WRITE",
                    (unsigned)lba, (unsigned)count, (unsigned)ch->disk_sectors);
            ch->error  = ATA_ERR_ABRT;
            ch->status = ATA_STATUS_RDY | ATA_STATUS_ERR;
            break;
        }

        if (cmd == ATA_CMD_READ_SECTORS) {
            if (ch->disk_read(ch->disk_ctx, lba, count, ch->xfer_buf) != 0) {
                ch->error  = ATA_ERR_ABRT;
                ch->status = ATA_STATUS_RDY | ATA_STATUS_ERR;
                break;
            }
            ch->xfer_len = (size_t)count * ATA_SECTOR_SIZE;
            ch->xfer_pos = 0;
            ch->state    = ATA_DATA_IN;
            ch->status   = ATA_STATUS_RDY | ATA_STATUS_DRQ;
            ch->error    = 0;
        } else {
            if (!ch->disk_write) {
                ch->error  = ATA_ERR_ABRT;
                ch->status = ATA_STATUS_RDY | ATA_STATUS_ERR;
                break;
            }
            ch->disk_wr_lba   = lba;
            ch->disk_wr_count = count;
            ch->xfer_len = (size_t)count * ATA_SECTOR_SIZE;
            ch->xfer_pos = 0;
            ch->state    = ATA_DATA_OUT;
            ch->status   = ATA_STATUS_RDY | ATA_STATUS_DRQ;
            ch->error    = 0;
        }
        break;

    default:
        kprintf("[ATA-IDE] disk: unknown command %02x\n", (unsigned)cmd);
        ch->error  = ATA_ERR_ABRT;
        ch->status = ATA_STATUS_RDY | ATA_STATUS_ERR;
        break;
    }
}

static void execute_command(AtaIdeChannel *ch, uint8_t cmd)
{
    CD_TRACE("[ATA-IDE] cmd %02x dev=%d\n", (unsigned)cmd, ata_selected_dev(ch));

    /* Device 0 (master) = ATA disk when attached; device 1 (slave) = ATAPI
     * CD.  With no disk, master must fail cleanly — if the ATAPI device
     * answered both selections, lide.device would create two units and the
     * CD would mount twice (CD0: + CD1:, the "two floppy icons" symptom). */
    if (ata_selected_dev(ch) == 0) {
        if (ata_disk_present(ch)) {
            execute_disk_command(ch, cmd);
        } else {
            /* absent master: no ATAPI signature, command aborted */
            ch->lba_mid  = 0;
            ch->lba_high = 0;
            ch->error    = ATA_ERR_ABRT;
            ch->status   = ATA_STATUS_RDY | ATA_STATUS_ERR;
        }
        return;
    }

    switch (cmd) {
    case ATA_CMD_DEVICE_RESET: {
        uint8_t dev = ch->dev_head & ATA_DEV_SLAVE_BIT;
        if (ch->atapi_reset)
            ch->atapi_reset(ch->atapi_ctx);
        ata_ide_channel_reset(ch);
        ch->dev_head = dev;      /* DEV bit survives reset (see disk path) */
        break;
    }

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

    CD_TRACE("[ATA-IDE] ATAPI pkt: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            (unsigned)ch->atapi_pkt[0],  (unsigned)ch->atapi_pkt[1],
            (unsigned)ch->atapi_pkt[2],  (unsigned)ch->atapi_pkt[3],
            (unsigned)ch->atapi_pkt[4],  (unsigned)ch->atapi_pkt[5],
            (unsigned)ch->atapi_pkt[6],  (unsigned)ch->atapi_pkt[7],
            (unsigned)ch->atapi_pkt[8],  (unsigned)ch->atapi_pkt[9],
            (unsigned)ch->atapi_pkt[10], (unsigned)ch->atapi_pkt[11]);

    rc = ch->atapi_exec(ch->atapi_ctx,
                        ch->atapi_pkt, 12,
                        ch->xfer_buf, ATA_XFER_BUF_SIZE,
                        &data_len);

    CD_TRACE("[ATA-IDE] atapi_exec rc=%d data_len=%u\n", rc, (unsigned)data_len);

    if (rc != 0 || data_len == 0) {
        /* Error or no-data command */
        ch->state  = ATA_IDLE;
        ch->status = ATA_STATUS_RDY | (rc ? ATA_STATUS_ERR : 0);
        if (rc) ch->error = ATA_ERR_ABRT;
        /* Interrupt reason: IO=1, COD=1 = status phase */
        ch->sector_count = IR_IO | IR_COD;
    } else {
        /*
         * RIPPLE hardware byte-swaps D0-D7 <-> D8-D15.  read16() returns
         * (buf[pos]<<8)|buf[pos+1], which replicates this: the m68k sees
         * the original big-endian byte pair and stores it correctly.
         * No pre-swap needed — lide.device stores words directly from the
         * data register without re-swapping for ATAPI command responses.
         * (Only IDENTIFY data is explicitly re-swapped by lide.device.)
         * Sector data (READ_10) uses read_fast/MOVEM.L: two 16-bit words
         * per longword give the correct big-endian layout in memory.
         */

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
    if (ch->state == ATA_IDENTIFY || ch->state == ATA_ATAPI_DATA_IN ||
        ch->state == ATA_DATA_IN) {
        if (ch->xfer_pos + 1u < ch->xfer_len) {
            lo = ch->xfer_buf[ch->xfer_pos];
            hi = ch->xfer_buf[ch->xfer_pos + 1u];
            ch->xfer_pos += 2u;

            /*
             * Replicate the RIPPLE hardware byte-swap (D0-D7 ↔ D8-D15).
             * Return (buf[pos]<<8)|buf[pos+1] — the m68k stores this as
             * [buf[pos], buf[pos+1]] in big-endian memory, which is the
             * original byte order from the ATAPI device.
             *
             * For IDENTIFY: buf stores ATA-native LE words, so
             *   lo=ATA_lo, hi=ATA_hi → (lo<<8)|hi; lide re-swaps → ATA_word
             * For ATAPI responses and sector data: buf holds big-endian bytes
             *   [B0,B1,...] → (B0<<8)|B1 → stored as [B0,B1] in memory
             */
            word = (uint16_t)(((uint16_t)lo << 8) | hi);

            /* Log first and last 4 words of each transfer */
            if (ch->xfer_pos <= 8u || ch->xfer_pos + 2u >= ch->xfer_len)
                CD_TRACE("[ATA-IDE] read16 pos=%u/%u word=%04x (lo=%02x hi=%02x)\n",
                        (unsigned)(ch->xfer_pos - 2u), (unsigned)ch->xfer_len,
                        (unsigned)word, (unsigned)lo, (unsigned)hi);

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

    /* ATA disk WRITE data: accumulate sector bytes, flush when complete.
     * The CPU writes byte-swapped words (RIPPLE swap), so memory byte
     * order is [hi, lo] — same convention as the ATAPI packet path. */
    if (ch->state == ATA_DATA_OUT) {
        if (ch->xfer_pos + 1u < ch->xfer_len) {
            ch->xfer_buf[ch->xfer_pos]      = hi;
            ch->xfer_buf[ch->xfer_pos + 1u] = lo;
            ch->xfer_pos += 2u;

            if (ch->xfer_pos >= ch->xfer_len) {
                int rc = ch->disk_write(ch->disk_ctx, ch->disk_wr_lba,
                                        ch->disk_wr_count, ch->xfer_buf);
                CD_TRACE("[ATA-IDE] disk WRITE flush lba=%u count=%u rc=%d\n",
                         (unsigned)ch->disk_wr_lba,
                         (unsigned)ch->disk_wr_count, rc);
                ch->state    = ATA_IDLE;
                ch->xfer_len = 0;
                ch->xfer_pos = 0;
                ch->status   = ATA_STATUS_RDY | (rc ? ATA_STATUS_ERR : 0);
                ch->error    = rc ? ATA_ERR_ABRT : 0;
            }
        }
        return;
    }

    /* DATA register write: ATAPI packet bytes come in here */
    if (ch->state == ATA_ATAPI_CMD && ch->atapi_pkt_pos < 12u) {
        /*
         * Undo the byte-swap before storing the packet byte:
         * the CPU writes a byte-swapped word, so we reverse it.
         */
        CD_TRACE("[ATA-IDE] pkt_write[%u] value=%04x -> bytes %02x %02x\n",
                (unsigned)ch->atapi_pkt_pos, (unsigned)value,
                (unsigned)hi, (unsigned)lo);
        ch->atapi_pkt[ch->atapi_pkt_pos++] = hi;
        if (ch->atapi_pkt_pos < 12u)
            ch->atapi_pkt[ch->atapi_pkt_pos++] = lo;

        if (ch->atapi_pkt_pos >= 12u)
            execute_atapi_packet(ch);
    }
}
