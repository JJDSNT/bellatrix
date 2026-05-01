// src/chipset/paula/paula_disk.c

#include "paula_disk.h"

#include "support.h"
#include <string.h>

#define DSKLEN_DMAEN 0x8000u
#define DSKLEN_WRITE 0x4000u
#define DSKLEN_LEN 0x3FFFu

#define DSKBYTR_DMAON 0x2000u
#define DSKBYTR_WORDSYNC 0x0800u

#define ADKCON_SETCLR 0x8000u
#define ADKCON_WORDSYNC 0x0400u

#define FLOPPY_FAKE_DMA_CYCLES 46000u
#define ADF_TRACK_BYTES 5632u
#define AMIGA_SECTORS_TRACK 11u
#define AMIGA_SECTOR_BYTES 512u
#define MFM_SECTOR_BYTES 1088u
#define MFM_TRACK_BYTES_PAL 12668u

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

static void put_u32be(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >>  8);
    dst[3] = (uint8_t)v;
}

static uint32_t get_u32be(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8)  |
           (uint32_t)src[3];
}

static uint16_t get_u16be(const uint8_t *src)
{
    return ((uint16_t)src[0] << 8) | (uint16_t)src[1];
}

static void mfmcode_u32(uint32_t *mfm, uint32_t longs)
{
    uint32_t prev = mfm[-1];

    while (longs-- > 0)
    {
        uint32_t v = *mfm & 0x55555555u;
        uint32_t mask1 = (prev << 31) | (v >> 1);
        uint32_t mask2 = v << 1;
        uint32_t mfmbits = ((~mask1) & (~mask2)) & 0xAAAAAAAAu;

        prev = v;
        v |= mfmbits;
        *mfm++ = v;
    }
}

static int mfm_track_contains_sync(const uint8_t *src, uint32_t len, uint16_t sync)
{
    if (!src || len < 2)
        return 0;

    for (uint32_t i = 0; i + 1 < len; i += 2)
    {
        if (get_u16be(src + i) == sync)
            return 1;
    }

    return 0;
}

static uint32_t mfm_track_find_sync_offset(const uint8_t *src, uint32_t len, uint16_t sync)
{
    if (!src || len < 2)
        return UINT32_MAX;

    for (uint32_t i = 0; i + 1 < len; i += 2)
    {
        if (get_u16be(src + i) == sync)
            return i;
    }

    return UINT32_MAX;
}

static void paula_disk_load_dskbytr(PaulaDisk *pd, uint16_t word)
{
    pd->dskbytr_data = (uint16_t)(0x8000u | (word & 0x00FFu));
}

static void paula_disk_maybe_emit_sync(PaulaDisk *pd)
{
    if (!pd->sync_seen)
        return;
    if (!(pd->adkcon & ADKCON_WORDSYNC))
        return;
    if (pd->sync_irq_fired)
        return;

    pd->sync_irq_fired = 1;
    kprintf("[DSKIRQ] DSKSYNC fired sync=%04x\n", pd->dsksync);
    emit_intreq(pd, PAULA_INTREQ_DSKSYNC);
}

static void encode_adf_track_to_mfm(
    const uint8_t *adf_track,
    int cylinder,
    int side,
    uint8_t *dst,
    uint32_t dst_len)
{
    uint8_t track = (uint8_t)((cylinder << 1) | side);
    uint32_t total_words = dst_len / 4u;
    uint32_t mfm_words[total_words];
    uint32_t *mfmbuf = mfm_words;
    uint32_t data_bytes = AMIGA_SECTORS_TRACK * MFM_SECTOR_BYTES;
    uint32_t gapsize = (dst_len > data_bytes) ? (dst_len - data_bytes) : 0u;

    memset(mfm_words, 0, sizeof(mfm_words));

    for (uint32_t i = 0; i < gapsize / 4u - 1u; i++)
        *mfmbuf++ = 0xAAAAAAAAu;

    for (uint32_t sec = 0; sec < AMIGA_SECTORS_TRACK; sec++)
    {
        uint32_t even, odd;
        uint32_t hck = 0;
        uint32_t dck = 0;
        const uint8_t *sector_data = adf_track + sec * AMIGA_SECTOR_BYTES;

        if (mfmbuf[-1] & 1u)
            mfmbuf[0] = 0x2AAAAAAAu;
        else
            mfmbuf[0] = 0xAAAAAAAAu;
        mfmbuf[1] = 0x44894489u;

        even = (0xFFu << 24) | ((uint32_t)track << 16) |
               (sec << 8) | (AMIGA_SECTORS_TRACK - sec);
        odd = (even >> 1) & 0x55555555u;
        even &= 0x55555555u;
        mfmbuf[2] = odd;
        mfmbuf[3] = even;
        hck ^= odd;
        hck ^= even;

        for (uint32_t i = 4; i < 12; i++)
            mfmbuf[i] = 0;

        for (uint32_t i = 0; i < 128; i++)
        {
            uint32_t data_long = get_u32be(sector_data + i * 4u);
            odd = (data_long >> 1) & 0x55555555u;
            even = data_long & 0x55555555u;
            mfmbuf[16 + i] = odd;
            mfmbuf[16 + 128 + i] = even;
            dck ^= odd;
            dck ^= even;
        }

        even = hck;
        odd = even >> 1;
        mfmbuf[12] = odd;
        mfmbuf[13] = even;

        even = dck;
        odd = even >> 1;
        mfmbuf[14] = odd;
        mfmbuf[15] = even;

        mfmcode_u32(mfmbuf + 2, 2u * 128u + 16u - 2u);
        mfmbuf += 2u * 128u + 16u;
    }

    mfmbuf[0] = 0;
    mfmcode_u32(mfmbuf, 1u);

    for (uint32_t i = 0; i < total_words; i++)
        put_u32be(dst + i * 4u, mfm_words[i]);
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
    pd->sync_irq_fired = 0;
    kprintf("[DSKSYNC] set=%04x sync_seen=%d\n", value, pd->sync_seen);
    paula_disk_maybe_emit_sync(pd);
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

    kprintf("[ADKCON] raw=%04x now=%04x wordsync=%d\n",
            value,
            pd->adkcon,
            !!(pd->adkcon & ADKCON_WORDSYNC));

    paula_disk_maybe_emit_sync(pd);
}

uint16_t paula_disk_read_dskbytr(PaulaDisk *pd)
{
    uint16_t v = (uint16_t)(pd->dskbytr_data & 0x80FFu);

    if (pd->sync_seen)
        v |= DSKBYTR_WORDSYNC;

    if (pd->dma_active)
        v |= DSKBYTR_DMAON;

    pd->dskbytr_data &= (uint16_t)~0x8000u;

    return v;
}

static void paula_disk_start_dma(PaulaDisk *pd, uint16_t value)
{
    uint16_t len_words = value & DSKLEN_LEN;
    uint32_t len_bytes = (uint32_t)len_words << 1;

    pd->dsklen = value;
    pd->write_mode = (value & DSKLEN_WRITE) ? 1 : 0;
    pd->dma_active = 1;
    pd->dma_armed = 0;
    pd->sync_seen = 0;
    pd->sync_irq_fired = 0;
    pd->dskbytr_data = 0;
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

    {
        uint8_t track_buf[MFM_TRACK_BYTES_PAL];

        encode_adf_track_to_mfm(
            pd->drive->adf + adf_offset,
            cyl,
            side,
            track_buf,
            sizeof(track_buf));

        for (uint32_t i = 0; i < len_bytes; ++i)
            pd->chipram[pd->dskptr + i] = track_buf[i % sizeof(track_buf)];

        pd->sync_seen = mfm_track_contains_sync(track_buf, sizeof(track_buf), pd->dsksync);

        {
            uint32_t sync_off = mfm_track_find_sync_offset(track_buf, sizeof(track_buf), pd->dsksync);
            if (sync_off != UINT32_MAX && sync_off + 16u <= sizeof(track_buf))
            {
                paula_disk_load_dskbytr(pd, get_u16be(track_buf + sync_off));
                kprintf("[DSKDMA-SECTOR] sync_off=%u words=%04x %04x %04x %04x %04x %04x %04x %04x\n",
                        sync_off,
                        get_u16be(track_buf + sync_off + 0u),
                        get_u16be(track_buf + sync_off + 2u),
                        get_u16be(track_buf + sync_off + 4u),
                        get_u16be(track_buf + sync_off + 6u),
                        get_u16be(track_buf + sync_off + 8u),
                        get_u16be(track_buf + sync_off + 10u),
                        get_u16be(track_buf + sync_off + 12u),
                        get_u16be(track_buf + sync_off + 14u));
            }
        }
    }

    pd->drive->disk_changed = 0;

    kprintf("[DSKDMA] encoded cyl=%d side=%d adf_offset=%u -> chip=%06x len=%u\n",
            cyl,
            side,
            adf_offset,
            pd->dskptr,
            len_bytes);
    kprintf("[DSKDMA] sync word=%04x found=%d adkcon=%04x head=%04x %04x %04x %04x\n",
            pd->dsksync,
            pd->sync_seen,
            pd->adkcon,
            get_u16be(&pd->chipram[pd->dskptr + 0u]),
            get_u16be(&pd->chipram[pd->dskptr + 2u]),
            get_u16be(&pd->chipram[pd->dskptr + 4u]),
            get_u16be(&pd->chipram[pd->dskptr + 6u]));

    paula_disk_maybe_emit_sync(pd);
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
        pd->sync_seen = 0;
        pd->sync_irq_fired = 0;
        pd->dskbytr_data = 0;
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
    pd->sync_seen = 0;
    pd->sync_irq_fired = 0;
    pd->dskbytr_data &= (uint16_t)~0x8000u;
    pd->dskbytr &= (uint16_t)~DSKBYTR_DMAON;
    pd->dsklen &= (uint16_t)~DSKLEN_DMAEN;

    if (!pd->write_mode)
    {
        pd->dskptr += ((uint32_t)len_words << 1);
    }

    kprintf("[DSKIRQ] DSKBLK fired ptr=%06x\n", pd->dskptr);
    emit_intreq(pd, PAULA_INTREQ_DSKBLK);
}
