// src/chipset/paula/paula_disk.c

#include "paula_disk.h"

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

static void __attribute__((unused)) write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
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
        if (s + 1088 > dst_len) break;

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

        encode_even_odd(&sector[4],  &dst[s + 8],  4);
        encode_even_odd(&sector[8],  &dst[s + 16], 16);
        encode_even_odd(&sector[32], &dst[s + 64], 512);

        uint8_t hcheck[4] = {0, 0, 0, 0};
        for (unsigned i = 8; i < 48; i += 4) {
            hcheck[0] ^= dst[s + i + 0];
            hcheck[1] ^= dst[s + i + 1];
            hcheck[2] ^= dst[s + i + 2];
            hcheck[3] ^= dst[s + i + 3];
        }

        sector[24] = hcheck[0];
        sector[25] = hcheck[1];
        sector[26] = hcheck[2];
        sector[27] = hcheck[3];
        encode_even_odd(&sector[24], &dst[s + 48], 4);

        uint8_t dcheck[4] = {0, 0, 0, 0};
        for (unsigned i = 64; i < 1088; i += 4) {
            dcheck[0] ^= dst[s + i + 0];
            dcheck[1] ^= dst[s + i + 1];
            dcheck[2] ^= dst[s + i + 2];
            dcheck[3] ^= dst[s + i + 3];
        }

        sector[28] = dcheck[0];
        sector[29] = dcheck[1];
        sector[30] = dcheck[2];
        sector[31] = dcheck[3];
        encode_even_odd(&sector[28], &dst[s + 56], 4);

        for (int i = 8; i < 1088; i++) {
            dst[s + i] = add_clock_bits(dst[s + i - 1], dst[s + i]);
        }

        s += 1088;
    }

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

void paula_disk_write_dsklen(PaulaDisk *pd, uint16_t value)
{
    pd->dsklen = value;

    if (!(value & DSKLEN_DMAEN)) {
        pd->dma_active = 0;
        pd->dskbytr &= (uint16_t)~DSKBYTR_DMAON;
        return;
    }

    pd->write_mode = (value & DSKLEN_WRITE) ? 1 : 0;
    pd->dma_active = 1;
    pd->countdown = FLOPPY_FAKE_DMA_CYCLES;
    pd->dskbytr |= DSKBYTR_DMAON;

    uint16_t len_words = value & DSKLEN_LEN;
    uint32_t len_bytes = (uint32_t)len_words << 1;

    if (!pd->write_mode && pd->drive && pd->drive->adf) {
        int cyl = pd->drive->cylinder;
        int side = pd->drive->side ? 1 : 0;

        uint32_t adf_offset = (uint32_t)(((cyl << 1) | side) * ADF_TRACK_BYTES);

        if (valid_chip_range(pd, pd->dskptr, len_bytes)) {
            encode_adf_track_to_mfm(
                pd->drive->adf + adf_offset,
                cyl,
                side,
                &pd->chipram[pd->dskptr],
                len_bytes
            );
        }

        if (pd->adkcon & ADKCON_WORDSYNC) {
            pd->dskbytr |= DSKBYTR_WORDSYNC;
            emit_intreq(pd, PAULA_INTREQ_DSKSYNC);
        }
    }
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
    pd->dskbytr &= (uint16_t)~DSKBYTR_DMAON;
    pd->dsklen &= (uint16_t)~DSKLEN_DMAEN;

    if (!pd->write_mode) {
        pd->dskptr += ((uint32_t)len_words << 1);
    }

    emit_intreq(pd, PAULA_INTREQ_DSKBLK);
}