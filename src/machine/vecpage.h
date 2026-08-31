/*
 * src/machine/vecpage.h
 *
 * The opt-in fault-driven vector page. See vecpage.c for why it is opt-in.
 */

#ifndef BELLATRIX_MACHINE_VECPAGE_H
#define BELLATRIX_MACHINE_VECPAGE_H

#include "machine/region.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called by Emu68's parse_cmdline (patches/emu68/0025). */
void bellatrix_parse_cmdline(const char *cmdline);

/* Did the command line ask for it? Decided before machine_init() runs. */
int machine_vecpage_trapped(void);

extern const MachineRegionOps machine_vecpage_ops;

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_MACHINE_VECPAGE_H */
