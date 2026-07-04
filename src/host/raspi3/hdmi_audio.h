#ifndef BELLATRIX_HOST_RASPI3_HDMI_AUDIO_H
#define BELLATRIX_HOST_RASPI3_HDMI_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * BCM2835/2837 (Raspberry Pi 3B, RASPPI<=3 register layout) HDMI audio —
 * direct MAI/HDMI register access, no VideoCore firmware service (VCHIQ)
 * involved. See AI_context/issues/ISSUE-0011.md for the bring-up plan this
 * follows and ISSUE-0034's sibling FPU work for the general project
 * convention of keeping bare-metal-only drivers self-contained.
 *
 * Independent implementation from the BCM2835/2837 peripheral register map
 * and the published IEC 60958-3 (S/PDIF) framing standard — not a port of
 * any GPL codebase. See the plan history for the licensing reasoning
 * (Bellatrix has no LICENSE file yet; intent is MIT, so no GPL-derived
 * expression is brought in here, only hardware/standard facts).
 *
 * Scope: Pi 3B only (no Pi4/Pi5 register variants), polling mode only (no
 * DMA yet — ISSUE-0011 explicitly orders DMA after the first working
 * polling-mode proof), fixed at BELLATRIX_AUDIO_OUTPUT_RATE_HZ (48 kHz).
 */

bool hdmi_audio_init(void);
bool hdmi_audio_writable(void);

/* sample16: same S16 range as AudioSample.left/right (audio/mixer.h).
 * subframe_in_block: position within the 384-subframe (192-frame stereo)
 * IEC958 block — caller must count 0..383 and wrap, incrementing once per
 * channel (left then right), so the B-preamble lands on subframe 0. */
void hdmi_audio_write_sample(int16_t sample16, unsigned subframe_in_block);

/* Generates a simple sine tone directly, bypassing audio/output.c's Paula
 * queue entirely — for validating the hardware path in isolation (ISSUE-0011
 * bring-up step 2), before Paula's real audio is wired in (step 5). */
void hdmi_audio_test_tone_tick(void);

void hdmi_audio_stop(void);

#endif
