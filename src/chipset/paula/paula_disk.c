// src/chipset/paula/paula_disk.c

#include "paula_disk.h"

#include "support.h"
#include <string.h>

#define DSKLEN_DMAEN  0x8000u
#define DSKLEN_WRITE  0x4000u
#define DSKLEN_LEN    0x3FFFu

#define DSKBYTR_DMAON    0x2000u
#define DSKBYTR_WORDSYNC 0x1000u

#define ADKCON_SETCLR    0x8000u
#define ADKCON_WORDSYNC  0x0400u

#define FLOPPY_FAKE_DMA_CYCLES 46000u
#define ADF_TRACK_BYTES        5632u
#define AMIGA_SECTORS_TRACK    11u
#define AMIGA_SECTOR_BYTES     512u
#define MFM_SECTOR_BYTES       1088u

static void emit_intreq(PaulaDisk *pd, uint16_t bits)
{
    if (pd->intreq_cb) {
        pd->intreq_cb(pd->intreq_user, bits);
    }
}

static int valid_chip_range(PaulaDisk *pd, uint32_t addr, uint32_t len)
{
    if (!pd->chipram) return 0;
    if (addr >= pd->chipram_size) return 0;
    if (len > pd->chipram_size - addr) return 0;
    return 1;
}

static int valid_adf_track(PaulaDisk *pd, uint32_t adf_offset)
{
    if (!pd->drive) return 0;
    if (!pd->drive->adf) return 0;
    if (!pd->drive->disk_inserted) return 0;
    if (pd->drive->adf_size == 0) return 0;
    if (adf_offset >= pd->drive->adf_size) return 0;
    if (ADF_TRACK_BYTES > pd->drive->adf_size - adf_offset) return 0;
    return 1;
}

static void encode_even_odd(const uint8_t *src, uint8_t *dst, int size)
{
    for (int i = 0; i < size; i++) {
        dst[i]        = (src[i] >> 1) & 0x55;
        dst[i + size] = src[i] & 0x55;
    }
}

static uint8_t add_clock_bits(uint8_t previous, uint8_t value)
{
    value &= 0x55;

    uint8_t l = (uint8_t)(value << 1);
    uint8_t r = (uint8_t)((value >> 1) | (previous << 7));
    uint8_t inv = (uint8_t)(l | r);
    uint8_t clk = (uint8_t)(inv ^ 0xAA);

    return (uint8_t)(value | clk);
}

static void encode_adf_track_to_mfm(
    const uint8_t *adf_track,
    int cylinder,
    int side,
    uint8_t *dst,
    uint32_t dst_len
)
{
    uint32_t s = 0;
    uint8_t sector[544];

    memset(dst, 0, dst_len);

    for (unsigned int sec = 0; sec < AMIGA_SECTORS_TRACK; sec++) {
        if (s + MFM_SECTOR_BYTES > dst_len) break;

        memset(sector, 0, sizeof(sector));

        sector[0] = 0x00;
        sector[1] = 0x00;
        sector[2] = 0xA1;
        sector[3] = 0xA1;

        sector[4] = 0xFF;
        sector[5] = (uint8_t)((cylinder << 1) | side);
        sector[6] = (uint8_t)sec;
        sector[7] = (uint8_t)(11 - sec);

        memcpy(&sector[32], &adf_track[sec * AMIGA_SECTOR_BYTES], AMIGA_SECTOR_BYTES);

        dst[s + 0] = 0xAA;
        dst[s + 1] = 0xAA;
        dst[s + 2] = 0xAA;
        dst[s + 3] = 0xAA;

        dst[s + 4] = 0x44;
        dst[s + 5] = 0x89;
        dst[s + 6] = 0x44;
        dst[s + 7] = 0x89;

        /*
         * Header checksum: XOR of raw header info + label (sector[4..23])
         * as 32-bit big-endian long words.
         *
         * Must be computed on raw bytes, not on MFM-encoded bytes.
         * trackdisk decodes the MFM stream, XORs the decoded long words,
         * and expects the result to equal the decoded checksum field.
         */
        {
            uint8_t hcheck[4] = {0, 0, 0, 0};
            for (int k = 4; k < 24; k += 4) {
                hcheck[0] ^= sector[k + 0];
                hcheck[1] ^= sector[k + 1];
                hcheck[2] ^= sector[k + 2];
                hcheck[3] ^= sector[k + 3];
            }
            sector[24] = hcheck[0];
            sector[25] = hcheck[1];
            sector[26] = hcheck[2];
            sector[27] = hcheck[3];
        }

        /*
         * Data checksum: XOR of raw sector data (sector[32..543]).
         */
        {
            uint8_t dcheck[4] = {0, 0, 0, 0};
            for (int k = 32; k < 32 + (int)AMIGA_SECTOR_BYTES; k += 4) {
                dcheck[0] ^= sector[k + 0];
                dcheck[1] ^= sector[k + 1];
                dcheck[2] ^= sector[k + 2];
                dcheck[3] ^= sector[k + 3];
            }
            sector[28] = dcheck[0];
            sector[29] = dcheck[1];
            sector[30] = dcheck[2];
            sector[31] = dcheck[3];
        }

        encode_even_odd(&sector[4],  &dst[s + 8],  4);   /* header info */
        encode_even_odd(&sector[8],  &dst[s + 16], 16);  /* label */
        encode_even_odd(&sector[24], &dst[s + 48], 4);   /* header checksum */
        encode_even_odd(&sector[28], &dst[s + 56], 4);   /* data checksum */
        encode_even_odd(&sector[32], &dst[s + 64], 512); /* data */

        for (int i = 8; i < (int)MFM_SECTOR_BYTES; i++) {
            dst[s + i] = add_clock_bits(dst[s + i - 1], dst[s + i]);
        }

        s += MFM_SECTOR_BYTES;
    }

    if (s == 0) return;

    for (uint32_t i = s; i < dst_len; i++) {
        dst[i] = add_clock_bits(dst[i - 1], 0);
    }
}

void paula_disk_init(PaulaDisk *pd)
{
    memset(pd, 0, sizeof(*pd));
    pd->dsksync = 0x4489;
}

void paula_disk_attach_memory(PaulaDisk *pd, uint8_t *chipram, size_t size)
{
    pd->chipram = chipram;
    pd->chipram_size = size;
}

void paula_disk_attach_drive(PaulaDisk *pd, FloppyDrive *drive)
{
    pd->drive = drive;
}

void paula_disk_set_intreq_callback(PaulaDisk *pd, PaulaDiskIntreqFn cb, void *user)
{
    pd->intreq_cb = cb;
    pd->intreq_user = user;
}

void paula_disk_write_dskpth(PaulaDisk *pd, uint16_t value)
{
    pd->dskptr = (pd->dskptr & 0x0000FFFFu) | ((uint32_t)value << 16);
}

void paula_disk_write_dskptl(PaulaDisk *pd, uint16_t value)
{
    pd->dskptr = (pd->dskptr & 0xFFFF0000u) | value;
}

void paula_disk_write_dsksync(PaulaDisk *pd, uint16_t value)
{
    pd->dsksync = value;
}

void paula_disk_write_adkcon(PaulaDisk *pd, uint16_t value)
{
    uint16_t bits = value & 0x7FFFu;

    if (value & ADKCON_SETCLR) {
        pd->adkcon |= bits;
    } else {
        pd->adkcon &= (uint16_t)~bits;
    }
}

uint16_t paula_disk_read_dskbytr(PaulaDisk *pd)
{
    return pd->dskbytr;
}

static void paula_disk_start_dma(PaulaDisk *pd, uint16_t value)
{
    uint16_t len_words = value & DSKLEN_LEN;
    uint32_t len_bytes = (uint32_t)len_words << 1;

    pd->dsklen = value;
    pd->write_mode = (value & DSKLEN_WRITE) ? 1 : 0;
    pd->dma_active = 1;
    pd->dma_armed = 0;
    pd->countdown = FLOPPY_FAKE_DMA_CYCLES;
    pd->dskbytr |= DSKBYTR_DMAON;
    pd->dskbytr &= (uint16_t)~DSKBYTR_WORDSYNC;

    kprintf("[DSKDMA] start value=%04x write=%d words=%u bytes=%u dskptr=%06x\n",
           value,
           pd->write_mode,
           len_words,
           len_bytes,
           pd->dskptr);

    if (pd->write_mode) {
        kprintf("[DSKDMA] write mode ignored for now\n");
        return;
    }

    if (len_words == 0) {
        kprintf("[DSKDMA] zero length\n");
        return;
    }

    if (!pd->drive || !floppy_has_media(pd->drive)) {
        kprintf("[DSKDMA] no media\n");
        return;
    }

    int cyl = pd->drive->cylinder;
    int side = pd->drive->side ? 1 : 0;

    uint32_t adf_offset = (uint32_t)(((cyl << 1) | side) * ADF_TRACK_BYTES);

    if (!valid_adf_track(pd, adf_offset)) {
        kprintf("[DSKDMA] invalid ADF range cyl=%d side=%d offset=%u size=%u\n",
               cyl,
               side,
               adf_offset,
               pd->drive ? pd->drive->adf_size : 0);
        return;
    }

    if (!valid_chip_range(pd, pd->dskptr, len_bytes)) {
        kprintf("[DSKDMA] invalid chip range addr=%06x len=%u chip=%zu\n",
               pd->dskptr,
               len_bytes,
               pd->chipram_size);
        return;
    }

    encode_adf_track_to_mfm(
        pd->drive->adf + adf_offset,
        cyl,
        side,
        &pd->chipram[pd->dskptr],
        len_bytes
    );

    pd->drive->disk_changed = 0;

    kprintf("[DSKDMA] encoded cyl=%d side=%d adf_offset=%u -> chip=%06x len=%u\n",
           cyl,
           side,
           adf_offset,
           pd->dskptr,
           len_bytes);

    if (pd->adkcon & ADKCON_WORDSYNC) {
        pd->dskbytr |= DSKBYTR_WORDSYNC;
        kprintf("[DSKIRQ] DSKSYNC fired\n");
        emit_intreq(pd, PAULA_INTREQ_DSKSYNC);
    }
}

void paula_disk_write_dsklen(PaulaDisk *pd, uint16_t value)
{
    kprintf("[DSKLEN] write value=%04x armed=%d active=%d ptr=%06x\n",
           value,
           pd->dma_armed,
           pd->dma_active,
           pd->dskptr);

    if (!(value & DSKLEN_DMAEN)) {
        pd->dsklen = value;
        pd->dma_active = 0;
        pd->dma_armed = 0;
        pd->armed_dsklen = 0;
        pd->countdown = 0;
        pd->dskbytr &= (uint16_t)~DSKBYTR_DMAON;

        kprintf("[DSKLEN] stop/disarm\n");
        return;
    }

    /*
     * Disk DMA start protocol:
     *
     * The Amiga disk DMA is normally started by writing DSKLEN twice with
     * DMAEN set. The first write arms, the second write starts.
     */
    if (!pd->dma_armed) {
        pd->dsklen = value;
        pd->armed_dsklen = value;
        pd->dma_armed = 1;

        kprintf("[DSKLEN] armed value=%04x\n", value);
        return;
    }

    paula_disk_start_dma(pd, value);
}

void paula_disk_step(PaulaDisk *pd, uint32_t cycles)
{
    if (!pd->dma_active) return;

    if (cycles >= pd->countdown) {
        pd->countdown = 0;
    } else {
        pd->countdown -= cycles;
    }

    if (pd->countdown != 0) return;

    uint16_t len_words = pd->dsklen & DSKLEN_LEN;

    pd->dma_active = 0;
    pd->dma_armed = 0;
    pd->armed_dsklen = 0;
    pd->dskbytr &= (uint16_t)~DSKBYTR_DMAON;
    pd->dsklen &= (uint16_t)~DSKLEN_DMAEN;

    if (!pd->write_mode) {
        pd->dskptr += ((uint32_t)len_words << 1);
    }

    kprintf("[DSKIRQ] DSKBLK fired ptr=%06x\n", pd->dskptr);
    emit_intreq(pd, PAULA_INTREQ_DSKBLK);
}