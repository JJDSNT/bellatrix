#ifndef BELLATRIX_EXPANSIONS_LIDE_CDROM_H
#define BELLATRIX_EXPANSIONS_LIDE_CDROM_H

#include <stdint.h>
#include <stddef.h>

#include "storage/iso/iso_image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BellatrixMachine BellatrixMachine;

/*
 * lide_cdrom — OAHR RIPPLE (MFR=0x144A, PROD=7) CD-ROM expansion.
 *
 * Registers a Zorro 2 board and an ATAPI CD-ROM backed by an ISO image.
 * The board ROM is embedded at build time (g_lide_rom_data / g_lide_rom_size).
 */

int  lide_cdrom_register(BellatrixMachine *machine);

/* Insert/eject ISO image (in-memory) */
int  lide_cdrom_insert_iso(BellatrixMachine *machine,
                           const void *data, size_t size);
void lide_cdrom_eject(BellatrixMachine *machine);

/* Attach an on-demand read callback instead of an in-memory buffer.
 * Uses iso_read_fn (multi-sector: bool fn(ctx, lba, count, dst)) so the
 * same callback type flows from the launcher through machine.c here. */
int lide_cdrom_attach_iso_fn(BellatrixMachine *machine,
                             iso_read_fn fn, void *ctx,
                             uint32_t sector_count);

#ifdef __cplusplus
}
#endif

#endif
