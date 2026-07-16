#ifndef BELLATRIX_DEBUG_CORE_LOG_H
#define BELLATRIX_DEBUG_CORE_LOG_H

/*
 * Runtime-role log tags. A role may move to another numbered core without
 * changing the subsystem's identity in diagnostics.
 *
 * Tags:
 *   [HOST]          host reactor: timeline, physical IO and presentation
 *   [CPU]           selected CPU backend
 *   [CHIPSET]       Rigel: Agnus, Denise, Paula and CIA
 *   [XCORE-<tag>]  Cross-core event propagation (use a descriptive tag)
 *
 * Usage:
 *   CPU_LOG("IPL=%u", ipl);
 *   XCORE_LOG("CPU->CHIPSET", "cck_target=%llu", target);
 *
 * Enable by defining BELLATRIX_CORE_LOG before including this header,
 * or via the build system: -DBELLATRIX_CORE_LOG
 *
 * When disabled, all macros are zero-cost no-ops.
 */

#include "support.h"

#ifdef BELLATRIX_CORE_LOG

#define HOST_LOG(fmt, ...)         kprintf("[HOST] "    fmt "\n", ##__VA_ARGS__)
#define CPU_LOG(fmt, ...)          kprintf("[CPU] "     fmt "\n", ##__VA_ARGS__)
#define CHIPSET_LOG(fmt, ...)      kprintf("[CHIPSET] " fmt "\n", ##__VA_ARGS__)
#define XCORE_LOG(tag, fmt, ...)   kprintf("[XCORE-" tag "] " fmt "\n", ##__VA_ARGS__)

#else

#define HOST_LOG(fmt, ...)         do {} while (0)
#define CPU_LOG(fmt, ...)          do {} while (0)
#define CHIPSET_LOG(fmt, ...)      do {} while (0)
#define XCORE_LOG(tag, fmt, ...)   do {} while (0)

#endif /* BELLATRIX_CORE_LOG */

#endif /* BELLATRIX_DEBUG_CORE_LOG_H */
