#include "cpu/emu68/emu68_machine_platform.h"

#include "A64.h"
#include "M68k.h"
#include "cache.h"
#include "mmu.h"

#include <string.h>

extern struct M68KState *__m68k_state;
extern void mmu_unmap(uintptr_t virt, uintptr_t length);

emu68_status_t emu68_machine_platform_map_direct(
    uint32_t guest_base, uint64_t size, void *host_base, uint32_t flags)
{
    uint32_t attr = MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0;

    attr |= (flags & EMU68_REGION_CACHEABLE) ? MMU_ATTR_CACHED :
                                               MMU_ATTR_UNCACHED;
    if ((flags & EMU68_REGION_WRITE) == 0u)
        attr |= MMU_READ_ONLY;
    mmu_map(mmu_virt2phys((uintptr_t)host_base), guest_base,
            (uintptr_t)size, attr, 0u);
    return EMU68_OK;
}

void emu68_machine_platform_unmap_direct(uint32_t guest_base, uint64_t size)
{
    mmu_unmap(guest_base, (uintptr_t)size);
}

void emu68_machine_platform_invalidate(uint32_t guest_base, uint64_t size)
{
    if (size > UINT32_MAX)
        cache_invalidate_all(ICACHE);
    else
        cache_invalidate_range(ICACHE, guest_base, (uint32_t)size);
}

void emu68_machine_platform_invalidate_all(void)
{
    cache_invalidate_all(ICACHE);
}

emu68_status_t emu68_machine_platform_reset(uint32_t initial_ssp,
                                            uint32_t initial_pc)
{
    uint32_t jit_control;
    uint32_t jit_control2;

    if (!__m68k_state)
        return EMU68_ERR_INTERNAL;

    jit_control = __m68k_state->JIT_CONTROL;
    jit_control2 = __m68k_state->JIT_CONTROL2;
    memset(__m68k_state, 0, sizeof(*__m68k_state));
    __m68k_state->JIT_CONTROL = jit_control;
    __m68k_state->JIT_CONTROL2 = jit_control2;
    __m68k_state->ISP.u32 = initial_ssp;
    __m68k_state->A[7].u32 = initial_ssp;
    __m68k_state->PC = initial_pc;
    __m68k_state->SR = SR_S | SR_IPL;
    __m68k_state->CACR = CACR_IE;
    cache_invalidate_all(ICACHE);
    return EMU68_OK;
}

emu68_status_t emu68_machine_platform_set_ipl(unsigned level)
{
    if (!__m68k_state)
        return EMU68_ERR_INTERNAL;
    __atomic_store_n(&__m68k_state->INT.IPL, (uint8_t)level, __ATOMIC_RELEASE);
    emu68_machine_platform_wake();
    return EMU68_OK;
}

void emu68_machine_platform_wake(void)
{
#ifdef __aarch64__
    __asm__ volatile("sev" ::: "memory");
#endif
}
