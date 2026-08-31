/*
 *  BCM2835 HDMI Audio hardware access for Raspberry Pi
 *
 *  Configures the HDMI MAI (Multi-channel Audio Interconnect) for
 *  audio output via HDMI, using DMA to feed IEC958 subframes
 *  to the MAI DATA FIFO.
 *
 *  The MAI passes IEC958 subframes through with minimal processing.
 *  Software must provide fully formatted subframes (audio data,
 *  validity, user data, channel status, parity).
 *
 *  Register offsets are for the BCM2835 HD/HDMI core register blocks.
 *  No VCHIQ firmware interaction needed — HDMI link is already
 *  established by the VideoCore firmware at boot.
 */

#include <config.h>

#include <exec/types.h>
#include <aros/macros.h>

#define DEBUG 0
#include <aros/debug.h>
#include <aros/macros.h>

#include <proto/exec.h>
#include <proto/mbox.h>

/* videocore.h builds VCMB_BASE from ARM_PERIIOBASE, and this port has no
 * compile-time peripheral base -- it is per-controller, in dd->periiobase.
 * Define it to nothing and address the mailbox explicitly below. */
#define ARM_PERIIOBASE 0
#include <hardware/videocore.h>
#undef ARM_PERIIOBASE

#include "DriverData.h"
#include "rpihdmi-hwaccess.h"
#include "rpihdmi-iec958.h"

#define RPIHDMI_CHANNEL_MASK 0x03

/*
 * Microsecond delay using a busy loop on the system timer.
 */
static void udelay(IPTR peribase, ULONG us)
{
    volatile ULONG *clo = (volatile ULONG *) (peribase + 0x003004);
    ULONG start = AROS_LE2LONG(*clo);

    while ((AROS_LE2LONG(*clo) - start) < us)
        ;
}

/*
 * Map sample rate to MAI format enum value.
 */
static ULONG srate_to_mai_enum(ULONG samplerate)
{
    switch (samplerate) {
    case 8000:
        return SRATE_8000;
    case 11025:
        return SRATE_11025;
    case 12000:
        return SRATE_12000;
    case 16000:
        return SRATE_16000;
    case 22050:
        return SRATE_22050;
    case 24000:
        return SRATE_24000;
    case 32000:
        return SRATE_32000;
    case 44100:
        return SRATE_44100;
    case 48000:
        return SRATE_48000;
    case 88200:
        return SRATE_88200;
    case 96000:
        return SRATE_96000;
    case 176400:
        return SRATE_176400;
    case 192000:
        return SRATE_192000;
    default:
        return SRATE_48000;
    }
}

/*
 * Get N value for HDMI audio clock recovery.
 * Standard N values from HDMI spec Table 7-1/7-2/7-3.
 */
static ULONG srate_to_n(ULONG samplerate)
{
    switch (samplerate) {
    case 32000:
        return 4096;
    case 44100:
        return 6272;
    case 48000:
        return 6144;
    case 88200:
        return 12544;
    case 96000:
        return 12288;
    case 176400:
        return 25088;
    case 192000:
        return 24576;
    default:
        return 128 * samplerate / 1000;
    }
}

/******************************************************************************
** HDMI Audio InfoFrame *******************************************************
******************************************************************************/

/*
 * Write a minimal HDMI Audio InfoFrame to RAM packet memory.
 * The infoframe tells the TV: 2-channel LPCM, sample rate, 16-bit.
 *
 * Audio InfoFrame (CEA-861):
 *   Type = 0x84, Version = 1, Length = 10
 *   Byte 1: CC=1 (2ch), CT=1 (PCM)
 *   Byte 2: SS=1 (16-bit), SF (sample rate code)
 *   Bytes 3-10: 0
 *
 * RAM packet layout: each packet slot is 0x24 bytes.
 * Audio InfoFrame is type 0x84, packet_id = type - 0x80 = 4.
 * Slot offset = 0x400 + packet_id * 0x24 = 0x490.
 */
static UBYTE srate_to_cea_sf(ULONG samplerate)
{
    switch (samplerate) {
    case 32000:
        return 1;
    case 44100:
        return 2;
    case 48000:
        return 3;
    case 88200:
        return 4;
    case 96000:
        return 5;
    case 176400:
        return 6;
    case 192000:
        return 7;
    default:
        return 0; /* refer to stream header */
    }
}

static void hdmi_write_audio_infoframe(struct RPiHDMIData *dd)
{
    ULONG slot_base = HDMI_RAM_PKT_START(dd) + 4 * 0x24;
    UBYTE infoframe[14];
    UBYTE checksum;
    int i;

    /* Header */
    infoframe[0] = 0x84; /* Audio InfoFrame type */
    infoframe[1] = 0x01; /* Version 1 */
    infoframe[2] = 0x0A; /* Length = 10 */

    /* Data bytes */
    infoframe[3] = 0x00;                                      /* Checksum (computed below) */
    infoframe[4] = 0x11;                                      /* CC=1 (2ch), CT=1 (L-PCM) */
    infoframe[5] = (srate_to_cea_sf(dd->samplerate) << 2) | 0x01; /* SF | SS=16bit */
    infoframe[6] = 0x00;                                      /* Format dependent */
    infoframe[7] = 0x00;                                      /* CA = 0 (FL/FR) */
    infoframe[8] = 0x00;                                      /* DM_INH=0, LSV=0 */
    for (i = 9; i < 14; i++)
        infoframe[i] = 0;

    /* Compute checksum: sum of all bytes must be 0 */
    checksum = 0;
    for (i = 0; i < 14; i++)
        if (i != 3)
            checksum += infoframe[i];
    infoframe[3] = (UBYTE) (0x100 - checksum);

    /*
     * Write to RAM packet memory.
     * Each RAM packet slot holds data packed as 32-bit LE words.
     * Header (3 bytes) + checksum + data bytes (10 bytes) = 14 bytes.
     * Written as ULONG words, low byte first.
     */
    for (i = 0; i < 14; i += 4) {
        ULONG word = 0;
        int j;
        for (j = 0; j < 4 && (i + j) < 14; j++)
            word |= ((ULONG) infoframe[i + j]) << (j * 8);
        wr32le(slot_base + i, word);
    }

    /* Enable the audio infoframe packet (bit 4 = packet_id 4) */
    wr32le(HDMI_RAM_PKT_CFG(dd), rd32le(HDMI_RAM_PKT_CFG(dd)) | (1 << 4));
}


/******************************************************************************
** HDMI MAI setup *************************************************************
******************************************************************************/

/*
 * Initialize the HDMI MAI for audio output.
 *
 * Programming sequence verified against working bare-metal
 * implementations.
 *
 * Key points:
 * - Reset sequence: RESET, ERRORF, FLUSH (separate writes)
 * - Software provides complete IEC958 subframes, including parity
 * - BIT_REVERSE | FORMAT_REVERSE required for correct serialization
 * - Channel map: 3-bit fields at bits 0-2 (ch0) and 4-6 (ch1)
 * - MAI_SMP: N = hsm_clock / samplerate, M = 0
 */
/*
 * Ask the firmware what the pixel clock actually is.
 *
 * CTS depends on it, and everything else here either assumes a mode
 * (1080p60) or trusts the HDMI block to derive it. The firmware programmed
 * the mode, so it is the one party that knows -- VCTAG_GETCLKRATE with
 * VCCLOCK_PIXEL is a direct question with a direct answer, and it stays right
 * when the resolution changes.
 *
 * Returns the rate in Hz, or 0 if the mailbox is unavailable or answers
 * nothing, which leaves the caller's other paths intact.
 */
/*
 * Single-tag VideoCore property transaction.
 *
 * This replaces a version that got both halves of the contract wrong, and it
 * is worth naming them because neither fails loudly:
 *
 * - the message lived on the stack. The mailbox register carries the buffer
 *   address in bits 31:4 and the channel in 3:0, so an address that is not
 *   16-byte aligned has its low nibble read as the channel and the firmware
 *   writes its 32-byte reply *somewhere else*. On a stack buffer that is a
 *   silent write into whatever is nearby, and the caller merely sees a reply
 *   address that does not match and gives up.
 * - it split the transaction into MBoxWrite + MBoxRead. Between the two, a
 *   concurrent mailbox user takes the reply.
 *
 * The pwmaudio driver in this tree already had this right; this is the same
 * mechanism, and the same reasoning applies word for word: MBoxCall is atomic,
 * and the buffer is confined to one 64-byte cache line so the reply invalidate
 * cannot discard a dirty neighbour on the heap.
 */
#define HDMI_FW_MSG_BYTES 64 /* one full cache line, >= 6+3 message words */

static BOOL hdmi_fw_property(struct RPiHDMIData *dd, ULONG tag, ULONG *vals, ULONG nvals)
{
    struct DriverBase *AHIsubBase = (struct DriverBase *) dd->ahisubbase;
    APTR mbox = (APTR) (dd->periiobase + VCMB_OFFSET);
    ULONG msgwords = 6 + nvals; /* header 2, tag header 3, values, end tag */
    ULONG allocsz = HDMI_FW_MSG_BYTES + 63;
    ULONG *raw, *m;
    ULONG i;
    BOOL ok = FALSE;
    APTR MBoxBase;

    (void) AHIsubBase;

    if ((MBoxBase = OpenResource("mbox.resource")) == NULL)
        return FALSE;
    if (msgwords * 4 > HDMI_FW_MSG_BYTES)
        return FALSE;

    raw = AllocMem(allocsz, MEMF_PUBLIC | MEMF_CLEAR);
    if (raw == NULL)
        return FALSE;

    m = (ULONG *) (((IPTR) raw + 63) & ~63);

    m[0] = AROS_LONG2LE(msgwords * 4);
    m[1] = AROS_LONG2LE(VCTAG_REQ);
    m[2] = AROS_LONG2LE(tag);
    m[3] = AROS_LONG2LE(nvals * 4);
    m[4] = AROS_LONG2LE(nvals * 4);
    for (i = 0; i < nvals; i++)
        m[5 + i] = AROS_LONG2LE(vals[i]);
    m[5 + nvals] = 0; /* end tag */

    /*
     * Require our buffer back, a success response code and the tag-processed
     * bit -- an error reply leaves the request values in place, spoofing a
     * result.
     */
    if ((APTR) MBoxCall(mbox, VCMB_PROPCHAN, m) == (APTR) m
        && AROS_LE2LONG(m[1]) == VCTAG_RESP
        && (AROS_LE2LONG(m[4]) & VCTAG_RESP)) {
        for (i = 0; i < nvals; i++)
            vals[i] = AROS_LE2LONG(m[5 + i]);
        ok = TRUE;
    }

    FreeMem(raw, allocsz);
    return ok;
}

/* Pixel clock in Hz, or 0 if the firmware would not say. */
static ULONG hdmi_pixel_clock_hz(struct RPiHDMIData *dd)
{
    ULONG vals[2];

    vals[0] = VCCLOCK_PIXEL;
    vals[1] = 0;
    if (!hdmi_fw_property(dd, VCTAG_GETCLKRATE, vals, 2))
        return 0;

    return vals[1];
}

void hdmi_mai_init(struct RPiHDMIData *dd)
{
    struct DriverBase *AHIsubBase =
        (struct DriverBase *) dd->ahisubbase;

    ULONG pb = dd->periiobase;
    ULONG srate_enum = srate_to_mai_enum(dd->samplerate);
    ULONG n_value = srate_to_n(dd->samplerate);

    /*
     * Bring-up instrumentation.
     *
     * PWM output works on this board and HDMI does not, with driver code that
     * is byte-for-byte the aarch64 original -- so what differs is the state
     * this driver is handed, not what it does with it. Every value it decides
     * from is printed here, because none of them were observable and a fix
     * chosen without them would be a guess.
     */
    bug("[hdmiaudio] init: periiobase=0x%08lx mai=0x%08lx hdmi=0x%08lx\n",
        (ULONG)pb, (ULONG)dd->soc->mai_base, (ULONG)dd->soc->hdmi_base);
    bug("[hdmiaudio] init: rate=%lu enum=%lu N=%lu hsm_clock=%lu\n",
        (ULONG)dd->samplerate, (ULONG)srate_enum, (ULONG)n_value,
        (ULONG)dd->soc->hsm_clock);

    /*
     * Reset MAI.
     * Three separate writes: RESET, then clear ERRORF, then FLUSH.
     * This resets the internal channel counter and FIFO state.
     */
    wr32le(HDMI_MAI_CTL(dd), MAI_CTL_RESET);
    udelay(pb, 100);
    wr32le(HDMI_MAI_CTL(dd), MAI_CTL_ERRORF);
    wr32le(HDMI_MAI_CTL(dd), MAI_CTL_FLUSH);
    udelay(pb, 100);

    /* Set audio format: PCM at bits 23:16, sample rate at bits 15:8 */
    wr32le(HDMI_MAI_FMT(dd), MAI_FMT_FORMAT_PCM | MAI_FMT_RATE(srate_enum));

    /*
     * FIFO thresholds.
     */
    /*
     * FIFO thresholds, also taken from the driver that worked.
     *
     * The SoC table asks for 0x10 in all four fields; the legacy driver wrote
     * 0x08080608 and produced sound. These are the levels at which the block
     * raises DREQ and PANIC, so a threshold the FIFO never reaches is a DREQ
     * that never fires -- and a DMA channel that sits ACTIVE waiting for one,
     * which is what the PWM path was observed doing.
     */
    wr32le(HDMI_MAI_THR(dd), MAI_THR_PROVEN);

    /* MAI_CONFIG: temporarily test one MAI channel at a time. */
    wr32le(HDMI_MAI_CONFIG(dd),
           MAI_CONFIG_BIT_REVERSE |
           MAI_CONFIG_FORMAT_REVERSE |
           MAI_CONFIG_CHANNEL_MASK(RPIHDMI_CHANNEL_MASK));

    /* Channel map for stereo: bcm283x :3-bit fields at bits 2:0 (ch0) and 6:4 (ch1)
     *                         bcm2711 :4-bit fields
     */
    wr32le(HDMI_MAI_CHANNEL_MAP(dd), dd->soc->hdmi_mai_channel_map);

    /*
     * Audio packet config:
     *   B_FRAME_IDENTIFIER = 0x8 at bits [13:10]
     *   CEA channel mask = 0x03 (channels 0 and 1 active)
     *   ZERO_DATA_ON_INACTIVE_CHANNELS
     *   ZERO_DATA_ON_SAMPLE_FLAT
     */
    wr32le(HDMI_AUDIO_PKT_CFG(dd),
           AUDIO_PKT_ZERO_DATA_ON_FLAT | AUDIO_PKT_ZERO_DATA_ON_INACTIVE | AUDIO_PKT_B_FRAME_ID(0x8) |
               AUDIO_PKT_CEA_MASK(0x03));

    D(bug("[RPiHDMI] channel: MAP=%08lx CONFIG=%08lx AUDIO=%08lx\n",
        rd32le(HDMI_MAI_CHANNEL_MAP(dd)),
        rd32le(HDMI_MAI_CONFIG(dd)),
        rd32le(HDMI_AUDIO_PKT_CFG(dd))));
    /*
     * Sample rate clock divider, anchored on a value proven on this silicon.
     *
     * N/(M+1) is the ratio between the HSM clock and the sample rate, and the
     * obvious spelling -- hsm_clock/samplerate with M=0 -- gives the wrong
     * answer here, because `hsm_clock` in the SoC table is wrong for this
     * board. Bellatrix's earlier bare-metal HDMI audio driver (legacy branch,
     * src/host/raspi3/hdmi_audio.c), which did produce sound on this exact
     * hardware, writes 0x0DCD21F3 at 48 kHz. That decodes to N=904481,
     * M=243, a ratio of 3722.14 -- implying an HSM clock near 178.66 MHz, not
     * the 163.68 MHz the table claims. The same driver notes it could never
     * read the HSM rate back (it returns 0), which is presumably why the
     * table's figure was a guess in the first place.
     *
     * So rather than recompute from a number we cannot trust, scale the
     * proven one: keep M and derive N from the 48 kHz value. That reproduces
     * 0x0DCD21F3 exactly at 48 kHz and stays on the same clock elsewhere.
     * Multiplying by 480 and dividing by samplerate/100 keeps every
     * intermediate inside 32 bits and avoids the integer-division error that
     * a /1000 would introduce at 44.1 kHz.
     */
    {
        ULONG n = (MAI_SMP_N_48K * 480UL) / (dd->samplerate / 100);

        wr32le(HDMI_MAI_SMP(dd), (n << 8) | MAI_SMP_M);
        bug("[hdmiaudio] init: MAI_SMP N=%lu M=%lu -> %08lx\n",
            n, (ULONG)MAI_SMP_M, (n << 8) | MAI_SMP_M);
    }

    /*
     * CTS/N audio clock recovery.
     * N is set from the HDMI spec standard values.
     * CTS is set to external mode — the hardware derives CTS from
     * the pixel clock automatically.
     * CTS = (pixel_clock * N) / (128 * samplerate)
     * RPi 3B+ default pixel clock ≈ 148500 kHz (1080p60) or 74250 kHz (720p60).
     * We read CTS_0 first to get the hardware-derived value, then write it back.
     */
    /*
     * EXTERNAL_CTS_EN stays on, and that is what makes this survive a mode
     * change.
     *
     * It does not mean "ignore the hardware and use what software wrote". The
     * block *measures* CTS against the live pixel clock and treats the written
     * value as a seed -- which is why a fixed seed stays valid at any
     * resolution. Bellatrix's own earlier HDMI audio driver says so in as many
     * words: it "hardcodes N/CTS/SMP and relies on the block's external CTS
     * measurement (EXTERNAL_CTS_EN), which makes a fixed CTS seed valid
     * regardless of the actual pixel/HSM clock".
     *
     * This was briefly cleared here on the theory that the bit blocked
     * derivation. It does the opposite: clearing it removes the very
     * mechanism that tracks the pixel clock.
     */
    wr32le(HDMI_CRP_CFG(dd), CRP_CFG_EXTERNAL_CTS_EN | CRP_CFG_N(n_value));

    {
        ULONG cts = rd32le(HDMI_CTS_0(dd));
        ULONG hw_cts = cts;


        if (cts == 0) {
            /*
             * Fallback: CTS for a 148500 kHz pixel clock (1080p60).
             *
             * CTS = (pixel_clock * N) / (128 * samplerate), and the obvious
             * spelling of that overflows 32 bits, so the original divided the
             * sample rate by 1000 first -- which is integer division: 44100
             * became 44, not 44.1. The denominator came out 5632 instead of
             * 5644.8 and CTS 165375 instead of the standard's 165000, 0.23%
             * high. That is enough for the audio clock to drift against the
             * video clock, and a sink that cannot lock them discards the audio
             * packets while the picture stays perfect -- which is exactly the
             * symptom this driver had on hardware.
             *
             * Divide in an order that stays exact and inside 32 bits: N is
             * always a multiple of 128 (6272 for 44.1 kHz, 6144 for 48), and
             * every standard rate is a multiple of 100, so scaling by 10/100
             * clears the fraction without ever exceeding ~73 million.
             *
             *   148500 * (6272/128) * 10 / (44100/100)
             *     = 148500 * 49 * 10 / 441 = 165000
             */
            ULONG pixel_hz = hdmi_pixel_clock_hz(dd);
            ULONG pixel_khz = pixel_hz ? (pixel_hz / 1000) : 148500UL;

            cts = (pixel_khz * (n_value / 128) * 10UL)
                / (dd->samplerate / 100);
            bug("[hdmiaudio] init: pixel clock %lu kHz (%s)\n",
                pixel_khz, pixel_hz ? "mailbox" : "assumed 1080p60");
        }
        /*
         * CTS is what locks the audio clock to the HDMI pixel clock. Get it
         * wrong and the sink discards the audio packets silently while the
         * picture stays perfect -- which is exactly the reported symptom. The
         * fallback assumes 1080p60; if the display negotiated anything else,
         * it is wrong by construction and this line is how anyone would know.
         */
        bug("[hdmiaudio] init: CTS hw=%lu used=%lu%s\n",
            hw_cts, cts,
            hw_cts ? " (already programmed -- NOT proof the block derived it;"
                     " a value we wrote earlier reads back the same way)"
                   : " (computed)");
        wr32le(HDMI_CTS_0(dd), cts);
        wr32le(HDMI_CTS_1(dd), cts);
    }

    /* Write Audio InfoFrame to RAM packet memory */
    hdmi_write_audio_infoframe(dd);

    /*
     * Enable MAI.
     * WHOLSMP + CHALIGN: L/R pairs consumed atomically.
     * Parity is already encoded in software, so leave PAREN cleared.
     * Note: DLATE/ERRORE/ERRORF are deliberately left cleared at
     * enable time.
     */
    wr32le(HDMI_MAI_CTL(dd), MAI_CTL_CHALIGN | MAI_CTL_WHOLSMP | MAI_CTL_CHNUM(2) | MAI_CTL_ENABLE);

    /* Initialize IEC958 channel status for this sample rate (separate L/R) */

    /*
     * Read back what the block actually took.
     *
     * Writing a register is not the same as the block accepting it, and on a
     * VideoCore that the firmware also owns it is entirely possible for a
     * write to land somewhere inert. These four say whether the MAI is
     * enabled, what format and thresholds it ended up with, and -- the one
     * that matters most -- whether the HDMI scheduler thinks there is an
     * active video stream to hang audio packets on. Audio into an idle
     * scheduler goes nowhere, and looks exactly like a driver bug.
     */
    bug("[hdmiaudio] armed: MAI_CTL=%08lx FMT=%08lx THR=%08lx SMP=%08lx\n",
        rd32le(HDMI_MAI_CTL(dd)), rd32le(HDMI_MAI_FMT(dd)),
        rd32le(HDMI_MAI_THR(dd)), rd32le(HDMI_MAI_SMP(dd)));
    bug("[hdmiaudio] armed: SCHED=%08lx RAMPKT_CFG=%08lx RAMPKT_STA=%08lx"
        " CRP=%08lx\n",
        rd32le(HDMI_SCHEDULER_CONTROL(dd)), rd32le(HDMI_RAM_PACKET_CFG(dd)),
        rd32le(HDMI_RAM_PACKET_STATUS(dd)), rd32le(HDMI_CRP_CFG(dd)));
    bug("[hdmiaudio] armed: AUDIO_PKT_CFG=%08lx\n",
        rd32le(HDMI_AUDIO_PACKET_CFG(dd)));
}

/*
 * Stop the HDMI MAI audio output.
 */
void hdmi_mai_stop(struct RPiHDMIData *dd)
{
    wr32le(HDMI_MAI_CTL(dd), MAI_CTL_FLUSH | MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF);

    udelay(dd->periiobase, 100);
}
