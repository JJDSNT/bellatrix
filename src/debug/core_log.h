#ifndef BELLATRIX_DEBUG_CORE_LOG_H
#define BELLATRIX_DEBUG_CORE_LOG_H

/*
 * Per-core log tags — canonical taxonomy from multicore.md and
 * docs/runtime_core_testing.md.
 *
 * Tags:
 *   [CORE0-HOST]    Core 0 — Machine/boot/supervisor and physical IO
 *   [CORE1-CPU]     Core 1 — CPU backend (Emu68 JIT or Musashi)
 *   [CORE2-CHIPSET] Core 2 — Rigel: Agnus, Denise, Paula, CIA (single-thread)
 *   [CORE3-IO]      Core 3 — reserved (future RTG/AHI worker)
 *   [XCORE-<tag>]  Cross-core event propagation (use a descriptive tag)
 *
 * Usage:
 *   CORE1_LOG("IPL=%u", ipl);
 *   XCORE_LOG("CPU->CHIPSET", "cck_target=%llu", target);
 *
 * Enable by defining BELLATRIX_CORE_LOG before including this header,
 * or via the build system: -DBELLATRIX_CORE_LOG
 *
 * When disabled, all macros are zero-cost no-ops.
 */

#include "support.h"

#ifdef BELLATRIX_CORE_LOG

#define CORE0_LOG(fmt, ...)        kprintf("[CORE0-HOST] "    fmt "\n", ##__VA_ARGS__)
#define CORE1_LOG(fmt, ...)        kprintf("[CORE1-CPU] "     fmt "\n", ##__VA_ARGS__)
#define CORE2_LOG(fmt, ...)        kprintf("[CORE2-CHIPSET] " fmt "\n", ##__VA_ARGS__)
#define CORE3_LOG(fmt, ...)        kprintf("[CORE3-IO] "      fmt "\n", ##__VA_ARGS__)
#define XCORE_LOG(tag, fmt, ...)   kprintf("[XCORE-" tag "] " fmt "\n", ##__VA_ARGS__)

#else

#define CORE0_LOG(fmt, ...)        do {} while (0)
#define CORE1_LOG(fmt, ...)        do {} while (0)
#define CORE2_LOG(fmt, ...)        do {} while (0)
#define CORE3_LOG(fmt, ...)        do {} while (0)
#define XCORE_LOG(tag, fmt, ...)   do {} while (0)

#endif /* BELLATRIX_CORE_LOG */

#endif /* BELLATRIX_DEBUG_CORE_LOG_H */
