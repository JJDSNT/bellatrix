#ifndef BELLATRIX_EMU68_BACKEND_H
#define BELLATRIX_EMU68_BACKEND_H

struct CpuBackend;

struct CpuBackend *bellatrix_emu68_backend_get(void);
void bellatrix_emu68_backend_init(void);
int bellatrix_emu68_backend_set_overlay(int enabled);

#endif
