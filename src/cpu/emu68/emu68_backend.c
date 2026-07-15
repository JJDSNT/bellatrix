#include "cpu/emu68/bellatrix.h"
#include "cpu/emu68/emu68_backend.h"
#include "cpu/emu68/emu68_machine.h"
#include "cpu/cpu_backend.h"
#include "machine/memory/memory.h"
#include "support.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if BELLATRIX_ENABLE_EMU68_BOARDS
#include <boards.h>
#endif

#define EMU68_CHIP_HOST_BASE ((void *)(uintptr_t)UINT64_C(0xffffff9000000000))
#define EMU68_FAST_HOST_BASE ((void *)(uintptr_t)UINT64_C(0x0000000000200000))
#define EMU68_EXT_ROM_HOST_BASE ((void *)(uintptr_t)UINT64_C(0xffffff9000e00000))
#define EMU68_ROM_HOST_BASE ((void *)(uintptr_t)UINT64_C(0xffffff9000f80000))

#define EMU68_REGION_ID_BELLATRIX_BUS 1u

extern void M68K_StartEmu(void *addr, void *fdt);

typedef struct BellatrixEmu68Backend {
    emu68_cpu_t *cpu;
    uint32_t pc;
    int ready;
    int topology_ready;
    int overlay;
    int overlay_pending;
#if BELLATRIX_ENABLE_EMU68_BOARDS
    int board_index;
    struct ExpansionBoard **boards;
#endif
} BellatrixEmu68Backend;

static BellatrixEmu68Backend s_backend;

static emu68_status_t map_direct(uint32_t guest_base, uint64_t size,
                                 void *host_base, uint32_t flags)
{
    emu68_direct_region_t region = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(region),
        .guest_base = guest_base,
        .size = size,
        .host_base = host_base,
        .flags = flags,
    };

    return emu68_machine_map_direct(s_backend.cpu, &region);
}

static emu68_status_t map_external(uint32_t guest_base, uint64_t size)
{
    emu68_external_region_t region = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(region),
        .guest_base = guest_base,
        .size = size,
        .region_id = EMU68_REGION_ID_BELLATRIX_BUS,
        .flags = EMU68_REGION_READ | EMU68_REGION_WRITE,
    };

    return emu68_machine_map_external(s_backend.cpu, &region);
}

static emu68_status_t map_low_overlay(int enabled)
{
    void *host = enabled ? EMU68_EXT_ROM_HOST_BASE : EMU68_CHIP_HOST_BASE;
    uint32_t flags = EMU68_REGION_READ | EMU68_REGION_EXECUTE |
                     EMU68_REGION_CACHEABLE;

    if (!enabled)
        flags |= EMU68_REGION_WRITE;
    return map_direct(0x00000000u, BELLATRIX_ROM_SIZE, host, flags);
}

static int configure_topology(void)
{
    const uint32_t ram_flags = EMU68_REGION_READ | EMU68_REGION_WRITE |
                               EMU68_REGION_EXECUTE | EMU68_REGION_CACHEABLE;
    const uint32_t rom_flags = EMU68_REGION_READ | EMU68_REGION_EXECUTE |
                               EMU68_REGION_CACHEABLE;

    if (map_low_overlay(s_backend.overlay) != EMU68_OK ||
        map_direct(0x00080000u, 0x00080000u,
                   (uint8_t *)EMU68_CHIP_HOST_BASE + 0x00080000u,
                   ram_flags) != EMU68_OK ||
        map_direct(0x00100000u, 0x00100000u, EMU68_CHIP_HOST_BASE,
                   ram_flags) != EMU68_OK ||
        map_direct(0x00200000u, 0x00800000u, EMU68_FAST_HOST_BASE,
                   ram_flags) != EMU68_OK ||
        map_external(0x00a00000u, 0x00400000u) != EMU68_OK ||
        map_direct(0x00e00000u, 0x00080000u, EMU68_EXT_ROM_HOST_BASE,
                   rom_flags) != EMU68_OK ||
        map_external(0x00e80000u, 0x00100000u) != EMU68_OK ||
        map_direct(0x00f80000u, 0x00080000u, EMU68_ROM_HOST_BASE,
                   rom_flags) != EMU68_OK ||
        map_external(0x01000000u, UINT64_C(0xff000000)) != EMU68_OK) {
        kprintf("[EMU68-MACHINE] failed to install Bellatrix topology\n");
        return 0;
    }

    s_backend.topology_ready = 1;
    return 1;
}

#if BELLATRIX_ENABLE_EMU68_BOARDS
extern struct ExpansionBoard *__boards_start;
extern int zorro_disable;

static struct ExpansionBoard *current_board(void)
{
    if (!s_backend.boards)
        s_backend.boards = &__boards_start;
    while (s_backend.boards[s_backend.board_index] &&
           !s_backend.boards[s_backend.board_index]->enabled)
        ++s_backend.board_index;
    return s_backend.boards[s_backend.board_index];
}

static int board_access(emu68_bus_access_t *access)
{
    struct ExpansionBoard *board;

    if (zorro_disable || access->address < 0x00e80000u ||
        access->address > 0x00e8ffffu)
        return 0;
    board = current_board();
    if (!board)
        return 0;

    if (access->kind == EMU68_ACCESS_READ) {
        if (access->width != 1u)
            return 0;
        access->value_lo =
            ((const uint8_t *)board->rom_file)[access->address - 0x00e80000u];
        return 1;
    }

    if (board->is_z3 && access->address == 0x00e80044u) {
        board->map_base = ((uint32_t)access->value_lo & 0xffffu) << 16;
        board->map(board);
        ++s_backend.board_index;
        return 1;
    }
    if (!board->is_z3 && access->address == 0x00e80048u) {
        board->map_base = ((uint32_t)access->value_lo & 0xffu) << 16;
        board->map(board);
        ++s_backend.board_index;
        return 1;
    }
    if (access->address == 0x00e8004cu ||
        access->address == 0x00e8004eu) {
        ++s_backend.board_index;
        return 1;
    }
    return 0;
}
#else
static int board_access(emu68_bus_access_t *access)
{
    (void)access;
    return 0;
}
#endif

static uint32_t bus_read(uint32_t address, uint8_t width)
{
    return bellatrix_bus_access(address, 0u, width, BUS_READ);
}

static void bus_write(uint32_t address, uint32_t value, uint8_t width)
{
    bellatrix_bus_access(address, value, width, BUS_WRITE);
}

static emu68_bus_result_t machine_bus_access(void *opaque,
                                             emu68_bus_access_t *access)
{
    uint8_t offset;
    (void)opaque;

    if (!access || access->region_id != EMU68_REGION_ID_BELLATRIX_BUS)
        return EMU68_BUS_ERROR;
    if (board_access(access))
        return EMU68_BUS_COMPLETE;

    if (access->kind == EMU68_ACCESS_READ) {
        access->value_lo = 0u;
        access->value_hi = 0u;
        for (offset = 0u; offset < access->width; offset += 4u) {
            uint8_t beat = (uint8_t)(access->width - offset);
            uint32_t value;
            if (beat > 4u)
                beat = 4u;
            value = bus_read(access->address + offset, beat);
            if (access->width <= 8u)
                access->value_lo |= (uint64_t)value <<
                    (unsigned)(access->width - offset - beat) * 8u;
            else if (offset < 8u)
                access->value_hi |= (uint64_t)value <<
                    (unsigned)(8u - offset - beat) * 8u;
            else
                access->value_lo |= (uint64_t)value <<
                    (unsigned)(16u - offset - beat) * 8u;
        }
    } else {
        for (offset = 0u; offset < access->width; offset += 4u) {
            uint8_t beat = (uint8_t)(access->width - offset);
            uint64_t source;
            unsigned shift;
            if (beat > 4u)
                beat = 4u;
            if (access->width <= 8u) {
                source = access->value_lo;
                shift = (unsigned)(access->width - offset - beat) * 8u;
            } else if (offset < 8u) {
                source = access->value_hi;
                shift = (unsigned)(8u - offset - beat) * 8u;
            } else {
                source = access->value_lo;
                shift = (unsigned)(16u - offset - beat) * 8u;
            }
            bus_write(access->address + offset,
                      (uint32_t)(source >> shift), beat);
        }
    }
    return EMU68_BUS_COMPLETE;
}

static void machine_progress(void *opaque, uint64_t cycle_delta,
                             uint64_t instruction_delta, uint32_t pc)
{
    (void)opaque;
    bellatrix_emu68_publish_cpu_progress(cycle_delta, instruction_delta, pc);
}

static uint32_t backend_get_pc(void *ctx)
{
    (void)ctx;
    return s_backend.pc;
}

static void backend_set_ipl(void *ctx, int level)
{
    (void)ctx;
    if (s_backend.ready)
        (void)emu68_machine_set_ipl(s_backend.cpu, (unsigned)level);
}

static void backend_reset(void *ctx)
{
    emu68_reset_state_t state = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(state),
    };
    (void)ctx;

    M68K_StartEmu(0, NULL);
    if (!s_backend.ready)
        return;
    if (!s_backend.topology_ready && !configure_topology()) {
        s_backend.ready = 0;
        return;
    }
#if BELLATRIX_ENABLE_EMU68_BOARDS
    s_backend.boards = &__boards_start;
    s_backend.board_index = 0;
#endif
    state.initial_ssp = bellatrix_reset_isp;
    state.initial_pc = bellatrix_reset_pc;
    if (emu68_machine_reset(s_backend.cpu, &state) != EMU68_OK) {
        kprintf("[EMU68-MACHINE] reset failed\n");
        s_backend.ready = 0;
        return;
    }
    s_backend.pc = state.initial_pc;
}

static int apply_pending_overlay(void)
{
    if (!s_backend.overlay_pending)
        return 1;
    if (emu68_machine_unmap(s_backend.cpu, 0u, BELLATRIX_ROM_SIZE) != EMU68_OK ||
        map_low_overlay(s_backend.overlay) != EMU68_OK) {
        kprintf("[EMU68-MACHINE] overlay remap failed\n");
        s_backend.ready = 0;
        return 0;
    }
    s_backend.overlay_pending = 0;
    return 1;
}

static int backend_run(void *ctx, uint32_t cycles)
{
    emu68_run_result_t result = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(result),
    };
    (void)ctx;

    if (!s_backend.ready || cycles == 0u)
        return 0;
    if (emu68_machine_run(s_backend.cpu, cycles, &result) != EMU68_OK) {
        kprintf("[EMU68-MACHINE] run failed\n");
        s_backend.ready = 0;
        return 0;
    }
    s_backend.pc = result.pc;
    if (result.reason == EMU68_STOP_REQUESTED && !apply_pending_overlay())
        return 0;
    if (result.reason == EMU68_STOP_STOPPED) {
        bellatrix_emu68_publish_idle_cycles(cycles);
        return (int)cycles;
    }
    if (result.reason == EMU68_STOP_BUS_ERROR ||
        result.reason == EMU68_STOP_FATAL) {
        kprintf("[EMU68-MACHINE] stopped reason=%u pc=%08x detail=%08x\n",
                (unsigned)result.reason, (unsigned)result.pc,
                (unsigned)result.detail);
        return 0;
    }
    if (result.cycles_executed > (uint64_t)INT_MAX)
        return INT_MAX;
    return (int)result.cycles_executed;
}

static CpuBackend s_cpu_backend = {
    .ctx = NULL,
    .get_pc = backend_get_pc,
    .set_ipl = backend_set_ipl,
    .reset = backend_reset,
    .run = backend_run,
    .progress_in_run = 1,
};

CpuBackend *bellatrix_emu68_backend_get(void)
{
    return &s_cpu_backend;
}

void bellatrix_emu68_backend_init(void)
{
    static const emu68_machine_ops_t ops = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(ops),
        .bus_access = machine_bus_access,
        .progress = machine_progress,
    };
    emu68_machine_config_t config = {
        .abi_version = EMU68_MACHINE_ABI_VERSION,
        .struct_size = sizeof(config),
        .execution_mode = EMU68_EXEC_SYNCHRONOUS,
        .ops = &ops,
        .opaque = NULL,
    };

    memset(&s_backend, 0, sizeof(s_backend));
    s_backend.overlay = 1;
    if (emu68_machine_create(&config, &s_backend.cpu) != EMU68_OK) {
        kprintf("[EMU68-MACHINE] create failed; no legacy fallback is permitted\n");
        return;
    }
    s_backend.ready = 1;
#if defined(BELLATRIX_EMU68_FAULT_DRIVEN) && BELLATRIX_EMU68_FAULT_DRIVEN
    kprintf("[EMU68-MACHINE] native fault-driven execution selected\n");
#else
    kprintf("[EMU68-MACHINE] public machine API active\n");
#endif
}

int bellatrix_emu68_backend_prepare_topology(void)
{
    /* Direct mappings update the host MMU and must be installed by the boot
     * core after Bellatrix has finished using the low bootstrap pages, but
     * before the CPU and chipset cores are launched. */
    if (!s_backend.ready)
        return 0;
#if defined(BELLATRIX_EMU68_FAULT_DRIVEN) && BELLATRIX_EMU68_FAULT_DRIVEN
    kprintf("[EMU68-MACHINE] diagnostic fault-driven access mode active\n");
    return 1;
#endif
    if (s_backend.topology_ready)
        return 1;
    if (!configure_topology()) {
        kprintf("[EMU68-MACHINE] topology setup failed; no legacy fallback is permitted\n");
        s_backend.ready = 0;
        return 0;
    }
    kprintf("[EMU68-MACHINE] topology installed on boot core\n");
    return 1;
}

int bellatrix_emu68_backend_set_overlay(int enabled)
{
    enabled = enabled != 0;
    s_backend.overlay = enabled;
#if defined(BELLATRIX_EMU68_FAULT_DRIVEN) && BELLATRIX_EMU68_FAULT_DRIVEN
    /* Let bellatrix.c install the legacy low-memory MMU mapping. */
    return 0;
#endif
    if (!s_backend.ready || !s_backend.topology_ready)
        return 0;
    s_backend.overlay_pending = 1;
    emu68_machine_request_stop(s_backend.cpu);
    return 1;
}
