#include "host/raspi3/hdmi_audio.h"

#include "audio/output.h"

#include "support.h" /* arm_flush_cache (Emu68) */
#include "mmu.h"     /* mmu_virt2phys (Emu68) */

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

/* kprintf is declared by Emu68's support.h (included above). */

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline void     hdmi_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = __builtin_bswap32(v); }
static inline uint32_t hdmi_rd32(uintptr_t a)             { return __builtin_bswap32(*(volatile uint32_t *)a); }
#else
static inline void     hdmi_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t hdmi_rd32(uintptr_t a)             { return *(volatile uint32_t *)a; }
#endif

/* Values placed in RAM structures that the BCM DMA controller reads (control
 * blocks, sample words) must be little-endian regardless of the (big-endian)
 * CPU, since the DMA does not byte-swap. Register accesses go through
 * hdmi_wr32/rd32 which already handle the peripheral's little-endianness. */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define CPU_TO_LE32(x) __builtin_bswap32((uint32_t)(x))
#else
#define CPU_TO_LE32(x) ((uint32_t)(x))
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
/* Offsets confirmed against the BCM2835/2837 VideoCore4 HDMI block (Pi Zero W
 * .. Pi 3B, RASPPI<=3). Cross-checked with the mainline Linux vc4_hdmi_regs.h
 * and the kumaashi Pi Zero W bring-up (external/RaspberryPI); both agree on
 * this contiguous 0x09c..0x0b0 audio region — the previous 0x0BC.. values were
 * shifted and, notably, 0x0C0 collided with HDMI_SCHEDULER_CONTROL. */
#define HDMI_MAI_CHANNEL_MAP     (ARM_HDMI_BASE + 0x090UL)
#define HDMI_MAI_CONFIG          (ARM_HDMI_BASE + 0x094UL)
#define HDMI_MAI_FORMAT          (ARM_HDMI_BASE + 0x098UL)
#define HDMI_AUDIO_PACKET_CONFIG (ARM_HDMI_BASE + 0x09CUL)
#define HDMI_RAM_PACKET_CONFIG   (ARM_HDMI_BASE + 0x0A0UL)
#define HDMI_RAM_PACKET_STATUS   (ARM_HDMI_BASE + 0x0A4UL)
#define HDMI_CRP_CFG             (ARM_HDMI_BASE + 0x0A8UL)
#define HDMI_AUDIO_CTS0          (ARM_HDMI_BASE + 0x0ACUL)
#define HDMI_AUDIO_CTS1          (ARM_HDMI_BASE + 0x0B0UL)
#define HDMI_RAM_PACKET_START    (ARM_HDMI_BASE + 0x400UL) /* packet slot 1, audio infoframe */

#define HDMI_AUDIO_PACKET_CONFIG_EN (1u << 31)
#define HDMI_CRP_CFG_EXTERNAL_CTS_EN (1u << 24)
#define HDMI_RAM_PACKET_CONFIG_EN (1u << 16) /* infoframe slot 1 enable bit */

/* --- Legacy BCM DMA controller (peripheral +0x7000), driven IRQ-free to feed
 * the MAI FIFO continuously from a double-buffered ring. This controller is
 * unused by Emu68 (only DWC2's own USB DMA) and by the rest of Bellatrix, so a
 * full channel is free. CPU-polled feeding of MAI_DATA produced a discontinuous
 * IEC958 stream that the sink muted; the DMA's HDMI DREQ paces the FIFO steadily
 * (matches the proven Pi Zero W reference Sample_HDMI_DMA_Audio_03). */
#define ARM_DMA_BASE       (ARM_PERI_VIRT_BASE + 0x007000UL)
#define HDMI_DMA_CH        5u  /* full (non-lite) channel, not used by firmware */
#define DMA_CS(ch)         (ARM_DMA_BASE + 0x000UL + 0x100UL * (ch))
#define DMA_CONBLK_AD(ch)  (ARM_DMA_BASE + 0x004UL + 0x100UL * (ch))
#define DMA_SOURCE_AD(ch)  (ARM_DMA_BASE + 0x00CUL + 0x100UL * (ch))
#define DMA_ENABLE_REG     (ARM_DMA_BASE + 0xFF0UL)

#define DMA_CS_RESET       (1u << 31)
#define DMA_CS_WAIT_OUTW   (1u << 28)
#define DMA_CS_ACTIVE      (1u << 0)

#define DMA_TI_PERMAP_HDMI (17u << 16) /* DREQ peripheral 17 = HDMI */
#define DMA_TI_SRC_INC     (1u << 8)
#define DMA_TI_DEST_DREQ   (1u << 6)
#define DMA_TI_BURST2      (2u << 12)

#define HD_BUS_BASE        0x7E808000u          /* HD block, VC bus view */
#define MAI_DATA_BUS       (HD_BUS_BASE + 0x20u) /* MAI FIFO, DMA destination */
#define DMA_BUS_UNCACHED   0xC0000000u           /* Pi3 uncached VC alias of RAM */

enum {
    IEC958_SUBFRAMES_PER_BLOCK = 384,
    HDMI_DMA_HALF_WORDS = 1024,          /* 512 stereo frames/half (~10.7 ms @48k) */
    HDMI_DMA_HALVES     = 2,
    HDMI_DMA_WORDS      = HDMI_DMA_HALF_WORDS * HDMI_DMA_HALVES,
};

/* BCM DMA control block: 8 words, must be 32-byte aligned. Read by the DMA
 * engine straight from RAM, so all fields are stored little-endian. */
typedef struct HdmiDmaCB {
    uint32_t ti, src, dst, len, stride, next, debug, reserved;
} __attribute__((aligned(32))) HdmiDmaCB;

static uint32_t  s_dma_buf[HDMI_DMA_WORDS] __attribute__((aligned(32)));
static HdmiDmaCB s_dma_cb[HDMI_DMA_HALVES] __attribute__((aligned(32)));
static uint32_t  s_dma_buf_bus;   /* native bus addr of s_dma_buf[0] (for poll math) */
static unsigned  s_dma_prev_half;
static bool      s_dma_running;
static int32_t   s_dma_peak;      /* peak |sample| since last diagnostic */
static uint32_t  s_dma_dbg_calls;
static uint32_t  s_dma_fills;         /* total half-refills since last diagnostic */
static uint32_t  s_dma_fill_half[2];  /* per-half refill counts since last diagnostic */

static bool s_hdmi_ready = false;

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

/* Native VC bus address (uncached alias) of a RAM object, for the DMA engine. */
static uint32_t dma_bus(const volatile void *p)
{
    return DMA_BUS_UNCACHED | (uint32_t)mmu_virt2phys((uintptr_t)p);
}

/* One 16-bit sample → MAI FIFO word: sample in bits [27:12], low nibble cleared
 * (the MAI block adds IEC958 channel status/parity/preamble in hardware). */
static uint32_t mai_word(int16_t s)
{
    uint32_t d = (uint32_t)(uint16_t)s;
    d <<= 16;
    d >>= 4;
    d &= ~0xFu;
    return d;
}

/*
 * Debug test-signal modes for the DMA ring (HDMI_DMA_TEST_MODE). All non-zero
 * modes synthesize/replay at the hardware 48 kHz rate, bypassing the emulator
 * output queue, so they isolate the DMA/MAI/HDMI output path from the
 * emulator's (sub-real-time under Musashi) sample-production rate.
 *
 *   0 QUEUE  production — feed the real Paula output queue (default)
 *   1 SINE   clean C-major scale up/down (narrow-band probe)
 *   2 CLIP   replay the embedded hdmi_clip.h PCM (broadband real audio)
 *   3 CHIRP  sine swept 200 Hz..6 kHz, sawtooth (broadband, code-generated)
 *
 * Discriminator (2026-07-09): the SINE plays cleanly but the CLIP comes out as
 * hiss. Since both use the identical mai_word()/DMA path and differ only in
 * sample values, a bit-level word-format bug is unlikely — that fingerprint
 * (narrow-band survives, broadband smears) points at IEC958 clock recovery
 * (CTS/N) or a bad clip. CHIRP is a code-generated broadband signal that tells
 * the two apart: if CHIRP also hisses the fault is the output path (CTS/clock);
 * if CHIRP sweeps audibly the embedded clip was the culprit, not the path.
 */
#define HDMI_DMA_TEST_MODE 0

#define HDMI_DMA_MODE_QUEUE 0
#define HDMI_DMA_MODE_SINE  1
#define HDMI_DMA_MODE_CLIP  2
#define HDMI_DMA_MODE_CHIRP 3

#if HDMI_DMA_TEST_MODE == HDMI_DMA_MODE_CLIP
#include "host/raspi3/hdmi_clip.h"
#endif

#if HDMI_DMA_TEST_MODE == HDMI_DMA_MODE_SINE || \
    HDMI_DMA_TEST_MODE == HDMI_DMA_MODE_CHIRP
/* One-period 64-entry sine LUT (±16000), built from a 17-entry quadrant so no
 * floating point is needed at build or run time. Shared by the SINE scale and
 * the CHIRP sweep. */
static int16_t s_sine64[64];
static bool    s_sine_ready;

static void hdmi_gen_sine(void)
{
    static const int16_t q[17] = {
        0, 1568, 3121, 4645, 6123, 7542, 8889, 10150, 11314,
        12368, 13304, 14111, 14782, 15311, 15693, 15923, 16000
    };
    unsigned i;
    for (i = 0; i <= 16u; i++) s_sine64[i] = q[i];
    for (i = 17u; i < 32u; i++) s_sine64[i] = q[32u - i];
    for (i = 32u; i <= 48u; i++) s_sine64[i] = (int16_t)-q[i - 32u];
    for (i = 49u; i < 64u; i++) s_sine64[i] = (int16_t)-q[64u - i];
    s_sine_ready = true;
}
#endif

#if HDMI_DMA_TEST_MODE == HDMI_DMA_MODE_SINE
/* C-major scale, up then down (Hz). ~0.28 s per note → ~3.9 s loop. */
static const uint16_t s_scale_hz[14] = {
    262, 294, 330, 349, 392, 440, 494, 523, 494, 440, 392, 349, 330, 294
};
#endif

/* Refill one buffer half from the output queue (silence on underrun), storing
 * words little-endian for the DMA, then clean the D-cache so the DMA (reading via
 * the uncached alias) sees the fresh data. */
static void hdmi_dma_fill_half(unsigned half)
{
    uint32_t *dst = &s_dma_buf[half * (unsigned)HDMI_DMA_HALF_WORDS];
    unsigned w;

#if HDMI_DMA_TEST_MODE == HDMI_DMA_MODE_CLIP
    static uint32_t pos; /* int16 index into s_clip (interleaved L,R), looping */
    for (w = 0; w < (unsigned)HDMI_DMA_HALF_WORDS; w += 2u) {
        int16_t l = s_clip[pos];
        int16_t r = s_clip[pos + 1u];
        int32_t a = l < 0 ? -(int32_t)l : (int32_t)l;
        pos += 2u;
        if (pos + 1u >= (uint32_t)HDMI_CLIP_SAMPLES)
            pos = 0u;
        if (a > s_dma_peak) s_dma_peak = a;
        dst[w + 0] = CPU_TO_LE32(mai_word(l));
        dst[w + 1] = CPU_TO_LE32(mai_word(r));
    }
#elif HDMI_DMA_TEST_MODE == HDMI_DMA_MODE_SINE
    static uint32_t phase;      /* 16.16 index into the 64-entry sine LUT */
    static uint32_t note_inc;   /* phase step for the current note */
    static uint32_t note_samp;  /* samples elapsed in the current note */
    static unsigned note_idx;

    if (!s_sine_ready)
        hdmi_gen_sine();

    for (w = 0; w < (unsigned)HDMI_DMA_HALF_WORDS; w += 2u) {
        int16_t v;
        if (note_samp == 0u) {
            /* phase step = freq * 64 (LUT size) * 65536 (16.16) / 48000 */
            note_inc = (uint32_t)(((uint64_t)s_scale_hz[note_idx] * 64ull * 65536ull) / 48000ull);
            phase = 0u; /* start each note at a zero crossing (no click) */
        }
        v = s_sine64[(phase >> 16) & 63u];
        phase += note_inc;
        if (++note_samp >= 13440u) { /* ~0.28 s at 48 kHz */
            note_samp = 0u;
            note_idx = (note_idx + 1u) % 14u;
        }
        if (v > s_dma_peak) s_dma_peak = v;
        dst[w + 0] = CPU_TO_LE32(mai_word(v));
        dst[w + 1] = CPU_TO_LE32(mai_word(v));
    }
#elif HDMI_DMA_TEST_MODE == HDMI_DMA_MODE_CHIRP
    static uint32_t phase_l; /* 16.16 index into the 64-entry sine LUT, L */
    static uint32_t phase_r; /* ...and R (swept the opposite way) */
    static uint32_t sweep;   /* sample counter, one full ramp per second */

    if (!s_sine_ready)
        hdmi_gen_sine();

    for (w = 0; w < (unsigned)HDMI_DMA_HALF_WORDS; w += 2u) {
        /* Stereo linear sweep over 1 s (sawtooth): L rises 200 Hz → 6 kHz while
         * R falls 6 kHz → 200 Hz. Broadband AND decorrelated between channels,
         * so it probes three failure modes at once: a clean path renders two
         * crossing "whoops"; an IEC958 clock/CTS fault smears both to hiss; an
         * L/R FIFO-alignment slip (silent on a mono tone) shows as channel
         * swap/crackle. */
        uint32_t p  = sweep % 48000u;
        uint32_t d  = (uint32_t)(((uint64_t)(6000u - 200u) * p) / 48000u);
        uint32_t fl = 200u + d;         /* L: 200 → 6000 */
        uint32_t fr = 6000u - d;        /* R: 6000 → 200 */
        uint32_t incl = (uint32_t)(((uint64_t)fl * 64ull * 65536ull) / 48000ull);
        uint32_t incr = (uint32_t)(((uint64_t)fr * 64ull * 65536ull) / 48000ull);
        int16_t vl = s_sine64[(phase_l >> 16) & 63u];
        int16_t vr = s_sine64[(phase_r >> 16) & 63u];
        int32_t a  = vl < 0 ? -(int32_t)vl : (int32_t)vl;
        phase_l += incl;
        phase_r += incr;
        sweep++;
        if (a > s_dma_peak) s_dma_peak = a;
        dst[w + 0] = CPU_TO_LE32(mai_word(vl));
        dst[w + 1] = CPU_TO_LE32(mai_word(vr));
    }
#else
    AudioSample s;
    static int16_t last_l, last_r; /* held across calls for graceful underrun */
    for (w = 0; w < (unsigned)HDMI_DMA_HALF_WORDS; w += 2u) {
        if (bellatrix_audio_output_pop(&s)) {
            int32_t al = s.left  < 0 ? -(int32_t)s.left  : (int32_t)s.left;
            int32_t ar = s.right < 0 ? -(int32_t)s.right : (int32_t)s.right;
            if (al > s_dma_peak) s_dma_peak = al;
            if (ar > s_dma_peak) s_dma_peak = ar;
            last_l = s.left;
            last_r = s.right;
            dst[w + 0] = CPU_TO_LE32(mai_word(s.left));
            dst[w + 1] = CPU_TO_LE32(mai_word(s.right));
        } else {
            /* Underrun: exponentially decay the last sample toward zero rather
             * than slamming to silence. A hard 0 is a discontinuity (click); a
             * ~1.5 %/sample fade is inaudible, avoids a stuck DC offset, and
             * self-heals the instant production resumes. (Fills the gap for
             * brief jitter; sustained sub-real-time production still fades to
             * silence — that is the speed axis, not this sync layer.) */
            last_l = (int16_t)(last_l - (last_l >> 6));
            last_r = (int16_t)(last_r - (last_r >> 6));
            dst[w + 0] = CPU_TO_LE32(mai_word(last_l));
            dst[w + 1] = CPU_TO_LE32(mai_word(last_r));
        }
    }
#endif

    arm_flush_cache((uintptr_t)dst, (unsigned)HDMI_DMA_HALF_WORDS * 4u);

    s_dma_fills++;
    s_dma_fill_half[half & 1u]++;
}

static void hdmi_dma_setup(void)
{
    unsigned i;

    for (i = 0; i < (unsigned)HDMI_DMA_WORDS; i++)
        s_dma_buf[i] = 0u;

    s_dma_buf_bus = dma_bus(&s_dma_buf[0]);

    for (i = 0; i < (unsigned)HDMI_DMA_HALVES; i++) {
        s_dma_cb[i].ti  = CPU_TO_LE32(DMA_TI_PERMAP_HDMI | DMA_TI_DEST_DREQ
                                      | DMA_TI_SRC_INC | DMA_TI_BURST2);
        s_dma_cb[i].src = CPU_TO_LE32(dma_bus(&s_dma_buf[i * (unsigned)HDMI_DMA_HALF_WORDS]));
        s_dma_cb[i].dst = CPU_TO_LE32(MAI_DATA_BUS);
        s_dma_cb[i].len = CPU_TO_LE32((unsigned)HDMI_DMA_HALF_WORDS * 4u);
        s_dma_cb[i].stride = 0u;
        s_dma_cb[i].next = CPU_TO_LE32(dma_bus(&s_dma_cb[(i + 1u) % (unsigned)HDMI_DMA_HALVES]));
        s_dma_cb[i].debug = 0u;
        s_dma_cb[i].reserved = 0u;
    }

    arm_flush_cache((uintptr_t)s_dma_buf, sizeof(s_dma_buf));
    arm_flush_cache((uintptr_t)s_dma_cb, sizeof(s_dma_cb));

    hdmi_wr32(DMA_ENABLE_REG, hdmi_rd32(DMA_ENABLE_REG) | (1u << HDMI_DMA_CH));
    hdmi_wr32(DMA_CS(HDMI_DMA_CH), DMA_CS_RESET);
    hdmi_wr32(DMA_CONBLK_AD(HDMI_DMA_CH), dma_bus(&s_dma_cb[0]));
    __asm__ __volatile__("dsb sy" ::: "memory");
    hdmi_wr32(DMA_CS(HDMI_DMA_CH), DMA_CS_WAIT_OUTW | DMA_CS_ACTIVE);

    s_dma_prev_half = 0u;
    s_dma_running = true;
}

bool hdmi_audio_init(void)
{
    uint32_t hsm_hz;

    s_hdmi_ready = false;

    if (!hdmi_display_audio_supported()) {
        kprintf("hdmi_audio: display did not enable audio packets (RamPacketConfig) -- no HDMI audio sink\n");
        return false;
    }

    /* Snapshot BEFORE we touch anything. With hdmi_drive=2 the VideoCore firmware
     * already configures the HDMI audio path (APCFG, CRP/CTS, audio infoframe in a
     * RAM-packet slot). This PRE dump reveals that pristine state so we can tell
     * which of our writes are redundant or actively clobbering the firmware. */
    kprintf("hdmi_audio: PRE  MAI_CTL=%08x FMT=%08x CFG=%08x CHMAP=%08x APCFG=%08x "
            "CRP=%08x CTS0=%08x CTS1=%08x RAMPKT_CFG=%08x THR=%08x\n",
            (unsigned)hdmi_rd32(MAI_CTL), (unsigned)hdmi_rd32(MAI_FMT),
            (unsigned)hdmi_rd32(HDMI_MAI_CONFIG), (unsigned)hdmi_rd32(HDMI_MAI_CHANNEL_MAP),
            (unsigned)hdmi_rd32(HDMI_AUDIO_PACKET_CONFIG), (unsigned)hdmi_rd32(HDMI_CRP_CFG),
            (unsigned)hdmi_rd32(HDMI_AUDIO_CTS0), (unsigned)hdmi_rd32(HDMI_AUDIO_CTS1),
            (unsigned)hdmi_rd32(HDMI_RAM_PACKET_CONFIG), (unsigned)hdmi_rd32(MAI_THR));

    /* HSM clock read is kept for diagnostics only — it must NOT gate init.
     * On this firmware/CM configuration it reads back 0, and the proven Pi Zero W
     * reference (external/RaspberryPI Sample_HDMI_DMA_Audio_03) never reads it:
     * it hardcodes N/CTS/SMP and relies on the block's external CTS measurement
     * (EXTERNAL_CTS_EN), which makes a fixed CTS seed valid regardless of the
     * actual pixel/HSM clock. */
    hsm_hz = hdmi_read_hsm_clock_hz(); /* diagnostic only */

    /* Defer to the firmware. With hdmi_drive=2 the VideoCore firmware already
     * brings up the entire HDMI audio path: MAI_CONFIG channel enables, the
     * channel map, AUDIO_PACKET_CONFIG, CRP with automatic CTS, the audio
     * infoframe in RAM-packet slot 4, and the MAI FIFO thresholds. The real-HW
     * PRE/POST dumps proved our previous setup was CLOBBERING that working config
     * (it zeroed MAI_CONFIG's L/R enables, overwrote the channel map, changed the
     * CRP mode and thresholds). We now write none of it and only feed samples into
     * the MAI data FIFO — but on the first HW test that alone stayed silent: the
     * firmware leaves the MAI engine idle (MAI_CTL=0x420). So we additionally do a
     * minimal MAI-engine bring-up, touching ONLY the HD-block MAI registers
     * (reset/flush, sample-rate, format, thresholds, enable) with the proven Pi
     * Zero W reference values, and still leave the HDMI-block packet/infoframe
     * config (APCFG/CRP/CTS/channel map/MAI_CONFIG/RAM packet) entirely to the
     * firmware. */
    hdmi_wr32(MAI_CTL, (1u << 9));                                 /* FLUSH */
    hdmi_wr32(MAI_CTL, (1u<<0)|(1u<<1)|(1u<<2)|(1u<<15)|(1u<<9));  /* RST|OF|UF|DLATE|FLUSH */
    hdmi_wr32(MAI_SMP, 0x0DCD21F3u);                              /* sample-rate constant (48 kHz) */
    hdmi_wr32(MAI_FMT, 0x00020900u);                             /* 2 channels, 48 kHz */
    hdmi_wr32(MAI_THR, 0x08080608u);                             /* MAI FIFO thresholds */
    hdmi_wr32(MAI_CTL, (1u<<3)|(2u<<4)|(1u<<12)|(1u<<13));        /* MAI enable, 2 channels */

    kprintf("hdmi_audio: init ok (firmware config + minimal MAI enable; hsm=%u Hz)\n",
            (unsigned)hsm_hz);
    /* Post-setup register snapshot — lets a single real-Pi boot capture the
     * ground-truth MAI/HDMI state needed to finish tuning the bit-level values
     * (MAI_CTL layout, AUDIO_PACKET_CONFIG, RAM packet enable) against hardware. */
    kprintf("hdmi_audio: POST MAI_CTL=%08x FMT=%08x CFG=%08x CHMAP=%08x APCFG=%08x "
            "CRP=%08x CTS0=%08x CTS1=%08x RAMPKT_CFG=%08x THR=%08x\n",
            (unsigned)hdmi_rd32(MAI_CTL), (unsigned)hdmi_rd32(MAI_FMT),
            (unsigned)hdmi_rd32(HDMI_MAI_CONFIG), (unsigned)hdmi_rd32(HDMI_MAI_CHANNEL_MAP),
            (unsigned)hdmi_rd32(HDMI_AUDIO_PACKET_CONFIG), (unsigned)hdmi_rd32(HDMI_CRP_CFG),
            (unsigned)hdmi_rd32(HDMI_AUDIO_CTS0), (unsigned)hdmi_rd32(HDMI_AUDIO_CTS1),
            (unsigned)hdmi_rd32(HDMI_RAM_PACKET_CONFIG), (unsigned)hdmi_rd32(MAI_THR));

    /* Start the IRQ-free DMA ring that feeds the MAI FIFO from the output queue. */
    hdmi_dma_setup();
    kprintf("hdmi_audio: dma ch%u started, CS=%08x CONBLK=%08x\n",
            (unsigned)HDMI_DMA_CH,
            (unsigned)hdmi_rd32(DMA_CS(HDMI_DMA_CH)),
            (unsigned)hdmi_rd32(DMA_CONBLK_AD(HDMI_DMA_CH)));

    s_hdmi_ready = true;
    return true;
}

/* Poll-driven DMA ring refill — called every chipset step from
 * machine_rigel_step.c. No interrupts: we watch which half the DMA is reading
 * (its source address) and refill the half it just finished. */
void hdmi_audio_dma_poll(void)
{
    uint32_t cur, off;
    unsigned cur_half;

    if (!s_dma_running)
        return;

    cur = hdmi_rd32(DMA_SOURCE_AD(HDMI_DMA_CH));
    off = cur - s_dma_buf_bus;
    cur_half = (unsigned)((off / ((unsigned)HDMI_DMA_HALF_WORDS * 4u))
                          % (unsigned)HDMI_DMA_HALVES);

    if (cur_half != s_dma_prev_half) {
        hdmi_dma_fill_half(s_dma_prev_half);
        s_dma_prev_half = cur_half;
    }

    if (++s_dma_dbg_calls >= 131072u) {
        BellatrixAudioOutputStats st = bellatrix_audio_output_stats();
        kprintf("[HDMI-AUD] dma_src=%08x buf=%08x off=%05x half=%u CS=%08x "
                "fills=%u (h0=%u h1=%u) produced=%llu consumed=%llu "
                "underrun=%llu qcount=%u peak=%d\n",
                (unsigned)cur, (unsigned)s_dma_buf_bus, (unsigned)off, cur_half,
                (unsigned)hdmi_rd32(DMA_CS(HDMI_DMA_CH)),
                (unsigned)s_dma_fills,
                (unsigned)s_dma_fill_half[0], (unsigned)s_dma_fill_half[1],
                (unsigned long long)st.produced_samples,
                (unsigned long long)st.consumed_samples,
                (unsigned long long)st.underrun_samples,
                (unsigned)bellatrix_audio_output_count(),
                (int)s_dma_peak);
        s_dma_dbg_calls = 0;
        s_dma_peak = 0;
        s_dma_fills = 0;
        s_dma_fill_half[0] = 0;
        s_dma_fill_half[1] = 0;
    }
}

bool hdmi_audio_writable(void)
{
    if (!s_hdmi_ready)
        return false;

    return (hdmi_rd32(MAI_CTL) & MAI_CTL_BUSY) == 0u;
}

void hdmi_audio_write_sample(int16_t sample16, unsigned subframe_in_block)
{
    uint32_t data;

    (void)subframe_in_block; /* HW does IEC958 framing; no software block counting */

    if (!s_hdmi_ready)
        return;

    /* MAI FIFO word format (proven Pi Zero W reference Sample_HDMI_DMA_Audio_03):
     * the 16-bit sample sits in bits [27:12] with the low nibble cleared; the MAI
     * block adds the IEC958 channel status, parity and preamble in hardware. One
     * FIFO word per channel sample — the caller writes left then right. */
    data = (uint32_t)(uint16_t)sample16;
    data <<= 16;
    data >>= 4;
    data &= ~0xFu;

    hdmi_wr32(MAI_DATA, data);
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
    /* We no longer own the HDMI audio configuration (the firmware does), so stop
     * simply means "stop feeding the FIFO" — do not tear down the firmware's
     * setup. */
    s_hdmi_ready = false;
}
