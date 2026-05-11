#include "chipset/paula/paula_audio.h"

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

static int16_t paula_decode_sample(
    uint16_t auddat)
{
    /*
     * Simplified:
     * high byte only.
     */

    int8_t s = (int8_t)((auddat >> 8) & 0xff);

    return (int16_t)s << 8;
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

        if (c->period_counter <= cycles) {

            c->period_counter =
                c->audper;

            /*
             * Simplified:
             * replay current AUDDAT.
             */

            c->current_sample =
                paula_decode_sample(
                    c->auddat);

            if (c->current_length > 0) {
                c->current_length--;
            }

            if (c->current_length == 0) {

                /*
                 * Reload length.
                 */

                c->current_length =
                    c->audlen;

                c->current_ptr =
                    c->audlc;

                paula_audio_channel_irq(
                    audio,
                    ch);
            }

        } else {
            c->period_counter -= cycles;
        }
    }

    paula_audio_mix(audio);
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
    if (!audio) {
        return;
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