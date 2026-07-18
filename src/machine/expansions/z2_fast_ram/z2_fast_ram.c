#include "machine/expansions/z2_fast_ram/z2_fast_ram.h"

#include "cpu/cpu_backend.h"
#include "cpu/direct_region.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/board_registry.h"
#include "machine/machine.h"
#include "support.h"

#include <string.h>

/*
 * Zorro II Fast RAM board, expressed in the Emu68 ExpansionBoard model: a
 * self-registering descriptor whose map() installs a DIRECT region. The host
 * backing and the CPU backend are taken from globals at map() time, mirroring
 * how Emu68's z2ram.c uses the global MMU.
 */

static uint8_t s_config[AUTOCONFIG_DATA_SIZE];

static uint8_t bytes_to_ac_size(uint32_t size)
{
    if (size >= 8u * 1024u * 1024u) return AC_SIZE_8MB;
    if (size >= 4u * 1024u * 1024u) return AC_SIZE_4MB;
    if (size >= 2u * 1024u * 1024u) return AC_SIZE_2MB;
    if (size >= 1u * 1024u * 1024u) return AC_SIZE_1MB;
    if (size >= 512u * 1024u)       return AC_SIZE_512KB;
    if (size >= 256u * 1024u)       return AC_SIZE_256KB;
    if (size >= 128u * 1024u)       return AC_SIZE_128KB;
    return AC_SIZE_64KB;
}

static uint32_t ac_size_to_bytes(uint8_t code)
{
    switch (code) {
    case AC_SIZE_8MB:   return 8u * 1024u * 1024u;
    case AC_SIZE_4MB:   return 4u * 1024u * 1024u;
    case AC_SIZE_2MB:   return 2u * 1024u * 1024u;
    case AC_SIZE_1MB:   return 1u * 1024u * 1024u;
    case AC_SIZE_512KB: return 512u * 1024u;
    case AC_SIZE_256KB: return 256u * 1024u;
    case AC_SIZE_128KB: return 128u * 1024u;
    default:            return 64u * 1024u;
    }
}

static void fast_ram_map(struct ExpansionBoard *board)
{
    BellatrixMemory *memory = bellatrix_machine_memory();
    CpuBackend *backend = cpu_backend_selected();
    BellatrixDirectRegion region;

    if (!memory || !backend || !memory->fast_ram ||
        memory->fast_ram_size < board->rom_size)
        return;

    region.guest_base = board->map_base;
    region.size = board->rom_size;
    region.host_base = memory->fast_ram;
    region.flags = BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_WRITE |
                   BELLATRIX_DIRECT_EXECUTE | BELLATRIX_DIRECT_CACHEABLE;
    if (cpu_backend_map_direct(backend, &region) != 0)
        return;

    memory->fast_ram_base = board->map_base;
    memory->fast_ram_configured = 1u;
    kprintf("[Z2-RAM] mapped backing=%p guest=%08x-%08x\n",
            (void *)memory->fast_ram, (unsigned)board->map_base,
            (unsigned)(board->map_base + board->rom_size - 1u));
}

static struct ExpansionBoard s_fast_ram_board = {
    .rom_file = s_config,
    .rom_size = 0u,
    .map_base = 0u,
    .is_z3 = 0u,
    .enabled = 0u,
    .map = fast_ram_map,
};
BELLATRIX_REGISTER_BOARD_Z2(s_fast_ram_board);

void bellatrix_z2_fast_ram_configure(uint32_t size_bytes)
{
    uint8_t raw[AUTOCONFIG_ROM_BYTES];
    uint8_t size_code;

    if (size_bytes == 0u) {
        bellatrix_z2_fast_ram_disable();
        return;
    }

    size_code = bytes_to_ac_size(size_bytes);
    memset(raw, 0, sizeof(raw));
    raw[0] = (uint8_t)(AC_TYPE_Z2 | AC_TYPE_MEMLIST | size_code);
    raw[1] = 0x01u;
    raw[4] = 0x07u;
    raw[5] = 0xdbu;
    raw[9] = 0x01u;
    autoconfig_build(s_config, raw);

    s_fast_ram_board.rom_size = ac_size_to_bytes(size_code);
    s_fast_ram_board.map_base = 0u;
    s_fast_ram_board.enabled = 1u;
}

void bellatrix_z2_fast_ram_disable(void)
{
    s_fast_ram_board.enabled = 0u;
    s_fast_ram_board.map_base = 0u;
}
