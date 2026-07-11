#include "cpu/emu68/emu68_api.h"

#include "M68k.h"
#include "cache.h"
#include "support.h"

#include <string.h>

extern struct M68KState *__m68k_state;
extern void MainLoop(void);
#ifdef BELLATRIX
extern void MainLoopWindow(void);
#endif

struct emu68 {
    emu68_config_t config;
    emu68_bus_ops_t bus;
    void *bus_user;
    emu68_hle_ops_t hle;
    void *hle_user;
    emu68_event_fn event_fn;
    void *event_user;
    emu68_stats_t stats;
    bool in_use;
    bool stop_requested;
    bool run_active;
    uint32_t run_budget_cycles;
    uint64_t run_cycles;
    uint64_t run_start_insn_count;
    uint64_t run_last_insn_count;
    emu68_stop_reason_t run_stop_reason;
    uint32_t run_stop_pc;
    uint32_t run_stop_detail;
    bool sync_pending;
    uint32_t sync_addr;
#if defined(BELLATRIX_EMU68_API_TRACE) && BELLATRIX_EMU68_API_TRACE
    bool logged_first_read;
    bool logged_first_write;
    bool logged_first_sync;
#endif
#if defined(BELLATRIX_EMU68_API_AUTODUMP) && BELLATRIX_EMU68_API_AUTODUMP
    bool dumped_first_sync_stats;
#endif
};

static emu68_t g_emu68_api;

static emu68_run_result_t unsupported_result(emu68_t *cpu,
                                             emu68_status_t detail)
{
    emu68_run_result_t result;

    memset(&result, 0, sizeof(result));
    result.reason = EMU68_STOP_UNSUPPORTED;
    result.detail = (uint32_t)detail;
    if (cpu && __m68k_state)
        result.pc = BE32(__m68k_state->PC);

    return result;
}

static void emit_event(emu68_t *cpu, const emu68_event_t *event)
{
    if (cpu && cpu->event_fn)
        cpu->event_fn(cpu->event_user, event);
}

static uint32_t emu68_api_estimate_cycles(emu68_t *cpu,
                                          uint64_t retired_instructions)
{
    uint64_t delta;

    if (!cpu)
        return 0;

    if (cpu->run_last_insn_count == 0u ||
        retired_instructions <= cpu->run_last_insn_count) {
        cpu->run_last_insn_count = retired_instructions;
        return 8u;
    }

    delta = retired_instructions - cpu->run_last_insn_count;
    cpu->run_last_insn_count = retired_instructions;
    if (delta > (UINT32_MAX / 8u))
        return UINT32_MAX;

    return (uint32_t)delta * 8u;
}

static emu68_run_result_t make_run_result(emu68_t *cpu)
{
    emu68_run_result_t result;

    memset(&result, 0, sizeof(result));
    if (!cpu) {
        result.reason = EMU68_STOP_UNSUPPORTED;
        result.detail = (uint32_t)EMU68_ERR_INVALID_ARG;
        return result;
    }

    result.reason = cpu->run_stop_reason;
    result.cycles_run = cpu->run_cycles;
    result.pc = cpu->run_stop_pc;
    result.detail = cpu->run_stop_detail;
    if (!result.pc && __m68k_state)
        result.pc = BE32(__m68k_state->PC);

    return result;
}

#if defined(BELLATRIX_EMU68_API_TRACE) && BELLATRIX_EMU68_API_TRACE
static uint32_t current_pc(void)
{
    if (!__m68k_state)
        return 0;

    return BE32(__m68k_state->PC);
}
#endif

uint32_t emu68_api_version(void)
{
    return EMU68_API_VERSION;
}

emu68_t *emu68_create(const emu68_config_t *config)
{
    if (g_emu68_api.in_use)
        return NULL;

    memset(&g_emu68_api, 0, sizeof(g_emu68_api));
    if (config)
        g_emu68_api.config = *config;

    g_emu68_api.in_use = true;
    return &g_emu68_api;
}

void emu68_destroy(emu68_t *cpu)
{
    if (cpu != &g_emu68_api)
        return;

    memset(&g_emu68_api, 0, sizeof(g_emu68_api));
}

emu68_status_t emu68_set_bus(emu68_t *cpu, const emu68_bus_ops_t *ops,
                             void *user)
{
    if (!cpu || !ops)
        return EMU68_ERR_INVALID_ARG;

    cpu->bus = *ops;
    cpu->bus_user = user;
    return EMU68_OK;
}

emu68_status_t emu68_set_hle(emu68_t *cpu, const emu68_hle_ops_t *ops,
                             void *user)
{
    if (!cpu)
        return EMU68_ERR_INVALID_ARG;

    if (ops)
        cpu->hle = *ops;
    else
        memset(&cpu->hle, 0, sizeof(cpu->hle));
    cpu->hle_user = user;

    return EMU68_ERR_UNSUPPORTED;
}

emu68_status_t emu68_set_event_callback(emu68_t *cpu, emu68_event_fn fn,
                                        void *user)
{
    if (!cpu)
        return EMU68_ERR_INVALID_ARG;

    cpu->event_fn = fn;
    cpu->event_user = user;
    return EMU68_OK;
}

void emu68_reset(emu68_t *cpu)
{
    if (!cpu)
        return;

    cpu->stop_requested = false;
    cpu->run_active = false;
    cpu->run_cycles = 0;
    cpu->run_budget_cycles = 0;
    cpu->run_start_insn_count = 0;
    cpu->run_last_insn_count = 0;
    cpu->run_stop_reason = EMU68_STOP_CYCLES_EXHAUSTED;
    cpu->run_stop_pc = 0;
    cpu->run_stop_detail = 0;
    cpu->sync_pending = false;
    cpu->sync_addr = 0;

    if (!__m68k_state)
        return;

#ifdef BELLATRIX
    {
        extern uint32_t bellatrix_reset_isp;
        extern uint32_t bellatrix_reset_pc;
        __m68k_state->ISP.u32 = BE32(bellatrix_reset_isp);
        __m68k_state->PC = BE32(bellatrix_reset_pc);
    }
#endif
    __m68k_state->SR = BE16(SR_S | SR_IPL);
    __m68k_state->INT32 = 0;
#ifdef BELLATRIX
    __m68k_state->STOPPED = 0u;
#endif
    __m68k_state->CACR |= BE32(CACR_IE);
    cache_invalidate_all(ICACHE);
}

void emu68_set_irq_level(emu68_t *cpu, int level)
{
    uint8_t old_level;

    if (!cpu || cpu != &g_emu68_api || !cpu->in_use || !__m68k_state)
        return;

    if (level < 0)
        level = 0;
    if (level > 7)
        level = 7;

    old_level = __m68k_state->INT.IPL;
    __m68k_state->INT.IPL = (uint8_t)level;
    cpu->stats.irq_level_set_count++;
    if (old_level != (uint8_t)level)
        cpu->stats.irq_level_change_count++;
#ifdef BELLATRIX
    /* Rigel publishes the guest's persistent IPL in software; no physical ARM
     * IRQ is involved. INT.ARM is the PiStorm external line translated to
     * Amiga level 6, so asserting it would mix two interrupt domains. */
    __m68k_state->INT.ARM = 0;
    asm volatile("dmb ish" ::: "memory");
    /* This is an event wakeup, not a physical IRQ. It also covers a future
     * scheduler parking the JIT core in WFE. */
    asm volatile("dsb sy\n\tsev" ::: "memory");
#else
    if (level)
        __m68k_state->INT.ARM = 1;
#endif
}

emu68_run_result_t emu68_run_cycles(emu68_t *cpu, uint32_t max_cycles)
{
#if defined(BELLATRIX_EMU68_API_TRACE) && BELLATRIX_EMU68_API_TRACE
    static int logged_first_run;
#endif
    if (!cpu)
        return unsupported_result(cpu, EMU68_ERR_INVALID_ARG);
    if (cpu != &g_emu68_api || !cpu->in_use)
        return unsupported_result(cpu, EMU68_ERR_INVALID_ARG);
    if (!__m68k_state)
        return unsupported_result(cpu, EMU68_ERR_UNSUPPORTED);
    if (cpu->run_active)
        return unsupported_result(cpu, EMU68_ERR_BUSY);

    cpu->stats.run_call_count++;

    cpu->run_budget_cycles = max_cycles;
    cpu->run_cycles = 0;
    cpu->run_start_insn_count = 0;
    cpu->run_last_insn_count = 0;
    cpu->run_stop_reason = EMU68_STOP_CYCLES_EXHAUSTED;
    cpu->run_stop_pc = BE32(__m68k_state->PC);
    cpu->run_stop_detail = 0;

    if (max_cycles == 0u)
        return make_run_result(cpu);

    if (cpu->stop_requested) {
        cpu->run_stop_reason = EMU68_STOP_HOST_REQUEST;
        return make_run_result(cpu);
    }

    cpu->run_active = true;
#if defined(BELLATRIX_EMU68_API_TRACE) && BELLATRIX_EMU68_API_TRACE
    if (!logged_first_run) {
        logged_first_run = 1;
        kprintf("[EMU68-API] first run window budget=%u pc=%08x\n",
                (unsigned)max_cycles, (unsigned)cpu->run_stop_pc);
    }
#endif
#ifdef BELLATRIX
    MainLoopWindow();
#else
    MainLoop();
#endif

    return make_run_result(cpu);
}

emu68_run_result_t emu68_step(emu68_t *cpu)
{
    return emu68_run_cycles(cpu, 8u);
}

void emu68_request_stop(emu68_t *cpu)
{
    emu68_event_t event;

    if (!cpu)
        return;

    cpu->stop_requested = true;
    cpu->stats.stop_request_count++;

    memset(&event, 0, sizeof(event));
    event.type = EMU68_EVENT_STOP;
    if (__m68k_state)
        event.pc = BE32(__m68k_state->PC);
    emit_event(cpu, &event);
}

int emu68_api_dispatch_quantum_progress(uint64_t retired_instructions,
                                        uint32_t pc)
{
    emu68_t *cpu = &g_emu68_api;
    uint32_t cycles;

    if (!cpu->in_use || !cpu->run_active)
        return 0;

    if (cpu->run_start_insn_count == 0u)
        cpu->run_start_insn_count = retired_instructions;

    cycles = emu68_api_estimate_cycles(cpu, retired_instructions);
    cpu->run_cycles += cycles;
    cpu->run_stop_pc = pc;

#ifdef BELLATRIX
    if (__m68k_state && __m68k_state->STOPPED) {
        uint16_t sr = BE16(__m68k_state->SR);
        unsigned int mask = (unsigned int)((sr & SR_IPL) >> SRB_IPL);
        unsigned int level = (unsigned int)__m68k_state->INT.IPL;

        if (level > mask) {
            /* STOP was already retired and PC already names the next
             * instruction. Clear the state and let MainLoop deliver IPL. */
            __m68k_state->STOPPED = 0u;
            cpu->stats.stopped_wake_count++;
        } else {
            cpu->run_stop_reason = EMU68_STOP_STOPPED;
            cpu->stats.stopped_return_count++;
            cpu->run_active = false;
            return 1;
        }
    }
#endif

    if (cpu->sync_pending) {
        cpu->sync_pending = false;
        cpu->stats.run_sync_stop_count++;
        cpu->run_stop_reason = EMU68_STOP_SYNC_REQUIRED;
        cpu->run_stop_detail = cpu->sync_addr;
        cpu->run_active = false;
        return 1;
    }

    if (cpu->stop_requested) {
        cpu->run_stop_reason = EMU68_STOP_HOST_REQUEST;
        cpu->run_active = false;
        return 1;
    }

    if (cpu->run_cycles >= (uint64_t)cpu->run_budget_cycles) {
        cpu->run_stop_reason = EMU68_STOP_CYCLES_EXHAUSTED;
        cpu->stats.run_cycles_exhausted_count++;
        cpu->run_active = false;
        return 1;
    }

    return 0;
}

int emu68_api_sync_pending(void)
{
    return g_emu68_api.in_use && g_emu68_api.run_active &&
           g_emu68_api.sync_pending;
}

void emu68_get_state(emu68_t *cpu, emu68_state_t *out)
{
    unsigned int i;

    (void)cpu;

    if (!out)
        return;

    memset(out, 0, sizeof(*out));
    if (!__m68k_state)
        return;

    for (i = 0; i < 8u; i++) {
        out->d[i] = BE32(__m68k_state->D[i].u32);
        out->a[i] = BE32(__m68k_state->A[i].u32);
    }
    out->pc = BE32(__m68k_state->PC);
    out->sr = BE16(__m68k_state->SR);
    out->usp = BE32(__m68k_state->USP.u32);
    out->ssp = BE32(__m68k_state->ISP.u32);
    out->vbr = BE32(__m68k_state->VBR);
}

emu68_status_t emu68_set_state(emu68_t *cpu, const emu68_state_t *state)
{
    unsigned int i;

    (void)cpu;

    if (!state)
        return EMU68_ERR_INVALID_ARG;
    if (!__m68k_state)
        return EMU68_ERR_UNSUPPORTED;

    for (i = 0; i < 8u; i++) {
        __m68k_state->D[i].u32 = BE32(state->d[i]);
        __m68k_state->A[i].u32 = BE32(state->a[i]);
    }
    __m68k_state->PC = BE32(state->pc);
    __m68k_state->SR = BE16(state->sr);
    __m68k_state->USP.u32 = BE32(state->usp);
    __m68k_state->ISP.u32 = BE32(state->ssp);
    __m68k_state->VBR = BE32(state->vbr);
    cache_invalidate_all(ICACHE);

    return EMU68_OK;
}

void emu68_get_stats(emu68_t *cpu, emu68_stats_t *out)
{
    if (!cpu || !out)
        return;

    *out = cpu->stats;
}

void emu68_reset_stats(emu68_t *cpu)
{
    if (!cpu)
        return;

    memset(&cpu->stats, 0, sizeof(cpu->stats));
}

void emu68_invalidate_code_range(emu68_t *cpu, uint32_t addr, uint32_t size)
{
    emu68_event_t event;

    if (!cpu || size == 0u)
        return;

    cpu->stats.invalidate_count++;
    cache_invalidate_range(ICACHE, addr, size);

    memset(&event, 0, sizeof(event));
    event.type = EMU68_EVENT_JIT_INVALIDATE;
    event.addr = addr;
    event.size = size;
    if (__m68k_state)
        event.pc = BE32(__m68k_state->PC);
    emit_event(cpu, &event);
}

void emu68_invalidate_all_code(emu68_t *cpu)
{
    emu68_event_t event;

    if (!cpu)
        return;

    cpu->stats.invalidate_count++;
    cache_invalidate_all(ICACHE);

    memset(&event, 0, sizeof(event));
    event.type = EMU68_EVENT_JIT_INVALIDATE;
    event.size = 0xffffffffu;
    if (__m68k_state)
        event.pc = BE32(__m68k_state->PC);
    emit_event(cpu, &event);
}

int emu68_api_dispatch_bus_access(uint32_t addr, uint32_t *value,
                                  unsigned int size, int is_write,
                                  emu68_space_t space)
{
    emu68_t *cpu = &g_emu68_api;

    if (!cpu->in_use) {
        cpu->stats.bus_unhandled_count++;
        return 0;
    }

    if (is_write) {
        emu68_bus_write_t result;
        memset(&result, 0, sizeof(result));

        if (!value) {
            cpu->stats.bus_unhandled_count++;
            return 0;
        }

        if (size == 1u && cpu->bus.write8) {
            result = cpu->bus.write8(cpu->bus_user, addr, (uint8_t)*value, space);
        } else if (size == 2u && cpu->bus.write16) {
            result = cpu->bus.write16(cpu->bus_user, addr, (uint16_t)*value, space);
        } else if (size == 4u && cpu->bus.write32) {
            result = cpu->bus.write32(cpu->bus_user, addr, *value, space);
        } else {
            if (size != 1u && size != 2u && size != 4u)
                cpu->stats.unsupported_size_count++;
            cpu->stats.bus_unhandled_count++;
            return 0;
        }

        cpu->stats.bus_write_count++;
#if defined(BELLATRIX_EMU68_API_TRACE) && BELLATRIX_EMU68_API_TRACE
        if (!cpu->logged_first_write) {
            cpu->logged_first_write = true;
            kprintf("[EMU68-API] first bus write addr=%08x size=%u value=%08x pc=%08x\n",
                    (unsigned)addr, (unsigned)size, (unsigned)*value,
                    (unsigned)current_pc());
        }
#endif
        if (result.status == EMU68_BUS_SYNC_REQUIRED) {
            emu68_event_t event;
            cpu->stats.bus_sync_required_count++;
            if (cpu->run_active && !cpu->sync_pending) {
                cpu->sync_pending = true;
                cpu->sync_addr = addr;
            }
#if defined(BELLATRIX_EMU68_API_TRACE) && BELLATRIX_EMU68_API_TRACE
            if (!cpu->logged_first_sync) {
                cpu->logged_first_sync = true;
                kprintf("[EMU68-API] first sync-required addr=%08x size=%u value=%08x pc=%08x\n",
                        (unsigned)addr, (unsigned)size, (unsigned)*value,
                        (unsigned)current_pc());
            }
#endif
#if defined(BELLATRIX_EMU68_API_AUTODUMP) && BELLATRIX_EMU68_API_AUTODUMP
            if (!cpu->dumped_first_sync_stats) {
                cpu->dumped_first_sync_stats = true;
                kprintf("[EMU68-API] first-sync stats bus_r=%llu bus_w=%llu sync=%llu err=%llu unhandled=%llu bad_size=%llu stop=%llu inv=%llu\n",
                        (unsigned long long)cpu->stats.bus_read_count,
                        (unsigned long long)cpu->stats.bus_write_count,
                        (unsigned long long)cpu->stats.bus_sync_required_count,
                        (unsigned long long)cpu->stats.bus_error_count,
                        (unsigned long long)cpu->stats.bus_unhandled_count,
                        (unsigned long long)cpu->stats.unsupported_size_count,
                        (unsigned long long)cpu->stats.stop_request_count,
                        (unsigned long long)cpu->stats.invalidate_count);
            }
#endif
            memset(&event, 0, sizeof(event));
            event.type = EMU68_EVENT_SYNC_REQUIRED;
            event.addr = addr;
            event.value = *value;
            event.size = size;
            if (__m68k_state)
                event.pc = BE32(__m68k_state->PC);
            emit_event(cpu, &event);
        }

        if (result.status == EMU68_BUS_ERROR) {
            cpu->stats.bus_error_count++;
            return 0;
        }

        return 1;
    }

    {
        emu68_bus_read_t result;
        memset(&result, 0, sizeof(result));

        if (!value) {
            cpu->stats.bus_unhandled_count++;
            return 0;
        }

        if (size == 1u && cpu->bus.read8) {
            result = cpu->bus.read8(cpu->bus_user, addr, space);
        } else if (size == 2u && cpu->bus.read16) {
            result = cpu->bus.read16(cpu->bus_user, addr, space);
        } else if (size == 4u && cpu->bus.read32) {
            result = cpu->bus.read32(cpu->bus_user, addr, space);
        } else {
            if (size != 1u && size != 2u && size != 4u)
                cpu->stats.unsupported_size_count++;
            cpu->stats.bus_unhandled_count++;
            return 0;
        }

        cpu->stats.bus_read_count++;
#if defined(BELLATRIX_EMU68_API_TRACE) && BELLATRIX_EMU68_API_TRACE
        if (!cpu->logged_first_read) {
            cpu->logged_first_read = true;
            kprintf("[EMU68-API] first bus read addr=%08x size=%u value=%08x pc=%08x\n",
                    (unsigned)addr, (unsigned)size, (unsigned)result.value,
                    (unsigned)current_pc());
        }
#endif
        if (result.status == EMU68_BUS_ERROR) {
            cpu->stats.bus_error_count++;
            return 0;
        }

        *value = result.value;

        if (result.status == EMU68_BUS_SYNC_REQUIRED) {
            emu68_event_t event;
            cpu->stats.bus_sync_required_count++;
            if (cpu->run_active && !cpu->sync_pending) {
                cpu->sync_pending = true;
                cpu->sync_addr = addr;
            }
            memset(&event, 0, sizeof(event));
            event.type = EMU68_EVENT_SYNC_REQUIRED;
            event.addr = addr;
            event.value = result.value;
            event.size = size;
            if (__m68k_state)
                event.pc = BE32(__m68k_state->PC);
            emit_event(cpu, &event);
        }

        return 1;
    }
}
