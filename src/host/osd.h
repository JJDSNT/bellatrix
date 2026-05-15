// src/host/osd.h — on-screen FPS/frame overlay
// Compiled only when BELLATRIX_OSD is defined.

#pragma once
#include <stdint.h>

#ifdef BELLATRIX_OSD
void osd_render(uint64_t frame);
#else
static inline void osd_render(uint64_t frame) { (void)frame; }
#endif
