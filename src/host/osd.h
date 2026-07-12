// src/host/osd.h — on-screen FPS/frame overlay
// Compiled only when BELLATRIX_OSD is defined.

#pragma once
#include <stdint.h>

#ifdef BELLATRIX_OSD
void osd_render(uint64_t frame);
/* Qualifies the emulated machine-frame clock separately from the presentation
 * counter passed to osd_render (ISSUE-0019). */
void osd_set_machine_frame(uint64_t frame);
void osd_set_realtime_percent(uint32_t percent);
/* Firmware GET_THROTTLED flags (bit0 undervoltage, bit2 throttled). The
 * heartbeat feeds this ~0.5 Hz; the overlay shows a red UV! while any
 * live condition is set. Pass 0 to clear. */
void osd_set_power_alert(uint32_t throttled_flags);
#else
static inline void osd_render(uint64_t frame) { (void)frame; }
static inline void osd_set_machine_frame(uint64_t frame) { (void)frame; }
static inline void osd_set_realtime_percent(uint32_t percent) { (void)percent; }
static inline void osd_set_power_alert(uint32_t throttled_flags)
{
    (void)throttled_flags;
}
#endif
