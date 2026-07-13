/*
 * Public machine-host API for Emu68.
 *
 * This interface deliberately contains no platform, MMU, exception-vector or
 * JIT implementation details.  See docs/emu68_public_api.md in Bellatrix for
 * the normative contract.
 */
#ifndef EMU68_MACHINE_H
#define EMU68_MACHINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EMU68_MACHINE_ABI_VERSION 1u

typedef struct emu68_cpu emu68_cpu_t;

typedef enum emu68_execution_mode {
    EMU68_EXEC_SYNCHRONOUS = 0,
    EMU68_EXEC_COOPERATIVE = 1
} emu68_execution_mode_t;

typedef enum emu68_status {
    EMU68_OK = 0,
    EMU68_ERR_INVALID_ARGUMENT,
    EMU68_ERR_ABI_MISMATCH,
    EMU68_ERR_BUSY,
    EMU68_ERR_OVERLAP,
    EMU68_ERR_NOT_FOUND,
    EMU68_ERR_ACCESS,
    EMU68_ERR_INTERNAL
} emu68_status_t;

typedef enum emu68_region_kind {
    EMU68_REGION_DIRECT = 0,
    EMU68_REGION_EXTERNAL = 1,
    EMU68_REGION_UNMAPPED = 2
} emu68_region_kind_t;

enum {
    EMU68_REGION_READ = 1u << 0,
    EMU68_REGION_WRITE = 1u << 1,
    EMU68_REGION_EXECUTE = 1u << 2,
    EMU68_REGION_CACHEABLE = 1u << 3
};

typedef enum emu68_access_kind {
    EMU68_ACCESS_READ = 0,
    EMU68_ACCESS_WRITE = 1
} emu68_access_kind_t;

typedef enum emu68_address_space {
    EMU68_SPACE_DATA = 0,
    EMU68_SPACE_PROGRAM = 1,
    EMU68_SPACE_CPU = 2
} emu68_address_space_t;

typedef enum emu68_bus_result {
    EMU68_BUS_COMPLETE = 0,
    EMU68_BUS_ERROR = 1
} emu68_bus_result_t;

typedef enum emu68_stop_reason {
    EMU68_STOP_BUDGET = 0,
    EMU68_STOP_EXTERNAL_ACCESS,
    EMU68_STOP_STOPPED,
    EMU68_STOP_REQUESTED,
    EMU68_STOP_BUS_ERROR,
    EMU68_STOP_FATAL
} emu68_stop_reason_t;

typedef struct emu68_bus_access {
    uint32_t abi_version;
    size_t struct_size;
    uint64_t sequence;
    uint32_t address;
    uint32_t region_id;
    emu68_access_kind_t kind;
    emu68_address_space_t space;
    uint8_t function_code;
    uint8_t width;
    emu68_bus_result_t result;
    uint8_t reserved[2];
    uint64_t value_lo;
    uint64_t value_hi;
} emu68_bus_access_t;

typedef emu68_bus_result_t (*emu68_bus_access_fn)(
    void *opaque, emu68_bus_access_t *access);
typedef void (*emu68_progress_fn)(
    void *opaque, uint64_t cycle_delta, uint64_t instruction_delta,
    uint32_t pc);

typedef struct emu68_machine_ops {
    uint32_t abi_version;
    size_t struct_size;
    emu68_bus_access_fn bus_access;
    emu68_progress_fn progress;
} emu68_machine_ops_t;

typedef struct emu68_machine_config {
    uint32_t abi_version;
    size_t struct_size;
    emu68_execution_mode_t execution_mode;
    const emu68_machine_ops_t *ops;
    void *opaque;
} emu68_machine_config_t;

typedef struct emu68_direct_region {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t guest_base;
    uint64_t size;
    void *host_base;
    uint32_t flags;
} emu68_direct_region_t;

typedef struct emu68_external_region {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t guest_base;
    uint64_t size;
    uint32_t region_id;
    uint32_t flags;
} emu68_external_region_t;

typedef struct emu68_run_result {
    uint32_t abi_version;
    size_t struct_size;
    emu68_stop_reason_t reason;
    uint64_t cycles_executed;
    uint64_t instructions_executed;
    uint32_t pc;
    uint32_t detail;
} emu68_run_result_t;

typedef struct emu68_reset_state {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t initial_ssp;
    uint32_t initial_pc;
} emu68_reset_state_t;

emu68_status_t emu68_machine_create(
    const emu68_machine_config_t *config, emu68_cpu_t **out_cpu);
void emu68_machine_destroy(emu68_cpu_t *cpu);

emu68_status_t emu68_machine_map_direct(
    emu68_cpu_t *cpu, const emu68_direct_region_t *region);
emu68_status_t emu68_machine_map_external(
    emu68_cpu_t *cpu, const emu68_external_region_t *region);
emu68_status_t emu68_machine_map_unmapped(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);
emu68_status_t emu68_machine_unmap(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);

emu68_status_t emu68_machine_run(
    emu68_cpu_t *cpu, uint64_t cycle_budget, emu68_run_result_t *result);
emu68_status_t emu68_machine_get_pending_access(
    emu68_cpu_t *cpu, emu68_bus_access_t *out_access);
emu68_status_t emu68_machine_complete_access(
    emu68_cpu_t *cpu, const emu68_bus_access_t *completion);

emu68_status_t emu68_machine_reset(
    emu68_cpu_t *cpu, const emu68_reset_state_t *state);
void emu68_machine_request_stop(emu68_cpu_t *cpu);
emu68_status_t emu68_machine_set_ipl(emu68_cpu_t *cpu, unsigned level);

emu68_status_t emu68_machine_invalidate_code(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);
emu68_status_t emu68_machine_invalidate_all_code(emu68_cpu_t *cpu);

#ifdef __cplusplus
}
#endif

#endif
