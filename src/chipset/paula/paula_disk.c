// src/chipset/paula/paula_disk.c

#include "paula_disk.h"

#include "support.h"
#include <string.h>

#define DSKLEN_DMAEN 0x8000u
#define DSKLEN_WRITE 0x4000u
#define DSKLEN_LEN 0x3FFFu

#define DSKBYTR_DMAON 0x2000u
#define DSKBYTR_WORDSYNC 0x1000u

#define ADKCON_SETCLR 0x8000u
#define ADKCON_WORDSYNC 0x0400u

#define FLOPPY_FAKE_DMA_CYCLES 46000u
#define ADF_TRACK_BYTES 5632u
#define AMIGA_SECTORS_TRACK 11u
#define AMIGA_SECTOR_BYTES 512u
#define MFM_SECTOR_BYTES 1088u

static void emit_intreq(PaulaDisk *pd, uint16_t bits)
{
    if (pd->intreq_cb)
    {
        pd->intreq_cb(pd->intreq_user, bits);
    }
}

static int valid_chip_range(PaulaDisk *pd, uint32_t addr, uint32_t len)
{
    if (!pd->chipram)
        return 0;
    if (addr >= pd->chipram_size)
        return 0;
    if (len > pd->chipram_size - addr)
        return 0;
    return 1;
}

static int valid_adf_track(PaulaDisk *pd, uint32_t adf_offset)
{
    if (!pd->drive)
        return 0;
    if (!pd->drive->adf)
        return 0;
    if (!pd->drive->disk_inserted)
        return 0;
    if (pd->drive->adf_size == 0)
        return 0;
    if (adf_offset >= pd->drive->adf_size)
        return 0;
    if (ADF_TRACK_BYTES > pd->drive->adf_size - adf_offset)
        return 0;
    return 1;
}

static void encode_even_odd(const uint8_t *src, uint8_t *dst, int size)
{
    for (int i = 0; i < size; i++)
    {
        dst[i] = (src[i] >> 1) & 0x55;
        dst[i + size] = src[i] & 0x55;
    }
}

static uint32_t mfm_checksum(const uint8_t *data, int bytes)
{
    uint32_t chk = 0;
    for (int i = 0; i < bytes; i += 4)
    {
        uint32_t w = ((uint32_t)data[i + 0] << 24) |
                     ((uint32_t)data[i + 1] << 16) |
                     ((uint32_t)data[i + 2] <<  8) |
                      (uint32_t)data[i + 3];
        chk ^= w;
    }
    return chk;
}

static void put_u32be(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >>  8);
    dst[3] = (uint8_t)v;
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
    uint32_t dst_len)
{
    uint32_t s = 0;

    memset(dst, 0, dst_len);

    for (unsigned int sec = 0; sec < AMIGA_SECTORS_TRACK; sec++)
    {
        if (s + MFM_SECTOR_BYTES > dst_len)
            break;

        const uint8_t *sector_data = adf_track + sec * AMIGA_SECTOR_BYTES;

        uint8_t header[4] = {
            0xFF,
            (uint8_t)((cylinder << 1) | side),
            (uint8_t)sec,
            (uint8_t)(AMIGA_SECTORS_TRACK - sec)
        };
        uint8_t label[16];
        memset(label, 0, sizeof(label));

        /* Preamble + sync words */
        dst[s + 0] = 0xAA;
        dst[s + 1] = 0xAA;
        dst[s + 2] = 0xAA;
        dst[s + 3] = 0xAA;
        dst[s + 4] = 0x44;
        dst[s + 5] = 0x89;
        dst[s + 6] = 0x44;
        dst[s + 7] = 0x89;

        /* Header info, label, data (even/odd encoded, no clock bits yet) */
        encode_even_odd(header,      &dst[s +   8], 4);
        encode_even_odd(label,       &dst[s +  16], 16);
        encode_even_odd(sector_data, &dst[s +  64], 512);

        /*
         * AmigaDOS checksum: XOR of raw (pre-encode) long words in each region.
         * The decoder reconstitutes raw bytes from even/odd halves, then XORs
         * as big-endian 32-bit words. Checksum stored as normal even/odd field.
         */
        {
            uint8_t hchk_b[4];
            /* header + label (label is all-zero, contributes 0) */
            put_u32be(hchk_b, mfm_checksum(header, 4));
            encode_even_odd(hchk_b, &dst[s + 48], 4);
        }
        {
            uint8_t dchk_b[4];
            put_u32be(dchk_b, mfm_checksum(sector_data, AMIGA_SECTOR_BYTES));
            encode_even_odd(dchk_b, &dst[s + 56], 4);
        }

        /* Add clock bits to all bytes after the sync words */
        for (int i = 8; i < (int)MFM_SECTOR_BYTES; i++)
            dst[s + i] = add_clock_bits(dst[s + i - 1], dst[s + i]);

        s += MFM_SECTOR_BYTES;
    }

    /* Trailing gap: clock-only bytes (0xAA = MFM zeros) */
    for (uint32_t i = s; i < dst_len; i++)
        dst[i] = add_clock_bits(dst[i > 0 ? i - 1 : 0], 0);
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

    if (value & ADKCON_SETCLR)
    {
        pd->adkcon |= bits;
    }
    else
    {
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

    if (pd->write_mode)
    {
        kprintf("[DSKDMA] write mode ignored for now\n");
        return;
    }

    if (len_words == 0)
    {
        kprintf("[DSKDMA] zero length\n");
        return;
    }

    if (!pd->drive || !floppy_has_media(pd->drive))
    {
        kprintf("[DSKDMA] no media\n");
        return;
    }

    int cyl = pd->drive->cylinder;
    int side = pd->drive->side ? 1 : 0;

    uint32_t adf_offset = (uint32_t)(((cyl << 1) | side) * ADF_TRACK_BYTES);

    if (!valid_adf_track(pd, adf_offset))
    {
        kprintf("[DSKDMA] invalid ADF range cyl=%d side=%d offset=%u size=%u\n",
                cyl,
                side,
                adf_offset,
                pd->drive ? pd->drive->adf_size : 0);
        return;
    }

    if (!valid_chip_range(pd, pd->dskptr, len_bytes))
    {
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
        len_bytes);

    pd->drive->disk_changed = 0;

    kprintf("[DSKDMA] encoded cyl=%d side=%d adf_offset=%u -> chip=%06x len=%u\n",
            cyl,
            side,
            adf_offset,
            pd->dskptr,
            len_bytes);

    if (pd->adkcon & ADKCON_WORDSYNC)
    {
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

    if (!(value & DSKLEN_DMAEN))
    {
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
    if (!pd->dma_armed)
    {
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
    if (!pd->dma_active)
        return;

    if (cycles >= pd->countdown)
    {
        pd->countdown = 0;
    }
    else
    {
        pd->countdown -= cycles;
    }

    if (pd->countdown != 0)
        return;

    uint16_t len_words = pd->dsklen & DSKLEN_LEN;

    pd->dma_active = 0;
    pd->dma_armed = 0;
    pd->armed_dsklen = 0;
    pd->dskbytr &= (uint16_t)~DSKBYTR_DMAON;
    pd->dsklen &= (uint16_t)~DSKLEN_DMAEN;

    if (!pd->write_mode)
    {
        pd->dskptr += ((uint32_t)len_words << 1);
    }

    kprintf("[DSKIRQ] DSKBLK fired ptr=%06x\n", pd->dskptr);
    emit_intreq(pd, PAULA_INTREQ_DSKBLK);
}