#ifndef BELLATRIX_DEBUG_CORE_LOG_H
#define BELLATRIX_DEBUG_CORE_LOG_H

/*
 * Per-core log tags — canonical taxonomy from docs/runtime_core_testing.md
 * and docs/regression_and_validation_strategy.md.
 *
 * Tags:
 *   [CORE0-CPU]    Core 0 — Emu68 JIT execution, CPU state, IPL consumption
 *   [CORE1-GFX]    Core 1 — Agnus, Denise, raster, DMA, copper, blitter
 *   [CORE2-PAULA]  Core 2 — Paula: INTREQ/INTENA, IPL publication, audio, serial, disk
 *   [CORE3-IO]     Core 3 — CIA-A/B, timers, TOD, keyboard, host UART, floppy lines
 *   [XCORE-<tag>]  Cross-core event propagation (use a descriptive tag)
 *
 * Usage:
 *   CORE0_LOG("IPL=%u", ipl);
 *   XCORE_LOG("CIA->PAULA", "INTREQ=%04x", intreq);
 *
 * Enable by defining BELLATRIX_CORE_LOG before including this header,
 * or via the build system: -DBELLATRIX_CORE_LOG
 *
 * When disabled, all macros are zero-cost no-ops.
 */

#include "support.h"

#ifdef BELLATRIX_CORE_LOG

#define CORE0_LOG(fmt, ...)        kprintf("[CORE0-CPU] "   fmt "\n", ##__VA_ARGS__)
#define CORE1_LOG(fmt, ...)        kprintf("[CORE1-GFX] "   fmt "\n", ##__VA_ARGS__)
#define CORE2_LOG(fmt, ...)        kprintf("[CORE2-PAULA] " fmt "\n", ##__VA_ARGS__)
#define CORE3_LOG(fmt, ...)        kprintf("[CORE3-IO] "    fmt "\n", ##__VA_ARGS__)
#define XCORE_LOG(tag, fmt, ...)   kprintf("[XCORE-" tag "] " fmt "\n", ##__VA_ARGS__)

#else

#define CORE0_LOG(fmt, ...)        do {} while (0)
#define CORE1_LOG(fmt, ...)        do {} while (0)
#define CORE2_LOG(fmt, ...)        do {} while (0)
#define CORE3_LOG(fmt, ...)        do {} while (0)
#define XCORE_LOG(tag, fmt, ...)   do {} while (0)

#endif /* BELLATRIX_CORE_LOG */

#endif /* BELLATRIX_DEBUG_CORE_LOG_H */
