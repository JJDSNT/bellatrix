#include "memory/memory.h"
#include "memory/memory_map.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t bellatrix_debug_cpu_pc(void)
{
    return 0u;
}

static uint8_t s_chip_ram[BELLATRIX_CHIP_RAM_SIZE];

static void failf(const char *expr, const char *file, int line,
                  uint32_t expected, uint32_t actual)
{
    fprintf(stderr,
            "FAIL %s:%d %s expected=0x%08x actual=0x%08x\n",
            file, line, expr, expected, actual);
    exit(1);
}

#define CHECK_EQ(expr, expected, actual)                                      \
    do                                                                        \
    {                                                                         \
        uint32_t expected__ = (uint32_t)(expected);                           \
        uint32_t actual__ = (uint32_t)(actual);                               \
        if (expected__ != actual__)                                           \
            failf((expr), __FILE__, __LINE__, expected__, actual__);          \
    } while (0)

static void test_decode_regions(void)
{
    CHECK_EQ("chip decode", MEM_REGION_CHIP_RAM, memory_map_decode(0x000100u));
    CHECK_EQ("fast decode lo", MEM_REGION_FAST, memory_map_decode(0x00200000u));
    CHECK_EQ("fast decode hi", MEM_REGION_FAST, memory_map_decode(0x009fffffu));
    CHECK_EQ("z2 decode", MEM_REGION_Z2, memory_map_decode(0x00e80000u));
    CHECK_EQ("exp rom check decode", MEM_REGION_EXP_ROM_CHECK, memory_map_decode(0x00f00000u));
    CHECK_EQ("rom decode", MEM_REGION_ROM, memory_map_decode(0x00f80000u));
}

static void test_memory_init(BellatrixMemory *mem)
{
    memset(s_chip_ram, 0, sizeof(s_chip_ram));
    bellatrix_memory_init(mem);
    mem->chip_ram = s_chip_ram;
    mem->chip_ram_size = sizeof(s_chip_ram);
    mem->chip_ram_mask = sizeof(s_chip_ram) - 1u;
}

static void test_overlay_reads_and_chip_writes(void)
{
    BellatrixMemory mem;
    uint8_t rom[8];

    test_memory_init(&mem);

    rom[0] = 0x11u;
    rom[1] = 0x22u;
    rom[2] = 0x33u;
    rom[3] = 0x44u;
    rom[4] = 0xaau;
    rom[5] = 0xbbu;
    rom[6] = 0xccu;
    rom[7] = 0xddu;

    bellatrix_memory_attach_rom(&mem, rom, sizeof(rom));

    bellatrix_mem_write32(&mem, 0x000000u, 0xdeadbeefu);

    CHECK_EQ("overlay low read32", 0x11223344u, bellatrix_mem_read32(&mem, 0x000000u));
    CHECK_EQ("chip backing kept writes", 0xdeadbeefu, bellatrix_chip_read32(&mem, 0x000000u));

    bellatrix_memory_set_overlay(&mem, 0);

    CHECK_EQ("overlay off low read32", 0xdeadbeefu, bellatrix_mem_read32(&mem, 0x000000u));
    CHECK_EQ("rom window read32", 0x11223344u, bellatrix_mem_read32(&mem, 0x00f80000u));
    CHECK_EQ("rom window read16", 0xaabbu, bellatrix_mem_read16(&mem, 0x00f80004u));
}

static void test_fast_ram_big_endian(void)
{
    BellatrixMemory mem;

    test_memory_init(&mem);

    bellatrix_mem_write32(&mem, 0x00200000u, 0x12345678u);

    CHECK_EQ("fast read32", 0x12345678u, bellatrix_mem_read32(&mem, 0x00200000u));
    CHECK_EQ("fast read16 hi", 0x1234u, bellatrix_mem_read16(&mem, 0x00200000u));
    CHECK_EQ("fast read16 lo", 0x5678u, bellatrix_mem_read16(&mem, 0x00200002u));
    CHECK_EQ("fast read8 b0", 0x12u, bellatrix_mem_read8(&mem, 0x00200000u));
    CHECK_EQ("fast read8 b3", 0x78u, bellatrix_mem_read8(&mem, 0x00200003u));
}

static void test_autoconfig_window_is_empty(void)
{
    BellatrixMemory mem;

    test_memory_init(&mem);

    CHECK_EQ("z2 read8 empty", 0xffu, bellatrix_mem_read8(&mem, 0x00e80000u));
    CHECK_EQ("z2 read16 empty", 0xffffu, bellatrix_mem_read16(&mem, 0x00e80000u));
    CHECK_EQ("z2 read32 empty", 0xffffffffu, bellatrix_mem_read32(&mem, 0x00e80000u));

    bellatrix_mem_write16(&mem, 0x00e80048u, 0x1234u);
    CHECK_EQ("z2 stays empty after write", 0xffffffffu, bellatrix_mem_read32(&mem, 0x00e80000u));
}

static void test_expansion_rom_probe_window_is_neutral(void)
{
    BellatrixMemory mem;

    test_memory_init(&mem);

    CHECK_EQ("exp probe read8", 0x00u, bellatrix_mem_read8(&mem, 0x00f00000u));
    CHECK_EQ("exp probe read16", 0x0000u, bellatrix_mem_read16(&mem, 0x00f00000u));
    CHECK_EQ("exp probe read32", 0x00000000u, bellatrix_mem_read32(&mem, 0x00f00000u));
}

static void test_chip_ram_bank_independence(void)
{
    BellatrixMemory mem;

    test_memory_init(&mem);

    bellatrix_chip_write32(&mem, 0x000000u, 0x11223344u);
    bellatrix_chip_write32(&mem, 0x080000u, 0x55667788u);
    bellatrix_chip_write32(&mem, 0x100000u, 0x99aabbccu);
    bellatrix_chip_write32(&mem, 0x180000u, 0xddeeff00u);

    CHECK_EQ("chip bank 0 keeps own pattern",
             0x11223344u,
             bellatrix_chip_read32(&mem, 0x000000u));
    CHECK_EQ("chip bank 1 keeps own pattern",
             0x55667788u,
             bellatrix_chip_read32(&mem, 0x080000u));
    CHECK_EQ("chip bank 2 keeps own pattern",
             0x99aabbccu,
             bellatrix_chip_read32(&mem, 0x100000u));
    CHECK_EQ("chip bank 3 keeps own pattern",
             0xddeeff00u,
             bellatrix_chip_read32(&mem, 0x180000u));
}

static void test_chip_ram_top_boundary_wrap(void)
{
    BellatrixMemory mem;

    test_memory_init(&mem);

    bellatrix_chip_write8(&mem, 0x000000u, 0x00u);
    bellatrix_chip_write8(&mem, 0x000001u, 0x00u);
    bellatrix_chip_write32(&mem, 0x001ffffeu, 0xa1b2c3d4u);

    CHECK_EQ("chip top read32 wraps into low bytes",
             0xa1b2c3d4u,
             bellatrix_chip_read32(&mem, 0x001ffffeu));
    CHECK_EQ("chip top byte 0 written at last even address",
             0xa1u,
             bellatrix_chip_read8(&mem, 0x001ffffeu));
    CHECK_EQ("chip top byte 1 written at last odd address",
             0xb2u,
             bellatrix_chip_read8(&mem, 0x001fffffu));
    CHECK_EQ("chip wrap updates low byte 0",
             0xc3u,
             bellatrix_chip_read8(&mem, 0x000000u));
    CHECK_EQ("chip wrap updates low byte 1",
             0xd4u,
             bellatrix_chip_read8(&mem, 0x000001u));
}

static void test_chip_ram_wrap_helper_and_overlay_probe_visibility(void)
{
    BellatrixMemory mem;
    uint8_t rom[4];

    test_memory_init(&mem);

    CHECK_EQ("chip wrap helper wraps exact size",
             0x000000u,
             bellatrix_chip_wrap_addr(&mem, 0x00200000u));
    CHECK_EQ("chip wrap helper wraps arbitrary offset",
             0x000123u,
             bellatrix_chip_wrap_addr(&mem, 0x00200123u));

    rom[0] = 0xdeu;
    rom[1] = 0xadu;
    rom[2] = 0xbeu;
    rom[3] = 0xefu;
    bellatrix_memory_attach_rom(&mem, rom, sizeof(rom));

    bellatrix_mem_write32(&mem, 0x000000u, 0x13579bdfu);
    CHECK_EQ("overlay hides chip probe writes while enabled",
             0xdeadbeefu,
             bellatrix_mem_read32(&mem, 0x000000u));

    bellatrix_memory_set_overlay(&mem, 0);
    CHECK_EQ("chip probe writes become visible once overlay drops",
             0x13579bdfu,
             bellatrix_mem_read32(&mem, 0x000000u));
}

int main(void)
{
    test_decode_regions();
    test_overlay_reads_and_chip_writes();
    test_fast_ram_big_endian();
    test_autoconfig_window_is_empty();
    test_expansion_rom_probe_window_is_neutral();
    test_chip_ram_bank_independence();
    test_chip_ram_top_boundary_wrap();
    test_chip_ram_wrap_helper_and_overlay_probe_visibility();

    puts("bellatrix_unit_memory: ok");
    return 0;
}
