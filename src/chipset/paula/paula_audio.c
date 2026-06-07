#include "chipset/paula/paula_audio.h"
#include "support.h"

#include <stdint.h>
#include <string.h>

#define AUDIO_IRQ_MASK(ch) (1u << ((ch) + 7))

static bool valid_channel(int ch)
{
    return ch >= 0 &&
           ch < PAULA_AUDIO_CHANNELS;
}

static PaulaAudioChannel *audio_channel(
    PaulaAudio *audio,
    int ch)
{
    if (!audio || !valid_channel(ch)) {
        return NULL;
    }

    return &audio->ch[ch];
}

static uint32_t channel_dma_bit(int ch)
{
    return (1u << ch);
}

static bool channel_dma_enabled(
    const PaulaAudio *audio,
    int ch)
{
    return (audio->dmacon &
            channel_dma_bit(ch)) != 0;
}


static void paula_audio_mix(PaulaAudio *audio)
{
    int32_t left = 0;
    int32_t right = 0;

    /*
     * Amiga stereo:
     * ch0/ch3 -> left
     * ch1/ch2 -> right
     */

    left +=
        (audio->ch[0].current_sample *
         audio->ch[0].audvol) /
        64;

    left +=
        (audio->ch[3].current_sample *
         audio->ch[3].audvol) /
        64;

    right +=
        (audio->ch[1].current_sample *
         audio->ch[1].audvol) /
        64;

    right +=
        (audio->ch[2].current_sample *
         audio->ch[2].audvol) /
        64;

    if (left > 32767) {
        left = 32767;
    }

    if (left < -32768) {
        left = -32768;
    }

    if (right > 32767) {
        right = 32767;
    }

    if (right < -32768) {
        right = -32768;
    }

    audio->mixed_left = (int16_t)left;
    audio->mixed_right = (int16_t)right;
}

static void paula_audio_channel_irq(
    PaulaAudio *audio,
    int ch)
{
    if (!audio->irq_raise_cb) {
        return;
    }

    audio->irq_raise_cb(
        audio->irq_opaque,
        AUDIO_IRQ_MASK(ch));
}

void paula_audio_init(PaulaAudio *audio)
{
    if (!audio) {
        return;
    }

    memset(audio, 0, sizeof(*audio));

    paula_audio_reset(audio);
}

void paula_audio_reset(PaulaAudio *audio)
{
    if (!audio) {
        return;
    }

    memset(audio->ch, 0, sizeof(audio->ch));

    audio->mixed_left = 0;
    audio->mixed_right = 0;

    audio->dmacon = 0;
}

void paula_audio_step(PaulaAudio *audio,
                      uint32_t cycles)
{
    if (!audio) {
        return;
    }

    for (int ch = 0;
         ch < PAULA_AUDIO_CHANNELS;
         ch++)
    {
        PaulaAudioChannel *c =
            &audio->ch[ch];

        c->dma_enabled =
            channel_dma_enabled(audio, ch);

        if (!c->dma_enabled) {
            continue;
        }

        if (c->audper == 0) {
            c->audper = 1;
        }

        if (c->period_counter == 0) {
            c->period_counter = c->audper;
        }

        uint32_t rem = cycles;

        while (rem >= c->period_counter) {
            rem -= c->period_counter;
            c->period_counter = c->audper;

            if (c->has_pending_lo) {
                /* consume the second byte of the last arbiter-fetched word */
                c->current_sample =
                    (int16_t)(int8_t)c->pending_lo << 8;
                c->has_pending_lo = false;

            } else if (c->dma_word_ready) {
                /* consume the word deposited by the DMA arbiter */
                c->current_sample =
                    (int16_t)(int8_t)(c->dma_word >> 8) << 8;
                c->pending_lo   = (uint8_t)(c->dma_word & 0xFFu);
                c->has_pending_lo = true;
                c->dma_word_ready = false;

                /* length / reload managed at fetch time in dma_service_word */

            }
            /* else: DMA stall — arbiter hasn't delivered a word yet;
             * hold last sample until one arrives */
        }

        c->period_counter -= rem;
    }

    paula_audio_mix(audio);
}

uint32_t paula_audio_dma_fetch_addr(const PaulaAudio *audio,
                                    int channel)
{
    const PaulaAudioChannel *c;

    if (!audio || !valid_channel(channel)) {
        return UINT32_MAX;
    }

    c = &audio->ch[channel];

    /* Request a fetch only when: DMA enabled, buffer has words left,
     * and the prefetch slot is empty (no word waiting to be consumed). */
    if (!channel_dma_enabled(audio, channel) ||
        c->current_length == 0 ||
        c->dma_word_ready) {
        return UINT32_MAX;
    }

    return c->current_ptr;
}

void paula_audio_dma_service_word(PaulaAudio *audio,
                                  int channel,
                                  uint16_t word)
{
    static bool s_logged[PAULA_AUDIO_CHANNELS];

    PaulaAudioChannel *c = audio_channel(audio, channel);

    if (!c) {
        return;
    }

    if (!s_logged[channel]) {
        s_logged[channel] = true;
        kprintf("[AUD%d] first DMA fetch ptr=0x%06x word=0x%04x"
                " len=%u per=%u vol=%u\n",
                channel,
                (unsigned)c->current_ptr,
                (unsigned)word,
                (unsigned)c->current_length,
                (unsigned)c->audper,
                (unsigned)c->audvol);
    }

    c->dma_word       = word;
    c->dma_word_ready = true;

    /* Advance fetch pointer and manage the length counter here,
     * as on the real hardware the DMA fetch is what drives these. */
    c->current_ptr += 2u;

    if (c->current_length > 0) {
        c->current_length--;
    }

    if (c->current_length == 0) {
        c->current_ptr    = c->audlc;
        c->current_length = c->audlen;
        paula_audio_channel_irq(audio, channel);
    }
}

void paula_audio_write_lch(
    PaulaAudio *audio,
    int channel,
    uint16_t value)
{
    PaulaAudioChannel *c =
        audio_channel(audio, channel);

    if (!c) {
        return;
    }

    c->audlc =
        (c->audlc & 0x0000ffffu) |
        ((uint32_t)value << 16);
}

void paula_audio_write_lcl(
    PaulaAudio *audio,
    int channel,
    uint16_t value)
{
    PaulaAudioChannel *c =
        audio_channel(audio, channel);

    if (!c) {
        return;
    }

    c->audlc =
        (c->audlc & 0xffff0000u) |
        (value & 0xfffeu);
}

void paula_audio_write_len(
    PaulaAudio *audio,
    int channel,
    uint16_t value)
{
    PaulaAudioChannel *c =
        audio_channel(audio, channel);

    if (!c) {
        return;
    }

    c->audlen = value;

    if (c->current_length == 0) {
        c->current_length = value;
        c->current_ptr = c->audlc;
    }
}

void paula_audio_write_per(
    PaulaAudio *audio,
    int channel,
    uint16_t value)
{
    PaulaAudioChannel *c =
        audio_channel(audio, channel);

    if (!c) {
        return;
    }

    c->audper = value ? value : 1;
}

void paula_audio_write_vol(
    PaulaAudio *audio,
    int channel,
    uint16_t value)
{
    PaulaAudioChannel *c =
        audio_channel(audio, channel);

    if (!c) {
        return;
    }

    c->audvol = value & 0x7f;

    if (c->audvol > 64) {
        c->audvol = 64;
    }
}

void paula_audio_write_dat(
    PaulaAudio *audio,
    int channel,
    uint16_t value)
{
    PaulaAudioChannel *c =
        audio_channel(audio, channel);

    if (!c) {
        return;
    }

    c->auddat = value;
    c->data_pending = true;
}

void paula_audio_set_dmacon(
    PaulaAudio *audio,
    uint16_t dmacon)
{
    static uint16_t s_prev = 0xFFFFu;

    if (!audio) {
        return;
    }

    if (dmacon != s_prev) {
        uint16_t changed = dmacon ^ s_prev;
        if (s_prev == 0xFFFFu) changed = dmacon & 0x000Fu;

        if (changed & 0x000Fu) {
            kprintf("[AUDIO-DMA] dmacon audio bits = %04x"
                    " (ch0=%u ch1=%u ch2=%u ch3=%u)\n",
                    (unsigned)(dmacon & 0x000Fu),
                    (unsigned)(dmacon & 1u),
                    (unsigned)((dmacon >> 1) & 1u),
                    (unsigned)((dmacon >> 2) & 1u),
                    (unsigned)((dmacon >> 3) & 1u));
        }
        s_prev = dmacon;
    }

    audio->dmacon = dmacon;
}

int16_t paula_audio_left(
    const PaulaAudio *audio)
{
    if (!audio) {
        return 0;
    }

    return audio->mixed_left;
}

int16_t paula_audio_right(
    const PaulaAudio *audio)
{
    if (!audio) {
        return 0;
    }

    return audio->mixed_right;
}
