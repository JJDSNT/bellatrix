#include "machine/expansions/z2_fast_ram/z2_fast_ram.h"

#include "cpu/direct_region.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "support.h"

#include <string.h>

typedef struct BellatrixZ2FastRam {
    CpuBackend *cpu_backend;
    BellatrixMemory *memory;
    uint32_t mapped_base;
    uint32_t mapped_size;
} BellatrixZ2FastRam;

static BellatrixZ2FastRam s_fast_ram;
static uint8_t s_config[AUTOCONFIG_DATA_SIZE];
static BellatrixZorro2BoardDesc s_desc;

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

static int fast_ram_map(void *userdata, uint32_t base, uint32_t size)
{
    BellatrixZ2FastRam *board = (BellatrixZ2FastRam *)userdata;
    BellatrixDirectRegion region;
    int rc;

    if (!board || !board->cpu_backend || !board->memory ||
        !board->memory->fast_ram || board->memory->fast_ram_size < size)
        return -1;
    region.guest_base = base;
    region.size = size;
    region.host_base = board->memory->fast_ram;
    region.flags = BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_WRITE |
                   BELLATRIX_DIRECT_EXECUTE | BELLATRIX_DIRECT_CACHEABLE;
    rc = cpu_backend_map_direct(board->cpu_backend, &region);
    if (rc != 0)
        return rc;
    board->mapped_base = base;
    board->mapped_size = size;
    board->memory->fast_ram_base = base;
    board->memory->fast_ram_configured = 1u;
    kprintf("[Z2-RAM] mapped backing=%p guest=%08x-%08x\n",
            (void *)board->memory->fast_ram, (unsigned)base,
            (unsigned)(base + size - 1u));
    return 0;
}

static void fast_ram_unmap(void *userdata, uint32_t base, uint32_t size)
{
    BellatrixZ2FastRam *board = (BellatrixZ2FastRam *)userdata;
    int rc;

    if (!board || !board->cpu_backend || board->mapped_base != base ||
        board->mapped_size != size)
        return;
    rc = cpu_backend_unmap_direct(board->cpu_backend, base, size);
    if (rc != 0)
        kprintf("[Z2-RAM] unmap failed rc=%d base=%08x size=%08x\n",
                rc, (unsigned)base, (unsigned)size);
    board->mapped_base = 0u;
    board->mapped_size = 0u;
    if (board->memory) {
        board->memory->fast_ram_base = 0u;
        board->memory->fast_ram_configured = 0u;
    }
}

static uint8_t fast_ram_read8(void *userdata, uint32_t offset)
{
    BellatrixZ2FastRam *board = (BellatrixZ2FastRam *)userdata;
    if (!board || !board->memory || !board->memory->fast_ram ||
        offset >= board->mapped_size)
        return 0xffu;
    return board->memory->fast_ram[offset];
}

static void fast_ram_write8(void *userdata, uint32_t offset, uint8_t value)
{
    BellatrixZ2FastRam *board = (BellatrixZ2FastRam *)userdata;
    if (!board || !board->memory || !board->memory->fast_ram ||
        offset >= board->mapped_size)
        return;
    board->memory->fast_ram[offset] = value;
}

int bellatrix_z2_fast_ram_register(CpuBackend *cpu_backend,
                                   BellatrixMemory *memory,
                                   uint32_t size_bytes)
{
    static const BellatrixZorro2BoardOps ops = {
        .map = fast_ram_map,
        .unmap = fast_ram_unmap,
        .read8 = fast_ram_read8,
        .write8 = fast_ram_write8,
    };
    uint8_t raw[AUTOCONFIG_ROM_BYTES];
    uint8_t size_code = bytes_to_ac_size(size_bytes);
    int rc;

    if (!cpu_backend || !memory)
        return -1;
    if (bellatrix_zorro2_fast_ram_registered()) {
        (void)bellatrix_zorro2_unregister_board("bellatrix.fastram");
        bellatrix_zorro2_publish_fast_ram(0u, 0);
    }
    memset(raw, 0, sizeof(raw));
    raw[0] = (uint8_t)(AC_TYPE_Z2 | AC_TYPE_MEMLIST | size_code);
    raw[1] = 0x01u;
    raw[4] = 0x07u;
    raw[5] = 0xdbu;
    raw[9] = 0x01u;
    autoconfig_build(s_config, raw);

    memset(&s_fast_ram, 0, sizeof(s_fast_ram));
    s_fast_ram.cpu_backend = cpu_backend;
    s_fast_ram.memory = memory;
    memset(&s_desc, 0, sizeof(s_desc));
    s_desc.id = "bellatrix.fastram";
    s_desc.config_data = s_config;
    s_desc.config_size = sizeof(s_config);
    s_desc.window_size = ac_size_to_bytes(size_code);
    s_desc.userdata = &s_fast_ram;
    s_desc.ops = &ops;
    rc = bellatrix_zorro2_register_board(&s_desc);
    bellatrix_zorro2_publish_fast_ram(s_desc.window_size, rc == 0);
    return rc;
}
