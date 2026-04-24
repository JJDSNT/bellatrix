// tools/harness/musashi_backend.h
#pragma once

#include "cpu/cpu_backend.h"
#include <stdint.h>

/* Load ROM image into the harness bus dispatcher.
 * base: M68K address for single-window ROMs (256/512 KB).
 *       Ignored for 1 MB ROMs — split is automatic (0xE00000 + 0xF80000).
 * Call before musashi_backend_reset(). */
void musashi_backend_load_rom(const uint8_t *data, uint32_t size, uint32_t base);

/* Return the CpuBackend suitable for bellatrix_machine_init(). */
CpuBackend *musashi_backend_get(void);

/* Init Musashi (call before bellatrix_machine_init). */
void musashi_backend_init(void);

/* Pulse the CPU reset line (reads reset vectors from bus). */
void musashi_backend_reset(void);

/* Execute up to `cycles` M68K cycles; returns cycles actually consumed. */
int musashi_backend_run(int cycles);
