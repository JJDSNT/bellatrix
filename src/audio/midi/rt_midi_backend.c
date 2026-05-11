#include "audio/midi/rt_midi_backend.h"

#include <string.h>
#include <stdlib.h>

#if defined(BELLATRIX_HAVE_RTMIDI)

#include <rtmidi_c.h>

#endif

typedef struct RTMidiInternal {
#if defined(BELLATRIX_HAVE_RTMIDI)

    RtMidiInPtr in;
    RtMidiOutPtr out;

#endif
} RTMidiInternal;

#if defined(BELLATRIX_HAVE_RTMIDI)

static void rt_midi_rx_trampoline(
    double time_stamp,
    const unsigned char *message,
    size_t size,
    void *user_data)
{
    (void)time_stamp;

    RTMidiBackend *rtm =
        (RTMidiBackend *)user_data;

    if (!rtm || !rtm->rx_cb) {
        return;
    }

    rtm->rx_cb(
        rtm->opaque,
        message,
        (uint32_t)size);
}

#endif

static RTMidiInternal *internal_ptr(
    RTMidiBackend *rtm)
{
    return (RTMidiInternal *)rtm->opaque;
}

bool rt_midi_backend_init(
    RTMidiBackend *rtm)
{
    if (!rtm) {
        return false;
    }

    memset(rtm, 0, sizeof(*rtm));

    RTMidiInternal *internal =
        calloc(1, sizeof(*internal));

    if (!internal) {
        return false;
    }

#if defined(BELLATRIX_HAVE_RTMIDI)

    internal->in =
        rtmidi_in_create_default();

    internal->out =
        rtmidi_out_create_default();

    if (!internal->in ||
        !internal->out)
    {
        free(internal);
        return false;
    }

#endif

    rtm->opaque = internal;

    return true;
}

void rt_midi_backend_shutdown(
    RTMidiBackend *rtm)
{
    if (!rtm) {
        return;
    }

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal) {
        return;
    }

#if defined(BELLATRIX_HAVE_RTMIDI)

    if (internal->in) {
        rtmidi_in_free(internal->in);
    }

    if (internal->out) {
        rtmidi_out_free(internal->out);
    }

#endif

    free(internal);

    memset(rtm, 0, sizeof(*rtm));
}

uint32_t rt_midi_backend_input_count(
    RTMidiBackend *rtm)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal || !internal->in) {
        return 0;
    }

    return (uint32_t)
        rtmidi_get_port_count(
            internal->in);

#else
    (void)rtm;
    return 0;
#endif
}

uint32_t rt_midi_backend_output_count(
    RTMidiBackend *rtm)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal || !internal->out) {
        return 0;
    }

    return (uint32_t)
        rtmidi_get_port_count(
            internal->out);

#else
    (void)rtm;
    return 0;
#endif
}

const char *rt_midi_backend_input_name(
    RTMidiBackend *rtm,
    uint32_t port)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    static char name[256];

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal || !internal->in) {
        return NULL;
    }

    name[0] = '\0';

    rtmidi_get_port_name(
        internal->in,
        port,
        name,
        sizeof(name));

    return name;

#else
    (void)rtm;
    (void)port;
    return NULL;
#endif
}

const char *rt_midi_backend_output_name(
    RTMidiBackend *rtm,
    uint32_t port)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    static char name[256];

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal || !internal->out) {
        return NULL;
    }

    name[0] = '\0';

    rtmidi_get_port_name(
        internal->out,
        port,
        name,
        sizeof(name));

    return name;

#else
    (void)rtm;
    (void)port;
    return NULL;
#endif
}

bool rt_midi_backend_open_input(
    RTMidiBackend *rtm,
    uint32_t port)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal || !internal->in) {
        return false;
    }

    rtmidi_open_port(
        internal->in,
        port,
        "Bellatrix MIDI IN");

    rtmidi_in_set_callback(
        internal->in,
        rt_midi_rx_trampoline,
        rtm);

    rtmidi_in_ignore_types(
        internal->in,
        false,
        false,
        false);

    rtm->input_port = port;
    rtm->input_open = true;

    return true;

#else
    (void)rtm;
    (void)port;
    return false;
#endif
}

bool rt_midi_backend_open_output(
    RTMidiBackend *rtm,
    uint32_t port)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal || !internal->out) {
        return false;
    }

    rtmidi_open_port(
        internal->out,
        port,
        "Bellatrix MIDI OUT");

    rtm->output_port = port;
    rtm->output_open = true;

    return true;

#else
    (void)rtm;
    (void)port;
    return false;
#endif
}

void rt_midi_backend_close_input(
    RTMidiBackend *rtm)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal || !internal->in) {
        return;
    }

    rtmidi_close_port(
        internal->in);

#endif

    rtm->input_open = false;
}

void rt_midi_backend_close_output(
    RTMidiBackend *rtm)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal || !internal->out) {
        return;
    }

    rtmidi_close_port(
        internal->out);

#endif

    rtm->output_open = false;
}

bool rt_midi_backend_send(
    RTMidiBackend *rtm,
    const uint8_t *data,
    uint32_t size)
{
#if defined(BELLATRIX_HAVE_RTMIDI)

    RTMidiInternal *internal =
        internal_ptr(rtm);

    if (!internal ||
        !internal->out ||
        !data ||
        !size)
    {
        return false;
    }

    int rc =
        rtmidi_out_send_message(
            internal->out,
            data,
            size);

    return rc == 0;

#else
    (void)rtm;
    (void)data;
    (void)size;
    return false;
#endif
}

void rt_midi_backend_set_rx_callback(
    RTMidiBackend *rtm,
    rt_midi_rx_cb cb,
    void *opaque)
{
    if (!rtm) {
        return;
    }

    rtm->rx_cb = cb;
    rtm->opaque = opaque;
}