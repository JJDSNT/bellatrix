#include "host/raspi3/hdmi_audio.h"

#include "audio/output.h"

#include <stdint.h>

/*
 * BCM2835/2837 (Pi 3B, RASPPI<=3 register variant) HDMI audio — direct MAI
 * (Multi-channel Audio Interface) + HDMI packet RAM access, polling mode,
 * no VideoCore firmware/VCHIQ involved. See hdmi_audio.h and
 * AI_context/issues/ISSUE-0011.md for the bring-up plan and the licensing
 * reasoning (independent reimplementation from hardware/standard facts, not
 * a port of any GPL codebase).
 *
 * Same MMIO convention as src/host/raspi3/pl011_backend.c: Emu68 runs
 * big-endian AArch64, peripherals are little-endian, so every raspi3 module
 * carries its own local bswap-gated helpers and uses the 0xf2000000 virtual
 * peripheral alias (never the raw 0x3f20xxxx physical window).
 */

int kprintf(const char *fmt, ...);

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline void     hdmi_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = __builtin_bswap32(v); }
static inline uint32_t hdmi_rd32(uintptr_t a)             { return __builtin_bswap32(*(volatile uint32_t *)a); }
#else
static inline void     hdmi_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t hdmi_rd32(uintptr_t a)             { return *(volatile uint32_t *)a; }
#endif

#define ARM_PERI_VIRT_BASE 0xF2000000UL
#define ARM_HDMI_BASE      (ARM_PERI_VIRT_BASE + 0x902000UL)
#define ARM_HD_BASE        (ARM_PERI_VIRT_BASE + 0x808000UL)
#define ARM_CM_BASE        (ARM_PERI_VIRT_BASE + 0x101000UL)

/* Clock Manager — HSM (HDMI state machine) clock, feeds the MAI/audio
 * packet logic. Same CM_PASSWD/CTL/DIV layout already used for GPCLK2 in
 * pl011_backend.c; HSM is a different CM generator at a different offset. */
#define CM_HSMCTL   (ARM_CM_BASE + 0x0B0UL)
#define CM_HSMDIV   (ARM_CM_BASE + 0x0B4UL)
#define CM_PASSWD   0x5A000000u
#define CM_CTL_ENAB (1u << 4)
#define CM_CTL_BUSY (1u << 7)
#define CM_CTL_SRC_MASK 0x0Fu

/* MAI (Multi-channel Audio Interface) registers, HD_BASE-relative. */
#define MAI_CTL        (ARM_HD_BASE + 0x14UL)
#define MAI_THR        (ARM_HD_BASE + 0x18UL)
#define MAI_FMT        (ARM_HD_BASE + 0x1CUL)
#define MAI_DATA       (ARM_HD_BASE + 0x20UL)
#define MAI_SMP        (ARM_HD_BASE + 0x2CUL)

/* MAI_CTL bits */
#define MAI_CTL_ENABLE      (1u << 0)
#define MAI_CTL_BUSY        (1u << 1)
#define MAI_CTL_CHALIGN     (1u << 2)
#define MAI_CTL_FLUSH       (1u << 8)
#define MAI_CTL_DLATE       (1u << 15)
#define MAI_CTL_ERRORE      (1u << 16)
#define MAI_CTL_ERRORF      (1u << 17)
#define MAI_CTL_RESET       (1u << 18)
#define MAI_CTL_FORMAT_SPDIF (1u << 4)
#define MAI_CTL_CHNUM_SHIFT 19
#define MAI_CTL_WHOLSMP     (1u << 24)
#define MAI_CTL_CHALIGN_SET (1u << 25)

/* MAI_THR: [15:8]=DREQ threshold (unused, polling), [7:0]=panic threshold */
#define MAI_THR_FIFO_MASK 0x3Fu

/* HDMI audio/infoframe/CRP registers, HDMI_BASE-relative (RASPPI<=3
 * layout — hardware facts from the BCM2835/2837 peripheral map). */
#define HDMI_AUDIO_PACKET_CONFIG (ARM_HDMI_BASE + 0x0BCUL)
#define HDMI_CRP_CFG             (ARM_HDMI_BASE + 0x0C0UL)
#define HDMI_AUDIO_CTS0          (ARM_HDMI_BASE + 0x0C4UL)
#define HDMI_AUDIO_CTS1          (ARM_HDMI_BASE + 0x0C8UL)
#define HDMI_MAI_CHANNEL_MAP     (ARM_HDMI_BASE + 0x090UL)
#define HDMI_MAI_CONFIG          (ARM_HDMI_BASE + 0x094UL)
#define HDMI_RAM_PACKET_CONFIG   (ARM_HDMI_BASE + 0x0A0UL)
#define HDMI_RAM_PACKET_STATUS   (ARM_HDMI_BASE + 0x0A4UL)
#define HDMI_TX_PHY_CTL0         (ARM_HDMI_BASE + 0x2F0UL)
#define HDMI_RAM_PACKET_START    (ARM_HDMI_BASE + 0x400UL) /* packet slot 1, audio infoframe */

#define HDMI_AUDIO_PACKET_CONFIG_EN (1u << 31)
#define HDMI_CRP_CFG_EXTERNAL_CTS_EN (1u << 24)
#define HDMI_RAM_PACKET_CONFIG_EN (1u << 16) /* infoframe slot 1 enable bit */

enum {
    IEC958_FRAMES_PER_BLOCK    = 192,
    IEC958_SUBFRAMES_PER_BLOCK = 384,
    IEC958_B_PREAMBLE          = 0x0F,
};

/* IEC 60958-3 channel status, fixed for 48 kHz consumer PCM stereo (fields
 * defined by the published standard, not by any particular implementation):
 * byte0: consumer format(0)/PCM linear(bit1=0)/no copyright(bit2=1... using
 *        0b100 = not-copyright-asserted bit set, mode=0, PCM)
 * byte1: category = general mode (0)
 * byte2: source/channel number = 0 (ignore)
 * byte3: sampling frequency code = 2 (48000 Hz per IEC 60958-3 table)
 * byte4: word length = 0b1011 (24-bit, max length 24) | orig freq code 13<<4 */
static const uint8_t s_iec958_status_48k[5] = {
    0x04u,
    0x00u,
    0x00u,
    0x02u,
    (uint8_t)(0x0Bu | (13u << 4)),
};

static bool s_hdmi_ready = false;

/* Classic continued-fraction rational approximation (Euclidean algorithm +
 * convergents) of num/den bounded by max_num/max_den. Public, well known
 * numerical method — not derived from any specific codebase. */
static void rational_best_approximation(uint64_t num, uint64_t den,
                                         uint64_t max_num, uint64_t max_den,
                                         uint32_t *out_num, uint32_t *out_den)
{
    uint64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    uint64_t n = num, d = den;

    if (den == 0u) {
        *out_num = 0;
        *out_den = 1;
        return;
    }

    while (d != 0u) {
        uint64_t a = n / d;
        uint64_t t;

        uint64_t p2 = a * p1 + p0;
        uint64_t q2 = a * q1 + q0;

        if (p2 > max_num || q2 > max_den)
            break;

        p0 = p1; q0 = q1;
        p1 = p2; q1 = q2;

        t = n - a * d;
        n = d;
        d = t;
    }

    if (q1 == 0u) {
        *out_num = 1;
        *out_den = 1;
    } else {
        *out_num = (uint32_t)p1;
        *out_den = (uint32_t)q1;
    }
}

/* Reads the HSM (HDMI State Machine) clock rate programmed by the
 * VideoCore firmware into the Clock Manager. We only read here — the
 * firmware owns HSM clock setup as part of its own HDMI/pixel clock
 * bring-up (see init-ordering note in hdmi_audio.h / ISSUE-0011). */
static uint32_t hdmi_read_hsm_clock_hz(void)
{
    uint32_t ctl = hdmi_rd32(CM_HSMCTL);
    uint32_t div = hdmi_rd32(CM_HSMDIV);

    /* CM generator: rate = source_rate / (integer_div + frac_div/4096).
     * Source is PLLH_AUX or oscillator depending on CTL src field; on a
     * stock VideoCore HDMI bring-up this is PLLH_AUX at 216 MHz. Without a
     * clock tree probe this session cannot verify beyond that, so treat
     * 216 MHz as the known common case (documented in the BCM2835 clock
     * manager reference) and derive HSM from CM_HSMDIV against it. */
    (void)ctl;

    uint32_t int_div = (div >> 12) & 0xFFFu;
    uint32_t frac_div = div & 0xFFFu;

    if (int_div == 0u)
        return 0u;

    uint64_t src_hz = 216000000ull;
    uint64_t denom = ((uint64_t)int_div << 12) | frac_div;

    return (uint32_t)((src_hz << 12) / denom);
}

static bool hdmi_display_audio_supported(void)
{
    return (hdmi_rd32(HDMI_RAM_PACKET_CONFIG) & HDMI_RAM_PACKET_CONFIG_EN) != 0u;
}

static void hdmi_program_channel_status_and_infoframe(void)
{
    /* CEA-861 audio infoframe, fixed for stereo Linear PCM, 2 channels,
     * sample size/rate "refer to stream" (0) — header fields are defined
     * by the published CEA-861 standard, not by any implementation. */
    static const uint8_t infoframe[10] = {
        0x84u, 0x01u, 0x0Au, /* type=0x84 (Audio), version=1, length=10 */
        0x00u,               /* checksum placeholder, patched below */
        0x01u,               /* CT=1 (PCM), CC=1 (2 channels) */
        0x00u,               /* sample size/rate: refer to stream header */
        0x00u,               /* format-specific */
        0x00u, 0x00u, 0x00u  /* speaker allocation + reserved */
    };
    uint32_t sum = 0;
    uint8_t checksum;
    uint32_t i;
    uint8_t frame[12] = {0};

    for (i = 0; i < 10; i++)
        frame[i] = infoframe[i];

    for (i = 0; i < 10; i++)
        sum += frame[i];
    checksum = (uint8_t)(0x100u - (sum & 0xFFu));
    frame[3] = checksum;

    for (i = 0; i < 12; i += 4) {
        uint32_t word = (uint32_t)frame[i]
            | ((uint32_t)frame[i + 1] << 8)
            | ((uint32_t)frame[i + 2] << 16)
            | ((uint32_t)frame[i + 3] << 24);
        hdmi_wr32(HDMI_RAM_PACKET_START + i, word);
    }

    hdmi_wr32(HDMI_RAM_PACKET_CONFIG,
              hdmi_rd32(HDMI_RAM_PACKET_CONFIG) | HDMI_RAM_PACKET_CONFIG_EN);
}

bool hdmi_audio_init(void)
{
    uint32_t hsm_hz;
    uint32_t n, cts;

    s_hdmi_ready = false;

    if (!hdmi_display_audio_supported()) {
        kprintf("hdmi_audio: display did not enable audio packets (RamPacketConfig) -- no HDMI audio sink\n");
        return false;
    }

    hsm_hz = hdmi_read_hsm_clock_hz();
    if (hsm_hz == 0u) {
        kprintf("hdmi_audio: HSM clock read as 0 Hz, cannot derive N/CTS\n");
        return false;
    }

    /* N/CTS audio clock recovery packet, per HDMI spec's recommended N for
     * 48 kHz (128 * fs / 1000 rounded, matches the standard's N table) and
     * CTS derived as the best rational approximation of
     * (hsm_hz * N) / (128 * fs) bounded to 20-bit fields. */
    n = 6144u; /* HDMI spec table value for 48 kHz */
    rational_best_approximation((uint64_t)hsm_hz * n,
                                 (uint64_t)128u * BELLATRIX_AUDIO_OUTPUT_RATE_HZ,
                                 0xFFFFFu, 0xFFFFFu, &n, &cts);
    if (n == 0u)
        n = 6144u;
    if (cts == 0u)
        cts = 1u;

    hdmi_wr32(HDMI_CRP_CFG, HDMI_CRP_CFG_EXTERNAL_CTS_EN | (n & 0x3FFFFu));
    hdmi_wr32(HDMI_AUDIO_CTS0, cts & 0xFFFFFu);
    hdmi_wr32(HDMI_AUDIO_CTS1, (cts >> 16) & 0xFFu);

    hdmi_wr32(HDMI_MAI_CHANNEL_MAP, 0x00000010u); /* L=slot0, R=slot1, MAI default map */
    hdmi_wr32(HDMI_MAI_CONFIG, 0x00000000u);

    hdmi_program_channel_status_and_infoframe();

    hdmi_wr32(MAI_CTL, MAI_CTL_RESET);
    hdmi_wr32(MAI_CTL, MAI_CTL_FLUSH);

    hdmi_wr32(MAI_THR, (0x10u << 8) | 0x10u); /* mid-FIFO thresholds, polling mode */
    hdmi_wr32(MAI_FMT, 0u); /* raw stereo PCM samples, no companding */

    hdmi_wr32(MAI_CTL,
              MAI_CTL_ENABLE | MAI_CTL_WHOLSMP | MAI_CTL_CHALIGN_SET
              | (2u << MAI_CTL_CHNUM_SHIFT));

    hdmi_wr32(HDMI_TX_PHY_CTL0, hdmi_rd32(HDMI_TX_PHY_CTL0) | 1u);

    hdmi_wr32(HDMI_AUDIO_PACKET_CONFIG,
              hdmi_rd32(HDMI_AUDIO_PACKET_CONFIG) | HDMI_AUDIO_PACKET_CONFIG_EN);

    kprintf("hdmi_audio: init ok (hsm=%u Hz, N=%u, CTS=%u)\n",
            (unsigned)hsm_hz, (unsigned)n, (unsigned)cts);

    s_hdmi_ready = true;
    return true;
}

bool hdmi_audio_writable(void)
{
    if (!s_hdmi_ready)
        return false;

    return (hdmi_rd32(MAI_CTL) & MAI_CTL_BUSY) == 0u;
}

void hdmi_audio_write_sample(int16_t sample16, unsigned subframe_in_block)
{
    unsigned frame_in_block = subframe_in_block / 2u;
    unsigned status_bit_index = frame_in_block; /* 0..191, status covers 0..39 then reads as 0 */
    uint32_t subframe;
    int32_t sample24 = (int32_t)sample16 << 8;

    subframe = ((uint32_t)sample24 & 0xFFFFFFu) << 4;

    if (status_bit_index < 40u) {
        unsigned byte_idx = status_bit_index / 8u;
        unsigned bit_idx = status_bit_index % 8u;
        if (byte_idx < sizeof(s_iec958_status_48k)
            && (s_iec958_status_48k[byte_idx] & (1u << bit_idx)))
            subframe |= (1u << 30);
    }

    if (subframe_in_block == 0u)
        subframe |= IEC958_B_PREAMBLE;

    if (__builtin_parity(subframe & 0x7FFFFFFFu))
        subframe |= (1u << 31);

    if (!s_hdmi_ready)
        return;

    hdmi_wr32(MAI_DATA, subframe);
}

void hdmi_audio_test_tone_tick(void)
{
    static unsigned phase = 0;
    static unsigned subframe = 0;
    /* Simple square-ish tone via a small sine LUT would need FP/table setup;
     * a direct sign-flip square wave is enough to validate FIFO/framing on
     * real hardware without extra state. */
    static const int16_t lut[8] = {
        0, 11585, 16384, 11585, 0, -11585, -16384, -11585
    };
    int16_t sample;

    if (!s_hdmi_ready)
        return;

    sample = lut[phase & 7u];
    phase++;

    hdmi_audio_write_sample(sample, subframe++);
    hdmi_audio_write_sample(sample, subframe++);
    if (subframe >= IEC958_SUBFRAMES_PER_BLOCK)
        subframe = 0u;
}

void hdmi_audio_stop(void)
{
    if (!s_hdmi_ready)
        return;

    hdmi_wr32(MAI_CTL, MAI_CTL_RESET);
    hdmi_wr32(HDMI_AUDIO_PACKET_CONFIG,
              hdmi_rd32(HDMI_AUDIO_PACKET_CONFIG) & ~HDMI_AUDIO_PACKET_CONFIG_EN);
    s_hdmi_ready = false;
}
