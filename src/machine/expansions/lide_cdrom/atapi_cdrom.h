#ifndef BELLATRIX_LIDE_ATAPI_CDROM_H
#define BELLATRIX_LIDE_ATAPI_CDROM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ATAPI CD-ROM emulation.
 *
 * Implements the subset of ATAPI/SCSI-2 commands required by lide.device:
 *   TEST UNIT READY, INQUIRY, READ TOC, REQUEST SENSE,
 *   READ(10), MODE SENSE(10), START STOP UNIT
 *
 * The ISO image backend is abstracted via AtapiMediaOps so the ATAPI
 * layer does not depend on a specific storage implementation.
 */

#define ATAPI_SECTOR_SIZE   2048u
#define ATAPI_TOC_DATA_LEN  12u   /* first/last track + track 1 entry */

typedef struct AtapiMediaOps {
    /* Returns non-zero if media is present */
    int  (*present)(void *ctx);
    /* Read 'count' sectors starting at 'lba' into 'buf'.  Returns 0 on ok. */
    int  (*read_sectors)(void *ctx, uint32_t lba, uint32_t count, uint8_t *buf);
    /* Total sectors on media, or 0 if no media. */
    uint32_t (*sector_count)(void *ctx);
} AtapiMediaOps;

typedef struct AtapiCdromState {
    const AtapiMediaOps *ops;
    void *ops_ctx;

    /* SENSE data from last failed command */
    uint8_t sense_key;
    uint8_t asc;
    uint8_t ascq;

    bool media_present;
    bool unit_attention;  /* raised on power-on/reset and media change */
} AtapiCdromState;

void atapi_cdrom_init(AtapiCdromState *s,
                      const AtapiMediaOps *ops, void *ops_ctx);
void atapi_cdrom_reset(AtapiCdromState *s);

void atapi_cdrom_insert(AtapiCdromState *s);
void atapi_cdrom_eject(AtapiCdromState *s);

/*
 * Execute an ATAPI packet command.
 * 'pkt'        : 12-byte CDB
 * 'buf_out'    : output buffer for data-in commands
 * 'buf_max'    : capacity of buf_out
 * 'data_len'   : set to actual bytes written (0 for non-data commands)
 *
 * Returns 0 on success, negative on device error (sense data set).
 */
int atapi_cdrom_exec(void *ctx,
                     const uint8_t *pkt, size_t pkt_len,
                     uint8_t *buf_out, size_t buf_max,
                     size_t *data_len);

#ifdef __cplusplus
}
#endif

#endif
