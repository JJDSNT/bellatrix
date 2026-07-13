#include "cpu/emu68/emu68_machine.h"
#include "cpu/emu68/emu68_machine_internal.h"
#include "cpu/emu68/emu68_machine_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned map_calls;
static unsigned unmap_calls;
static unsigned invalidate_calls;
static unsigned reset_calls;
static unsigned wake_calls;
static unsigned ipl_value;
static unsigned callback_calls;

static void fail(const char *expression, int line)
{
    fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    exit(1);
}

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression))                                                    \
            fail(#expression, __LINE__);                                      \
    } while (0)

emu68_status_t emu68_machine_platform_map_direct(
    uint32_t guest_base, uint64_t size, void *host_base, uint32_t flags)
{
    (void)guest_base;
    (void)size;
    (void)host_base;
    (void)flags;
    ++map_calls;
    return EMU68_OK;
}

void emu68_machine_platform_unmap_direct(uint32_t guest_base, uint64_t size)
{
    (void)guest_base;
    (void)size;
    ++unmap_calls;
}

void emu68_machine_platform_invalidate(uint32_t guest_base, uint64_t size)
{
    (void)guest_base;
    (void)size;
    ++invalidate_calls;
}

void emu68_machine_platform_invalidate_all(void)
{
    ++invalidate_calls;
}

emu68_status_t emu68_machine_platform_reset(uint32_t initial_ssp,
                                            uint32_t initial_pc)
{
    CHECK(initial_ssp == 0x1000u);
    CHECK(initial_pc == 0x2000u);
    ++reset_calls;
    return EMU68_OK;
}

emu68_status_t emu68_machine_platform_set_ipl(unsigned level)
{
    ipl_value = level;
    return EMU68_OK;
}

void emu68_machine_platform_wake(void)
{
    ++wake_calls;
}

static emu68_bus_result_t synchronous_access(void *opaque,
                                             emu68_bus_access_t *access)
{
    CHECK(opaque == (void *)0x1234u);
    CHECK(access->region_id == 77u);
    ++callback_calls;
    if (access->kind == EMU68_ACCESS_READ)
        access->value_lo = 0xfeedu;
    return EMU68_BUS_COMPLETE;
}

static emu68_cpu_t *create_cpu(emu68_execution_mode_t mode)
{
    static const emu68_machine_ops_t ops = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(ops),
        .bus_access = synchronous_access,
    };
    emu68_machine_config_t config = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(config),
        .execution_mode = mode,
        .ops = mode == EMU68_EXEC_SYNCHRONOUS ? &ops : NULL,
        .opaque = (void *)0x1234u,
    };
    emu68_cpu_t *cpu = NULL;

    CHECK(emu68_machine_create(&config, &cpu) == EMU68_OK);
    CHECK(cpu != NULL);
    return cpu;
}

static void test_abi_and_singleton(void)
{
    emu68_machine_config_t config;
    emu68_cpu_t *cpu = (emu68_cpu_t *)1;
    emu68_cpu_t *second = (emu68_cpu_t *)1;

    memset(&config, 0, sizeof(config));
    config.abi_version = EMU68_MACHINE_ABI_VERSION + 1u;
    config.struct_size = sizeof(config);
    CHECK(emu68_machine_create(&config, &cpu) == EMU68_ERR_ABI_MISMATCH);
    CHECK(cpu == NULL);

    cpu = create_cpu(EMU68_EXEC_COOPERATIVE);
    CHECK(emu68_machine_create(&config, &second) == EMU68_ERR_ABI_MISMATCH);
    CHECK(second == NULL);
    emu68_machine_destroy(cpu);
}

static void test_regions_and_sync_access(void)
{
    static uint8_t direct_memory[0x10000] __attribute__((aligned(4096)));
    emu68_cpu_t *cpu = create_cpu(EMU68_EXEC_SYNCHRONOUS);
    emu68_direct_region_t direct = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(direct),
        .guest_base = 0x00010000u,
        .size = sizeof(direct_memory),
        .host_base = direct_memory,
        .flags = EMU68_REGION_READ | EMU68_REGION_WRITE |
                 EMU68_REGION_EXECUTE | EMU68_REGION_CACHEABLE,
    };
    emu68_external_region_t external = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(external),
        .guest_base = 0x00dff000u,
        .size = 0x200u,
        .region_id = 77u,
        .flags = EMU68_REGION_READ | EMU68_REGION_WRITE,
    };
    emu68_machine_access_result_t classification;
    emu68_bus_access_t access;

    CHECK(emu68_machine_map_direct(cpu, &direct) == EMU68_OK);
    CHECK(map_calls == 1u);
    CHECK(emu68_machine_read_pages[1] == EMU68_PAGE_DIRECT);
    CHECK(emu68_machine_write_pages[1] == EMU68_PAGE_DIRECT);
    CHECK(emu68_machine_map_direct(cpu, &direct) == EMU68_ERR_OVERLAP);
    CHECK(map_calls == 1u);

    CHECK(emu68_machine_map_external(cpu, &external) == EMU68_OK);
    CHECK(emu68_machine_classify_access(0x00dff020u, 4u,
                                        EMU68_ACCESS_READ,
                                        &classification) == EMU68_OK);
    CHECK(classification.region_kind == EMU68_REGION_EXTERNAL);
    CHECK(classification.region_id == 77u);
    CHECK(emu68_machine_classify_access(0x00dff1ffu, 2u,
                                        EMU68_ACCESS_READ,
                                        &classification) == EMU68_OK);
    CHECK(classification.region_kind == EMU68_REGION_UNMAPPED);

    memset(&access, 0, sizeof(access));
    access.address = 0x00dff020u;
    access.kind = EMU68_ACCESS_READ;
    access.space = EMU68_SPACE_DATA;
    access.function_code = 5u;
    access.width = 2u;
    CHECK(emu68_machine_dispatch_access(&access) == EMU68_BUS_COMPLETE);
    CHECK(callback_calls == 1u);
    CHECK(access.sequence == 1u);
    CHECK(access.value_lo == 0xfeedu);
    {
        emu68_machine_bridge_result_t bridge = emu68_machine_bridge_dispatch(
            0x00dff020u, 0u, 2u, 0u, NULL);
        CHECK(bridge.outcome == EMU68_BRIDGE_COMPLETE);
        CHECK(bridge.value == 0xfeedu);
        CHECK(callback_calls == 2u);
    }

    CHECK(emu68_machine_unmap(cpu, direct.guest_base, direct.size) == EMU68_OK);
    CHECK(unmap_calls == 1u);
    CHECK(emu68_machine_unmap(cpu, direct.guest_base, direct.size) ==
          EMU68_ERR_NOT_FOUND);
    emu68_machine_destroy(cpu);
}

static void test_cooperative_access(void)
{
    emu68_cpu_t *cpu = create_cpu(EMU68_EXEC_COOPERATIVE);
    uint8_t native_frame[800];
    emu68_external_region_t external = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(external),
        .guest_base = 0x00bfd000u,
        .size = 0x1000u,
        .region_id = 9u,
        .flags = EMU68_REGION_READ | EMU68_REGION_WRITE,
    };
    emu68_bus_access_t access;
    emu68_bus_access_t pending;
    emu68_reset_state_t reset = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(reset),
        .initial_ssp = 0x1000u,
        .initial_pc = 0x2000u,
    };

    CHECK(emu68_machine_map_external(cpu, &external) == EMU68_OK);
    CHECK(emu68_machine_bridge_dispatch(
              0x00bfd100u, 0x55u,
              EMU68_BRIDGE_META_WRITE | 1u, 0u, NULL).outcome ==
          EMU68_BRIDGE_BUS_ERROR);
    CHECK(emu68_machine_get_pending_access(cpu, &pending) ==
          EMU68_ERR_NOT_FOUND);

    memset(native_frame, 0xa5, sizeof(native_frame));
    CHECK(emu68_machine_bridge_dispatch(
              0x00bfd100u, 0x55u,
              EMU68_BRIDGE_META_WRITE | 1u, 0x12345678u,
              native_frame).outcome == EMU68_BRIDGE_PENDING);
    CHECK(emu68_machine_native_access_pending());
    CHECK(!emu68_machine_prepare_native_resume());
    CHECK(emu68_machine_get_pending_access(cpu, &pending) == EMU68_OK);
    CHECK(pending.sequence == 1u);
    CHECK(pending.value_lo == 0x55u);
    pending.result = EMU68_BUS_COMPLETE;
    CHECK(emu68_machine_complete_access(cpu, &pending) == EMU68_OK);
    CHECK(emu68_machine_prepare_native_resume());
    CHECK(emu68_machine_resume_address == (uintptr_t)0x12345678u);
    CHECK(emu68_machine_resume_outcome == EMU68_BRIDGE_COMPLETE);
    CHECK(emu68_machine_resume_frame[799] == 0xa5u);
    CHECK(!emu68_machine_native_access_pending());

    memset(&access, 0, sizeof(access));
    access.address = 0x00bfd100u;
    access.kind = EMU68_ACCESS_WRITE;
    access.space = EMU68_SPACE_DATA;
    access.function_code = 5u;
    access.width = 1u;
    access.value_lo = 0x55u;
    CHECK(emu68_machine_dispatch_access(&access) == EMU68_BUS_COMPLETE);
    CHECK(emu68_machine_get_pending_access(cpu, &pending) == EMU68_OK);
    CHECK(pending.sequence == 2u);
    CHECK(pending.value_lo == 0x55u);

    pending.sequence++;
    pending.result = EMU68_BUS_COMPLETE;
    CHECK(emu68_machine_complete_access(cpu, &pending) == EMU68_ERR_ACCESS);
    CHECK(emu68_machine_get_pending_access(cpu, &pending) == EMU68_OK);
    pending.result = EMU68_BUS_COMPLETE;
    CHECK(emu68_machine_complete_access(cpu, &pending) == EMU68_OK);

    CHECK(emu68_machine_set_ipl(cpu, 7u) == EMU68_OK);
    CHECK(ipl_value == 7u);
    CHECK(emu68_machine_set_ipl(cpu, 8u) == EMU68_ERR_INVALID_ARGUMENT);
    emu68_machine_request_stop(cpu);
    CHECK(wake_calls != 0u);

    CHECK(emu68_machine_reset(cpu, &reset) == EMU68_OK);
    CHECK(reset_calls == 1u);
    CHECK(emu68_machine_get_pending_access(cpu, &pending) ==
          EMU68_ERR_NOT_FOUND);
    emu68_machine_destroy(cpu);
}

int main(void)
{
    test_abi_and_singleton();
    test_regions_and_sync_access();
    test_cooperative_access();
    puts("emu68 machine API unit tests: PASS");
    return 0;
}
