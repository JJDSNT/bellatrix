#ifndef BELLATRIX_BT_TRACE_LOG_H
#define BELLATRIX_BT_TRACE_LOG_H

/*
 * Low-level BT HAL byte/block tracing ([BT-HAL] rx/tx block request/
 * complete/preview, [BT-RX]/[BT-TX] per-byte dumps).  Fires on every HCI
 * transaction, so during scan/inquiry retry loops it floods the serial log
 * and drowns out higher-value signal (e.g. [EMU68-LIVE], [DIAG]).
 *
 * Enable by defining BELLATRIX_BT_TRACE before including this header, or
 * via the build system: -DBELLATRIX_BT_TRACE
 *
 * When disabled, all macros are zero-cost no-ops. State-transition logs
 * ([BT-CM] ... -> ..., pairing window open/close, HID connect events) are
 * NOT gated by this — those are low-frequency and worth keeping always on.
 */

#include "support.h"

#ifdef BELLATRIX_BT_TRACE

#define BT_TRACE_LOG(fmt, ...)   kprintf(fmt "\n", ##__VA_ARGS__)

#else

#define BT_TRACE_LOG(fmt, ...)   do {} while (0)

#endif /* BELLATRIX_BT_TRACE */

#endif /* BELLATRIX_BT_TRACE_LOG_H */
