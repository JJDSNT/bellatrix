#include "cpu/emu68/emu68_backend.h"

#include "cpu/cpu_backend.h"
#include "cpu/emu68/emu68_direct_region.h"
#include "host/pal.h"
#include "support.h"

#include "M68k.h"

extern struct M68KState *__m68k_state;

static uint32_t native_get_pc(void *ctx)
{
    (void)ctx;
    return __m68k_state ? BE32(__m68k_state->PC) : 0u;
}

static void native_set_ipl(void *ctx, int level)
{
    (void)ctx;
    PAL_IPL_Set((uint8_t)level);
}

static int native_map_direct(void *ctx,
                             const BellatrixDirectRegion *region)
{
    (void)ctx;
    return bellatrix_emu68_direct_region_install(region);
}

static int native_unmap_direct(void *ctx, uint32_t guest_base, uint32_t size)
{
    (void)ctx;
    return bellatrix_emu68_direct_region_remove(guest_base, size);
}

static CpuBackend s_native_backend = {
    .ctx = NULL,
    .get_pc = native_get_pc,
    .set_ipl = native_set_ipl,
    .map_direct = native_map_direct,
    .unmap_direct = native_unmap_direct,
};

CpuBackend *bellatrix_emu68_backend_get(void)
{
    return &s_native_backend;
}

void bellatrix_emu68_backend_init(void)
{
    bellatrix_emu68_direct_region_init();
}

int bellatrix_emu68_backend_set_overlay(int enabled)
{
    (void)enabled;
    /* Native fault-driven execution keeps low-memory MMU ownership in
     * bellatrix.c; there is no managed-machine stop/remap protocol. */
    return 0;
}
