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

    uint8_t  pending_lo;      /* low byte of last DMA-fetched word */
    bool     has_pending_lo;  /* pending_lo is valid */

    uint16_t dma_word;        /* word deposited by the DMA arbiter */
    bool     dma_word_ready;  /* dma_word contains an unread word */
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
 * DMA arbiter interface — called by Agnus, not by Paula internals.
 *
 * paula_audio_dma_fetch_addr: returns the chip RAM address the arbiter must
 * read for this channel, or UINT32_MAX if the channel needs no fetch now.
 *
 * paula_audio_dma_service_word: called by the arbiter after it has read the
 * word at that address; Paula advances current_ptr / current_length here.
 */

uint32_t paula_audio_dma_fetch_addr(const PaulaAudio *audio,
                                    int channel);

void paula_audio_dma_service_word(PaulaAudio *audio,
                                  int channel,
                                  uint16_t word);

/*
 * Audio output.
 */

int16_t paula_audio_left(const PaulaAudio *audio);
int16_t paula_audio_right(const PaulaAudio *audio);

#endif