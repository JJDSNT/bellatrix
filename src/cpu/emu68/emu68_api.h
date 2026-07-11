#ifndef BELLATRIX_EMU68_API_H
#define BELLATRIX_EMU68_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU68_API_VERSION 2u

typedef struct emu68 emu68_t;

typedef enum {
    EMU68_OK = 0,
    EMU68_ERR_INVALID_ARG,
    EMU68_ERR_UNSUPPORTED,
    EMU68_ERR_BUSY,
    EMU68_ERR_BUS,
    EMU68_ERR_INTERNAL
} emu68_status_t;

typedef enum {
    EMU68_SPACE_DATA = 0,
    EMU68_SPACE_PROGRAM,
    EMU68_SPACE_CPU
} emu68_space_t;

typedef enum {
    EMU68_BUS_OK = 0,
    EMU68_BUS_SYNC_REQUIRED,
    EMU68_BUS_ERROR
} emu68_bus_status_t;

typedef struct {
    emu68_bus_status_t status;
    uint32_t value;
} emu68_bus_read_t;

typedef struct {
    emu68_bus_status_t status;
} emu68_bus_write_t;

typedef struct {
    emu68_bus_read_t (*read8)(void *user, uint32_t addr, emu68_space_t space);
    emu68_bus_read_t (*read16)(void *user, uint32_t addr, emu68_space_t space);
    emu68_bus_read_t (*read32)(void *user, uint32_t addr, emu68_space_t space);

    emu68_bus_write_t (*write8)(void *user, uint32_t addr, uint8_t value,
                                emu68_space_t space);
    emu68_bus_write_t (*write16)(void *user, uint32_t addr, uint16_t value,
                                 emu68_space_t space);
    emu68_bus_write_t (*write32)(void *user, uint32_t addr, uint32_t value,
                                 emu68_space_t space);
} emu68_bus_ops_t;

typedef struct {
    bool enable_trace;
    bool enable_hle;
    uint32_t initial_cache_size;
} emu68_config_t;

typedef enum {
    EMU68_STOP_CYCLES_EXHAUSTED = 0,
    EMU68_STOP_SYNC_REQUIRED,
    EMU68_STOP_EXCEPTION,
    EMU68_STOP_STOPPED,
    EMU68_STOP_HALTED,
    EMU68_STOP_HOST_REQUEST,
    EMU68_STOP_UNSUPPORTED
} emu68_stop_reason_t;

typedef struct {
    emu68_stop_reason_t reason;
    uint64_t cycles_run;
    uint32_t pc;
    uint32_t detail;
} emu68_run_result_t;

typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint16_t sr;
    uint32_t usp;
    uint32_t ssp;
    uint32_t vbr;
} emu68_state_t;

typedef struct {
    uint64_t run_call_count;
    uint64_t run_cycles_exhausted_count;
    uint64_t bus_read_count;
    uint64_t bus_write_count;
    uint64_t bus_sync_required_count;
    uint64_t run_sync_stop_count;
    uint64_t stopped_return_count;
    uint64_t stopped_wake_count;
    uint64_t irq_level_set_count;
    uint64_t irq_level_change_count;
    uint64_t bus_error_count;
    uint64_t bus_unhandled_count;
    uint64_t unsupported_size_count;
    uint64_t stop_request_count;
    uint64_t invalidate_count;
} emu68_stats_t;

typedef struct {
    bool (*on_trap)(void *user, emu68_t *cpu, uint8_t trap_num);
    bool (*on_aline)(void *user, emu68_t *cpu, uint16_t opcode);
    bool (*on_fline)(void *user, emu68_t *cpu, uint16_t opcode);
    bool (*on_illegal)(void *user, emu68_t *cpu, uint16_t opcode);
} emu68_hle_ops_t;

typedef enum {
    EMU68_EVENT_EXCEPTION = 0,
    EMU68_EVENT_MMIO_ACCESS,
    EMU68_EVENT_SYNC_REQUIRED,
    EMU68_EVENT_JIT_INVALIDATE,
    EMU68_EVENT_STOP
} emu68_event_type_t;

typedef struct {
    emu68_event_type_t type;
    uint32_t pc;
    uint32_t addr;
    uint32_t value;
    uint32_t size;
} emu68_event_t;

typedef void (*emu68_event_fn)(void *user, const emu68_event_t *event);

uint32_t emu68_api_version(void);

emu68_t *emu68_create(const emu68_config_t *config);
void emu68_destroy(emu68_t *cpu);

emu68_status_t emu68_set_bus(emu68_t *cpu, const emu68_bus_ops_t *ops,
                             void *user);
emu68_status_t emu68_set_hle(emu68_t *cpu, const emu68_hle_ops_t *ops,
                             void *user);
emu68_status_t emu68_set_event_callback(emu68_t *cpu, emu68_event_fn fn,
                                        void *user);

void emu68_reset(emu68_t *cpu);
void emu68_set_irq_level(emu68_t *cpu, int level);

emu68_run_result_t emu68_run_cycles(emu68_t *cpu, uint32_t max_cycles);
emu68_run_result_t emu68_step(emu68_t *cpu);
void emu68_request_stop(emu68_t *cpu);

void emu68_get_state(emu68_t *cpu, emu68_state_t *out);
emu68_status_t emu68_set_state(emu68_t *cpu, const emu68_state_t *state);

void emu68_get_stats(emu68_t *cpu, emu68_stats_t *out);
void emu68_reset_stats(emu68_t *cpu);

void emu68_invalidate_code_range(emu68_t *cpu, uint32_t addr, uint32_t size);
void emu68_invalidate_all_code(emu68_t *cpu);

int emu68_api_dispatch_bus_access(uint32_t addr, uint32_t *value,
                                  unsigned int size, int is_write,
                                  emu68_space_t space);
int emu68_api_dispatch_quantum_progress(uint64_t retired_instructions,
                                        uint32_t pc);
int emu68_api_sync_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_EMU68_API_H */
