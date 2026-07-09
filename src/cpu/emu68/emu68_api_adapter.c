#include "cpu/emu68/emu68_api.h"

#include "M68k.h"
#include "cache.h"
#include "support.h"

#include <string.h>

extern struct M68KState *__m68k_state;

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
    bool logged_first_read;
    bool logged_first_write;
    bool logged_first_sync;
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

static uint32_t current_pc(void)
{
    if (!__m68k_state)
        return 0;

    return BE32(__m68k_state->PC);
}

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
    __m68k_state->CACR |= BE32(CACR_IE);
    cache_invalidate_all(ICACHE);
}

void emu68_set_irq_level(emu68_t *cpu, int level)
{
    (void)cpu;

    if (!__m68k_state)
        return;

    if (level < 0)
        level = 0;
    if (level > 7)
        level = 7;

    __m68k_state->INT.IPL = (uint8_t)level;
    if (level)
        __m68k_state->INT.ARM = 1;
}

emu68_run_result_t emu68_run_cycles(emu68_t *cpu, uint32_t max_cycles)
{
    emu68_run_result_t result;

    if (!cpu)
        return unsupported_result(cpu, EMU68_ERR_INVALID_ARG);

    memset(&result, 0, sizeof(result));
    if (__m68k_state)
        result.pc = BE32(__m68k_state->PC);

    if (cpu->stop_requested) {
        result.reason = EMU68_STOP_HOST_REQUEST;
        return result;
    }

    (void)max_cycles;
    return unsupported_result(cpu, EMU68_ERR_UNSUPPORTED);
}

emu68_run_result_t emu68_step(emu68_t *cpu)
{
    return emu68_run_cycles(cpu, 1u);
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
        if (!cpu->logged_first_write) {
            cpu->logged_first_write = true;
            kprintf("[EMU68-API] first bus write addr=%08x size=%u value=%08x pc=%08x\n",
                    (unsigned)addr, (unsigned)size, (unsigned)*value,
                    (unsigned)current_pc());
        }
        if (result.status == EMU68_BUS_SYNC_REQUIRED) {
            emu68_event_t event;
            cpu->stats.bus_sync_required_count++;
            if (!cpu->logged_first_sync) {
                cpu->logged_first_sync = true;
                kprintf("[EMU68-API] first sync-required addr=%08x size=%u value=%08x pc=%08x\n",
                        (unsigned)addr, (unsigned)size, (unsigned)*value,
                        (unsigned)current_pc());
            }
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
        if (!cpu->logged_first_read) {
            cpu->logged_first_read = true;
            kprintf("[EMU68-API] first bus read addr=%08x size=%u value=%08x pc=%08x\n",
                    (unsigned)addr, (unsigned)size, (unsigned)result.value,
                    (unsigned)current_pc());
        }
        if (result.status == EMU68_BUS_ERROR) {
            cpu->stats.bus_error_count++;
            return 0;
        }

        *value = result.value;

        if (result.status == EMU68_BUS_SYNC_REQUIRED) {
            emu68_event_t event;
            cpu->stats.bus_sync_required_count++;
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
