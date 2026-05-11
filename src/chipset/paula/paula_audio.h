#ifndef BELLATRIX_PAULA_AUDIO_H
#define BELLATRIX_PAULA_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#define PAULA_AUDIO_CHANNELS 4

typedef struct PaulaAudioChannel {
    uint16_t audlen;
    uint16_t audper;
    uint16_t audvol;
    uint16_t auddat;

    uint32_t audlc;

    uint32_t current_ptr;
    uint16_t current_length;

    uint16_t period_counter;

    int16_t current_sample;

    bool dma_enabled;
    bool data_pending;
} PaulaAudioChannel;

typedef struct PaulaAudio {
    PaulaAudioChannel ch[PAULA_AUDIO_CHANNELS];

    /*
     * Mixed Paula output.
     */
    int16_t mixed_left;
    int16_t mixed_right;

    /*
     * DMA enable mask snapshot.
     */
    uint16_t dmacon;

    /*
     * Optional IRQ callback.
     */
    void *irq_opaque;

    void (*irq_raise_cb)(void *opaque,
                         uint16_t mask);
} PaulaAudio;

void paula_audio_init(PaulaAudio *audio);

void paula_audio_reset(PaulaAudio *audio);

void paula_audio_step(PaulaAudio *audio,
                      uint32_t cycles);

/*
 * Register writes.
 */

void paula_audio_write_lch(PaulaAudio *audio,
                           int channel,
                           uint16_t value);

void paula_audio_write_lcl(PaulaAudio *audio,
                           int channel,
                           uint16_t value);

void paula_audio_write_len(PaulaAudio *audio,
                           int channel,
                           uint16_t value);

void paula_audio_write_per(PaulaAudio *audio,
                           int channel,
                           uint16_t value);

void paula_audio_write_vol(PaulaAudio *audio,
                           int channel,
                           uint16_t value);

void paula_audio_write_dat(PaulaAudio *audio,
                           int channel,
                           uint16_t value);

/*
 * DMA control.
 */

void paula_audio_set_dmacon(PaulaAudio *audio,
                            uint16_t dmacon);

/*
 * Audio output.
 */

int16_t paula_audio_left(const PaulaAudio *audio);
int16_t paula_audio_right(const PaulaAudio *audio);

#endif