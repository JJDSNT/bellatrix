#ifndef BELLATRIX_AUDIO_MIDI_RT_MIDI_BACKEND_H
#define BELLATRIX_AUDIO_MIDI_RT_MIDI_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

typedef struct RTMidiBackend RTMidiBackend;

typedef void (*rt_midi_rx_cb)(
    void *opaque,
    const uint8_t *data,
    uint32_t size);

struct RTMidiBackend {
    void *opaque;

    /*
     * Opaque RtMidi handles.
     */
    void *midi_in;
    void *midi_out;

    /*
     * Current ports.
     */
    uint32_t input_port;
    uint32_t output_port;

    bool input_open;
    bool output_open;

    /*
     * Receive callback.
     */
    rt_midi_rx_cb rx_cb;
};

bool rt_midi_backend_init(
    RTMidiBackend *rtm);

void rt_midi_backend_shutdown(
    RTMidiBackend *rtm);

/*
 * Port enumeration.
 */

uint32_t rt_midi_backend_input_count(
    RTMidiBackend *rtm);

uint32_t rt_midi_backend_output_count(
    RTMidiBackend *rtm);

const char *rt_midi_backend_input_name(
    RTMidiBackend *rtm,
    uint32_t port);

const char *rt_midi_backend_output_name(
    RTMidiBackend *rtm,
    uint32_t port);

/*
 * Open/close ports.
 */

bool rt_midi_backend_open_input(
    RTMidiBackend *rtm,
    uint32_t port);

bool rt_midi_backend_open_output(
    RTMidiBackend *rtm,
    uint32_t port);

void rt_midi_backend_close_input(
    RTMidiBackend *rtm);

void rt_midi_backend_close_output(
    RTMidiBackend *rtm);

/*
 * Send MIDI message.
 */

bool rt_midi_backend_send(
    RTMidiBackend *rtm,
    const uint8_t *data,
    uint32_t size);

/*
 * Receive callback.
 */

void rt_midi_backend_set_rx_callback(
    RTMidiBackend *rtm,
    rt_midi_rx_cb cb,
    void *opaque);

#endif