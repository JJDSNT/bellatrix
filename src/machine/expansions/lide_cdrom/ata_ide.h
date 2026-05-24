#ifndef BELLATRIX_LIDE_ATA_IDE_H
#define BELLATRIX_LIDE_ATA_IDE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Simplified ATA/IDE register layer for ATAPI-only channel emulation.
 *
 * RIPPLE board layout (offsets from board base):
 *   CS1 primary channel  = 0x1000  (CHANNEL_0)
 *   CS2 secondary channel = 0x2000 (CHANNEL_1, unimplemented)
 *   Register stride       = 0x200  (NEXT_REG)
 *
 * The RIPPLE board hardware byte-swaps D0-D7 ↔ D8-D15 between the CPU
 * and the IDE bus.  Our emulation replicates this on 16-bit data reads:
 *   - IDENTIFY data: stored as ATA-native words (lo byte first) and
 *     returned byte-swapped; lide.device re-swaps to get the correct value.
 *   - Sector data:  emitted as natural byte order; the byte-swap + MOVEM
 *     produces correct big-endian memory layout on the Amiga side.
 *
 * The IDE register offset seen by the CPU is decoded as:
 *   offset_from_CS1 = board_offset - 0x1000
 *   reg_number      = (offset_from_CS1 / 0x200) & 7
 *
 * ATA registers (ATAPI uses the same):
 *   reg 0: DATA  (16-bit)
 *   reg 1: ERROR / FEATURES
 *   reg 2: SECTOR COUNT / INTERRUPT REASON
 *   reg 3: LBA LOW  / BYTE COUNT LOW
 *   reg 4: LBA MID  / BYTE COUNT HIGH (lo)
 *   reg 5: LBA HIGH / BYTE COUNT HIGH (hi)
 *   reg 6: DEVICE / HEAD
 *   reg 7: STATUS / COMMAND
 */

/* Transfer buffer capacity — large enough for up to 256 CD-ROM sectors */
#define ATA_XFER_BUF_SECTORS  256u
#define ATA_XFER_BUF_SIZE     (ATA_XFER_BUF_SECTORS * 2048u)

typedef enum AtaRegState {
    ATA_IDLE = 0,
    ATA_IDENTIFY,        /* sending IDENTIFY response */
    ATA_ATAPI_CMD,       /* waiting for ATAPI packet bytes */
    ATA_ATAPI_DATA_IN,   /* sending ATAPI data to host */
    ATA_ATAPI_DATA_OUT,  /* receiving ATAPI data from host */
    ATA_ATAPI_STATUS,    /* final status after command */
} AtaRegState;

typedef struct AtaIdeChannel {
    /* Scratch registers */
    uint8_t  features;
    uint8_t  sector_count;
    uint8_t  lba_low;
    uint8_t  lba_mid;
    uint8_t  lba_high;
    uint8_t  dev_head;
    uint8_t  error;
    uint8_t  status;

    /* Transfer state */
    AtaRegState state;
    uint8_t *xfer_buf;   /* dynamically allocated (ATA_XFER_BUF_SIZE bytes) */
    size_t   xfer_len;   /* bytes valid in xfer_buf */
    size_t   xfer_pos;   /* next byte to emit (in 16-bit word steps) */

    /* ATAPI packet accumulation */
    uint8_t  atapi_pkt[12];
    size_t   atapi_pkt_pos;

    /* Pointer to ATAPI layer (set by lide_cdrom_init) */
    void    *atapi_ctx;
    int    (*atapi_exec)(void *ctx,
                         const uint8_t *pkt, size_t pkt_len,
                         uint8_t *buf_out, size_t buf_max,
                         size_t *data_len_out);
} AtaIdeChannel;

/* alloc/free manage the xfer_buf heap allocation.
 * Call ata_ide_channel_alloc() once after embedding in a struct,
 * then ata_ide_channel_free() before destroying the struct. */
int     ata_ide_channel_alloc(AtaIdeChannel *ch);   /* returns 0 on success */
void    ata_ide_channel_free(AtaIdeChannel *ch);
void    ata_ide_channel_init(AtaIdeChannel *ch);     /* zeros registers, resets state */
void    ata_ide_channel_reset(AtaIdeChannel *ch);

/* Byte-level register read/write.
 * 'reg' is the ATA register number 0-7. */
uint8_t  ata_ide_read8 (AtaIdeChannel *ch, uint8_t reg);
uint16_t ata_ide_read16(AtaIdeChannel *ch, uint8_t reg);
void     ata_ide_write8 (AtaIdeChannel *ch, uint8_t reg, uint8_t  value);
void     ata_ide_write16(AtaIdeChannel *ch, uint8_t reg, uint16_t value);

/* Map a board byte-offset (relative to CS1 base 0x1000) to a register
 * number 0-7.  Returns -1 for addresses that don't decode. */
int ata_ide_offset_to_reg(uint32_t board_offset);

#ifdef __cplusplus
}
#endif

#endif
