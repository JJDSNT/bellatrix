#include "cpu/emu68/emu68_machine_internal.h"
#include "cpu/emu68/emu68_machine_platform.h"

#include <string.h>

#define EMU68_MACHINE_MAX_REGIONS 256u
#define EMU68_MACHINE_NATIVE_FRAME_SIZE 800u
#define EMU68_MACHINE_VALID_FLAGS                                             \
    (EMU68_REGION_READ | EMU68_REGION_WRITE | EMU68_REGION_EXECUTE |          \
     EMU68_REGION_CACHEABLE)

struct emu68_machine_region {
    uint32_t base;
    uint64_t size;
    uintptr_t host_base;
    uint32_t region_id;
    uint32_t flags;
    emu68_region_kind_t kind;
};

struct emu68_cpu {
    emu68_execution_mode_t mode;
    emu68_machine_ops_t ops;
    void *opaque;
    struct emu68_machine_region regions[EMU68_MACHINE_MAX_REGIONS];
    uint16_t region_count;
    uint8_t active;
    uint8_t running;
    uint8_t pending;
    uint8_t completion_ready;
    uint8_t bus_error_pending;
    uint8_t stopped;
    uint64_t next_sequence;
    emu68_bus_access_t pending_access;
    uintptr_t native_resume;
    uint64_t run_budget;
    uint64_t run_start_instructions;
    uint64_t run_instructions;
    uint64_t run_cycles;
    uint32_t run_pc;
    uint8_t native_frame[EMU68_MACHINE_NATIVE_FRAME_SIZE]
        __attribute__((aligned(16)));
    volatile uint32_t stop_requested;
};

uint8_t *emu68_machine_resume_frame;
uintptr_t emu68_machine_resume_address;
uint64_t emu68_machine_resume_value;
uint64_t emu68_machine_resume_outcome;
uint32_t emu68_machine_bridge_address;
uint64_t emu68_machine_bridge_write_value;
uint32_t emu68_machine_bridge_metadata;
uintptr_t emu68_machine_bridge_tu_return;
uint64_t emu68_machine_bridge_read_value;
uint64_t emu68_machine_bridge_outcome;

uint8_t emu68_machine_read_pages[EMU68_MACHINE_PAGE_COUNT];
uint8_t emu68_machine_write_pages[EMU68_MACHINE_PAGE_COUNT];

static struct emu68_cpu machine_cpu;

static int public_struct_valid(uint32_t version, size_t actual, size_t required)
{
    return version == EMU68_MACHINE_ABI_VERSION && actual >= required;
}

static int valid_cpu(const emu68_cpu_t *cpu)
{
    return cpu == &machine_cpu && machine_cpu.active;
}

static int valid_range(uint32_t base, uint64_t size)
{
    return size != 0u && size <= UINT64_C(0x100000000) &&
           (uint64_t)base + size <= UINT64_C(0x100000000);
}

static uint64_t region_end(const struct emu68_machine_region *region)
{
    return (uint64_t)region->base + region->size;
}

static int region_contains(const struct emu68_machine_region *region,
                           uint32_t address, uint8_t width)
{
    uint64_t end = (uint64_t)address + width;

    return width != 0u && (uint64_t)address >= region->base &&
           end <= region_end(region) && end <= UINT64_C(0x100000000);
}

static const struct emu68_machine_region *find_region(uint32_t address,
                                                       uint8_t width)
{
    unsigned int i;

    for (i = 0; i < machine_cpu.region_count; ++i) {
        if (region_contains(&machine_cpu.regions[i], address, width))
            return &machine_cpu.regions[i];
    }
    return NULL;
}

static uint8_t classify_page(uint32_t page, uint32_t required_flag)
{
    uint32_t base = page << EMU68_MACHINE_PAGE_SHIFT;
    const struct emu68_machine_region *region =
        find_region(base, (uint8_t)1u);
    uint64_t end = (uint64_t)base + EMU68_MACHINE_PAGE_SIZE;

    if (!region)
        return EMU68_PAGE_FAULT;
    if (end > region_end(region))
        return EMU68_PAGE_MIXED;
    if ((region->flags & required_flag) == 0u)
        return EMU68_PAGE_FAULT;
    if (region->kind == EMU68_REGION_DIRECT)
        return EMU68_PAGE_DIRECT;
    if (region->kind == EMU68_REGION_EXTERNAL)
        return EMU68_PAGE_EXTERNAL;
    return EMU68_PAGE_FAULT;
}

static void rebuild_pages(void)
{
    uint32_t page;

    for (page = 0; page < EMU68_MACHINE_PAGE_COUNT; ++page) {
        emu68_machine_read_pages[page] =
            classify_page(page, EMU68_REGION_READ);
        emu68_machine_write_pages[page] =
            classify_page(page, EMU68_REGION_WRITE);
    }
}

static int overlaps_existing(uint32_t base, uint64_t size)
{
    uint64_t end = (uint64_t)base + size;
    unsigned int i;

    for (i = 0; i < machine_cpu.region_count; ++i) {
        const struct emu68_machine_region *other = &machine_cpu.regions[i];
        if ((uint64_t)base < region_end(other) && end > other->base)
            return 1;
    }
    return 0;
}

static emu68_status_t validate_new_region(
    const struct emu68_machine_region *region)
{
    if (!valid_range(region->base, region->size))
        return EMU68_ERR_INVALID_ARGUMENT;
    if ((region->flags & ~EMU68_MACHINE_VALID_FLAGS) != 0u)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (overlaps_existing(region->base, region->size))
        return EMU68_ERR_OVERLAP;
    if (machine_cpu.region_count == EMU68_MACHINE_MAX_REGIONS)
        return EMU68_ERR_INTERNAL;

    return EMU68_OK;
}

static void insert_region(const struct emu68_machine_region *region)
{
    unsigned int insert = machine_cpu.region_count;

    while (insert != 0u &&
           machine_cpu.regions[insert - 1u].base > region->base) {
        machine_cpu.regions[insert] = machine_cpu.regions[insert - 1u];
        --insert;
    }
    machine_cpu.regions[insert] = *region;
    ++machine_cpu.region_count;
    rebuild_pages();
    emu68_machine_platform_invalidate_all();
}

emu68_status_t emu68_machine_create(const emu68_machine_config_t *config,
                                    emu68_cpu_t **out_cpu)
{
    if (!config || !out_cpu)
        return EMU68_ERR_INVALID_ARGUMENT;
    *out_cpu = NULL;
    if (!public_struct_valid(config->abi_version, config->struct_size,
                             sizeof(*config)))
        return EMU68_ERR_ABI_MISMATCH;
    if (config->execution_mode != EMU68_EXEC_SYNCHRONOUS &&
        config->execution_mode != EMU68_EXEC_COOPERATIVE)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (config->execution_mode == EMU68_EXEC_SYNCHRONOUS) {
        if (!config->ops ||
            !public_struct_valid(config->ops->abi_version,
                                 config->ops->struct_size,
                                 sizeof(*config->ops)) ||
            !config->ops->bus_access)
            return EMU68_ERR_INVALID_ARGUMENT;
    }
    if (machine_cpu.active)
        return EMU68_ERR_BUSY;

    memset(&machine_cpu, 0, sizeof(machine_cpu));
    machine_cpu.mode = config->execution_mode;
    if (config->ops)
        machine_cpu.ops = *config->ops;
    machine_cpu.opaque = config->opaque;
    machine_cpu.next_sequence = 1u;
    machine_cpu.active = 1u;
    rebuild_pages();
    *out_cpu = &machine_cpu;
    return EMU68_OK;
}

void emu68_machine_destroy(emu68_cpu_t *cpu)
{
    unsigned int i;

    if (!valid_cpu(cpu) || machine_cpu.running)
        return;
    for (i = 0; i < machine_cpu.region_count; ++i) {
        const struct emu68_machine_region *region = &machine_cpu.regions[i];
        if (region->kind == EMU68_REGION_DIRECT)
            emu68_machine_platform_unmap_direct(region->base, region->size);
    }
    memset(&machine_cpu, 0, sizeof(machine_cpu));
    memset(emu68_machine_read_pages, EMU68_PAGE_FAULT,
           sizeof(emu68_machine_read_pages));
    memset(emu68_machine_write_pages, EMU68_PAGE_FAULT,
           sizeof(emu68_machine_write_pages));
}

emu68_status_t emu68_machine_map_direct(
    emu68_cpu_t *cpu, const emu68_direct_region_t *public_region)
{
    struct emu68_machine_region region;
    emu68_status_t status;

    if (!valid_cpu(cpu) || !public_region)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    if (!public_struct_valid(public_region->abi_version,
                             public_region->struct_size,
                             sizeof(*public_region)))
        return EMU68_ERR_ABI_MISMATCH;
    if (!public_region->host_base ||
        (public_region->guest_base & 0xfffu) != 0u ||
        ((uintptr_t)public_region->host_base & 0xfffu) != 0u ||
        (public_region->size & 0xfffu) != 0u)
        return EMU68_ERR_INVALID_ARGUMENT;

    memset(&region, 0, sizeof(region));
    region.base = public_region->guest_base;
    region.size = public_region->size;
    region.host_base = (uintptr_t)public_region->host_base;
    region.flags = public_region->flags;
    region.kind = EMU68_REGION_DIRECT;
    status = validate_new_region(&region);
    if (status != EMU68_OK)
        return status;
    status = emu68_machine_platform_map_direct(
        region.base, region.size, (void *)region.host_base, region.flags);
    if (status != EMU68_OK)
        return status;
    insert_region(&region);
    return EMU68_OK;
}

emu68_status_t emu68_machine_map_external(
    emu68_cpu_t *cpu, const emu68_external_region_t *public_region)
{
    struct emu68_machine_region region;
    emu68_status_t status;

    if (!valid_cpu(cpu) || !public_region)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    if (!public_struct_valid(public_region->abi_version,
                             public_region->struct_size,
                             sizeof(*public_region)))
        return EMU68_ERR_ABI_MISMATCH;
    if ((public_region->flags &
         (EMU68_REGION_EXECUTE | EMU68_REGION_CACHEABLE)) != 0u)
        return EMU68_ERR_INVALID_ARGUMENT;

    memset(&region, 0, sizeof(region));
    region.base = public_region->guest_base;
    region.size = public_region->size;
    region.region_id = public_region->region_id;
    region.flags = public_region->flags;
    region.kind = EMU68_REGION_EXTERNAL;
    status = validate_new_region(&region);
    if (status != EMU68_OK)
        return status;
    insert_region(&region);
    return EMU68_OK;
}

emu68_status_t emu68_machine_map_unmapped(emu68_cpu_t *cpu,
                                          uint32_t guest_base, uint64_t size)
{
    struct emu68_machine_region region;
    emu68_status_t status;

    if (!valid_cpu(cpu))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    memset(&region, 0, sizeof(region));
    region.base = guest_base;
    region.size = size;
    region.kind = EMU68_REGION_UNMAPPED;
    status = validate_new_region(&region);
    if (status != EMU68_OK)
        return status;
    insert_region(&region);
    return EMU68_OK;
}

emu68_status_t emu68_machine_unmap(emu68_cpu_t *cpu, uint32_t guest_base,
                                   uint64_t size)
{
    unsigned int i;

    if (!valid_cpu(cpu) || !valid_range(guest_base, size))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    for (i = 0; i < machine_cpu.region_count; ++i) {
        struct emu68_machine_region *region = &machine_cpu.regions[i];
        if (region->base == guest_base && region->size == size) {
            unsigned int remaining = machine_cpu.region_count - i - 1u;
            if (region->kind == EMU68_REGION_DIRECT)
                emu68_machine_platform_unmap_direct(region->base,
                                                    region->size);
            if (remaining != 0u)
                memmove(region, region + 1, remaining * sizeof(*region));
            --machine_cpu.region_count;
            rebuild_pages();
            emu68_machine_platform_invalidate_all();
            return EMU68_OK;
        }
    }
    return EMU68_ERR_NOT_FOUND;
}

emu68_status_t emu68_machine_classify_access(
    uint32_t address, uint8_t width, emu68_access_kind_t kind,
    emu68_machine_access_result_t *result)
{
    const struct emu68_machine_region *region;
    uint32_t required;

    if (!result || (width != 1u && width != 2u && width != 4u &&
                    width != 8u && width != 16u) ||
        (kind != EMU68_ACCESS_READ && kind != EMU68_ACCESS_WRITE))
        return EMU68_ERR_INVALID_ARGUMENT;

    memset(result, 0, sizeof(*result));
    region = find_region(address, width);
    if (!region) {
        result->region_kind = EMU68_REGION_UNMAPPED;
        return EMU68_OK;
    }
    required = kind == EMU68_ACCESS_READ ? EMU68_REGION_READ :
                                           EMU68_REGION_WRITE;
    if ((region->flags & required) == 0u) {
        result->region_kind = EMU68_REGION_UNMAPPED;
        return EMU68_OK;
    }
    result->region_kind = region->kind;
    result->region_id = region->region_id;
    result->flags = region->flags;
    return EMU68_OK;
}

emu68_bus_result_t emu68_machine_dispatch_access(emu68_bus_access_t *access)
{
    emu68_machine_access_result_t classification;

    if (!machine_cpu.active || !access ||
        emu68_machine_classify_access(access->address, access->width,
                                      access->kind, &classification) != EMU68_OK)
        return EMU68_BUS_ERROR;
    if (classification.region_kind != EMU68_REGION_EXTERNAL)
        return EMU68_BUS_ERROR;

    access->abi_version = EMU68_MACHINE_ABI_VERSION;
    access->struct_size = sizeof(*access);
    access->region_id = classification.region_id;
    access->sequence = machine_cpu.next_sequence++;
    if (machine_cpu.mode == EMU68_EXEC_SYNCHRONOUS) {
        if (!machine_cpu.ops.bus_access)
            return EMU68_BUS_ERROR;
        access->result = machine_cpu.ops.bus_access(machine_cpu.opaque, access);
        return access->result;
    }

    if (machine_cpu.pending)
        return EMU68_BUS_ERROR;
    machine_cpu.pending_access = *access;
    machine_cpu.pending = 1u;
    machine_cpu.completion_ready = 0u;
    return EMU68_BUS_COMPLETE;
}

static uint8_t current_function_code(emu68_address_space_t space,
                                     uint32_t metadata)
{
    uint8_t explicit_fc =
        (uint8_t)((metadata & EMU68_BRIDGE_META_FC_MASK) >>
                  EMU68_BRIDGE_META_FC_SHIFT);
    uint32_t sr = 0u;

    if (explicit_fc != 0u)
        return explicit_fc;
#ifdef __aarch64__
    __asm__ volatile("mrs %0, TPIDR_EL0" : "=r"(sr));
#endif
    if (space == EMU68_SPACE_CPU)
        return 7u;
    return (uint8_t)(((sr & 0x2000u) ? 4u : 0u) |
                     (space == EMU68_SPACE_PROGRAM ? 2u : 1u));
}

emu68_machine_bridge_result_t emu68_machine_bridge_dispatch(
    uint32_t address, uint64_t value, uint32_t metadata,
    uintptr_t native_resume, const void *native_frame)
{
    emu68_machine_bridge_result_t bridge = {
        .value = 0u,
        .outcome = EMU68_BRIDGE_BUS_ERROR,
    };
    emu68_bus_access_t access;
    emu68_machine_access_result_t classification;
    emu68_access_kind_t kind =
        (metadata & EMU68_BRIDGE_META_WRITE) ? EMU68_ACCESS_WRITE :
                                                EMU68_ACCESS_READ;
    emu68_address_space_t space = (emu68_address_space_t)(
        (metadata & EMU68_BRIDGE_META_SPACE_MASK) >>
        EMU68_BRIDGE_META_SPACE_SHIFT);
    uint8_t width = (uint8_t)(metadata & EMU68_BRIDGE_META_WIDTH_MASK);

    if (space > EMU68_SPACE_CPU ||
        emu68_machine_classify_access(address, width, kind,
                                      &classification) != EMU68_OK ||
        classification.region_kind != EMU68_REGION_EXTERNAL) {
        machine_cpu.bus_error_pending = 1u;
        return bridge;
    }

    if (machine_cpu.mode == EMU68_EXEC_COOPERATIVE &&
        (!native_resume || !native_frame))
        return bridge;

    memset(&access, 0, sizeof(access));
    access.address = address;
    access.kind = kind;
    access.space = space;
    access.function_code = current_function_code(space, metadata);
    access.width = width;
    access.value_lo = value;
    if (emu68_machine_dispatch_access(&access) != EMU68_BUS_COMPLETE) {
        machine_cpu.bus_error_pending = 1u;
        return bridge;
    }
    if (machine_cpu.mode == EMU68_EXEC_COOPERATIVE) {
        machine_cpu.native_resume = native_resume;
        memcpy(machine_cpu.native_frame, native_frame,
               sizeof(machine_cpu.native_frame));
        bridge.outcome = EMU68_BRIDGE_PENDING;
        return bridge;
    }
    bridge.value = access.value_lo;
    bridge.outcome = access.result == EMU68_BUS_COMPLETE ?
                         EMU68_BRIDGE_COMPLETE : EMU68_BRIDGE_BUS_ERROR;
    if (bridge.outcome == EMU68_BRIDGE_BUS_ERROR)
        machine_cpu.bus_error_pending = 1u;
    return bridge;
}

int emu68_machine_prepare_native_resume(void)
{
    if (!machine_cpu.active || !machine_cpu.pending ||
        !machine_cpu.completion_ready || !machine_cpu.native_resume)
        return 0;

    emu68_machine_resume_frame = machine_cpu.native_frame;
    emu68_machine_resume_address = machine_cpu.native_resume;
    emu68_machine_resume_value = machine_cpu.pending_access.value_lo;
    emu68_machine_resume_outcome =
        machine_cpu.pending_access.result == EMU68_BUS_COMPLETE ?
            EMU68_BRIDGE_COMPLETE : EMU68_BRIDGE_BUS_ERROR;
    if (emu68_machine_resume_outcome == EMU68_BRIDGE_BUS_ERROR)
        machine_cpu.bus_error_pending = 1u;
    machine_cpu.pending = 0u;
    machine_cpu.completion_ready = 0u;
    machine_cpu.native_resume = 0u;
    return 1;
}

int emu68_machine_native_access_pending(void)
{
    return machine_cpu.active && machine_cpu.pending &&
           !machine_cpu.completion_ready;
}

int emu68_machine_native_bus_error_pending(void)
{
    return machine_cpu.active && machine_cpu.bus_error_pending;
}

int emu68_machine_dispatch_quantum_progress(uint64_t retired_instructions,
                                            uint32_t pc)
{
    uint64_t delta;
    int stopped = 0;

    if (!machine_cpu.active || !machine_cpu.running)
        return 0;
    if (retired_instructions < machine_cpu.run_start_instructions)
        retired_instructions = machine_cpu.run_start_instructions;
    delta = retired_instructions - machine_cpu.run_start_instructions;
    machine_cpu.run_instructions = delta;
    /* Emu68 currently exposes only retired instructions. This temporary
     * conversion keeps bounded execution functional while the translator's
     * per-opcode 68k cycle accumulator is introduced. The public contract is
     * not considered complete until that accumulator replaces this line. */
    machine_cpu.run_cycles = delta * 8u;
    machine_cpu.run_pc = pc;
    emu68_machine_platform_snapshot(NULL, NULL, &stopped);
    machine_cpu.stopped = stopped != 0;
    return machine_cpu.pending || machine_cpu.bus_error_pending ||
           machine_cpu.stopped ||
           __atomic_load_n(&machine_cpu.stop_requested, __ATOMIC_ACQUIRE) ||
           machine_cpu.run_cycles >= machine_cpu.run_budget;
}

int emu68_machine_runtime_active(void)
{
    return machine_cpu.active;
}

emu68_status_t emu68_machine_get_pending_access(
    emu68_cpu_t *cpu, emu68_bus_access_t *out_access)
{
    if (!valid_cpu(cpu) || !out_access)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (!machine_cpu.pending)
        return EMU68_ERR_NOT_FOUND;
    *out_access = machine_cpu.pending_access;
    return EMU68_OK;
}

emu68_status_t emu68_machine_run(emu68_cpu_t *cpu, uint64_t cycle_budget,
                                 emu68_run_result_t *result)
{
    uint64_t instructions = 0u;
    uint32_t pc = 0u;
    int stopped = 0;

    if (!valid_cpu(cpu) || !result)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (!public_struct_valid(result->abi_version, result->struct_size,
                             sizeof(*result)))
        return EMU68_ERR_ABI_MISMATCH;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;

    emu68_machine_platform_snapshot(&instructions, &pc, &stopped);
    memset(result, 0, sizeof(*result));
    result->abi_version = EMU68_MACHINE_ABI_VERSION;
    result->struct_size = sizeof(*result);
    result->pc = pc;
    if (machine_cpu.pending && !machine_cpu.completion_ready) {
        result->reason = EMU68_STOP_EXTERNAL_ACCESS;
        return EMU68_OK;
    }
    if (machine_cpu.bus_error_pending) {
        result->reason = EMU68_STOP_BUS_ERROR;
        return EMU68_OK;
    }
    if (__atomic_load_n(&machine_cpu.stop_requested, __ATOMIC_ACQUIRE)) {
        result->reason = EMU68_STOP_REQUESTED;
        return EMU68_OK;
    }
    if (stopped) {
        result->reason = EMU68_STOP_STOPPED;
        return EMU68_OK;
    }
    if (cycle_budget == 0u) {
        result->reason = EMU68_STOP_BUDGET;
        return EMU68_OK;
    }

    machine_cpu.run_budget = cycle_budget;
    machine_cpu.run_start_instructions = instructions;
    machine_cpu.run_instructions = 0u;
    machine_cpu.run_cycles = 0u;
    machine_cpu.run_pc = pc;
    machine_cpu.stopped = 0u;
    machine_cpu.running = 1u;
    emu68_machine_platform_run();
    machine_cpu.running = 0u;

    emu68_machine_platform_snapshot(&instructions, &pc, &stopped);
    if (instructions >= machine_cpu.run_start_instructions)
        machine_cpu.run_instructions =
            instructions - machine_cpu.run_start_instructions;
    machine_cpu.run_cycles = machine_cpu.run_instructions * 8u;
    result->cycles_executed = machine_cpu.run_cycles;
    result->instructions_executed = machine_cpu.run_instructions;
    result->pc = pc;
    if (machine_cpu.pending && !machine_cpu.completion_ready)
        result->reason = EMU68_STOP_EXTERNAL_ACCESS;
    else if (machine_cpu.bus_error_pending)
        result->reason = EMU68_STOP_BUS_ERROR;
    else if (__atomic_load_n(&machine_cpu.stop_requested, __ATOMIC_ACQUIRE))
        result->reason = EMU68_STOP_REQUESTED;
    else if (stopped)
        result->reason = EMU68_STOP_STOPPED;
    else
        result->reason = EMU68_STOP_BUDGET;
    return EMU68_OK;
}

static int completion_identity_matches(const emu68_bus_access_t *completion)
{
    const emu68_bus_access_t *pending = &machine_cpu.pending_access;

    return completion->sequence == pending->sequence &&
           completion->address == pending->address &&
           completion->region_id == pending->region_id &&
           completion->kind == pending->kind &&
           completion->space == pending->space &&
           completion->function_code == pending->function_code &&
           completion->width == pending->width;
}

emu68_status_t emu68_machine_complete_access(
    emu68_cpu_t *cpu, const emu68_bus_access_t *completion)
{
    if (!valid_cpu(cpu) || !completion)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (!public_struct_valid(completion->abi_version,
                             completion->struct_size,
                             sizeof(*completion)))
        return EMU68_ERR_ABI_MISMATCH;
    if (machine_cpu.mode != EMU68_EXEC_COOPERATIVE || !machine_cpu.pending)
        return EMU68_ERR_NOT_FOUND;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    if (!completion_identity_matches(completion))
        return EMU68_ERR_ACCESS;
    if (completion->result != EMU68_BUS_COMPLETE &&
        completion->result != EMU68_BUS_ERROR)
        return EMU68_ERR_INVALID_ARGUMENT;

    machine_cpu.pending_access.result = completion->result;
    machine_cpu.pending_access.value_lo = completion->value_lo;
    machine_cpu.pending_access.value_hi = completion->value_hi;
    machine_cpu.completion_ready = 1u;
    return EMU68_OK;
}

emu68_status_t emu68_machine_reset(emu68_cpu_t *cpu,
                                   const emu68_reset_state_t *state)
{
    if (!valid_cpu(cpu) || !state)
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    if (!public_struct_valid(state->abi_version, state->struct_size,
                             sizeof(*state)))
        return EMU68_ERR_ABI_MISMATCH;

    machine_cpu.pending = 0u;
    machine_cpu.completion_ready = 0u;
    machine_cpu.native_resume = 0u;
    machine_cpu.bus_error_pending = 0u;
    __atomic_store_n(&machine_cpu.stop_requested, 0u, __ATOMIC_RELEASE);
    return emu68_machine_platform_reset(state->initial_ssp,
                                        state->initial_pc);
}

void emu68_machine_request_stop(emu68_cpu_t *cpu)
{
    if (valid_cpu(cpu)) {
        __atomic_store_n(&machine_cpu.stop_requested, 1u, __ATOMIC_RELEASE);
        emu68_machine_platform_wake();
    }
}

emu68_status_t emu68_machine_set_ipl(emu68_cpu_t *cpu, unsigned level)
{
    if (!valid_cpu(cpu))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (level > 7u)
        return EMU68_ERR_INVALID_ARGUMENT;
    return emu68_machine_platform_set_ipl(level);
}

emu68_status_t emu68_machine_invalidate_code(emu68_cpu_t *cpu,
                                              uint32_t guest_base,
                                              uint64_t size)
{
    if (!valid_cpu(cpu) || !valid_range(guest_base, size))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    emu68_machine_platform_invalidate(guest_base, size);
    return EMU68_OK;
}

emu68_status_t emu68_machine_invalidate_all_code(emu68_cpu_t *cpu)
{
    if (!valid_cpu(cpu))
        return EMU68_ERR_INVALID_ARGUMENT;
    if (machine_cpu.running)
        return EMU68_ERR_BUSY;
    emu68_machine_platform_invalidate_all();
    return EMU68_OK;
}
