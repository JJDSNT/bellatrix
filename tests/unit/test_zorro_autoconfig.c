#include "cpu/direct_region.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/zorro_autoconfig.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/bus/zorro3/zorro3.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kprintf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

static void check_eq(const char *name, uint32_t expected, uint32_t actual)
{
    if (expected != actual) {
        fprintf(stderr, "FAIL %s expected=%08x actual=%08x\n",
                name, (unsigned)expected, (unsigned)actual);
        exit(1);
    }
}

typedef struct MapProbe {
    BellatrixDirectRegionMap direct_map;
    uint32_t map_count;
    uint32_t unmap_count;
    uint32_t base;
    uint32_t size;
    uint32_t fail_at_map_count;
} MapProbe;

static uint8_t s_z3_rom[0x1000] __attribute__((aligned(0x1000)));

static int backend_map(void *userdata, const BellatrixDirectRegion *region)
{
    MapProbe *probe = (MapProbe *)userdata;
    probe->map_count++;
    probe->base = region->guest_base;
    probe->size = region->size;
    if (probe->fail_at_map_count == probe->map_count) {
        probe->fail_at_map_count = 0u;
        return -1;
    }
    return 0;
}

static int backend_unmap(void *userdata, const BellatrixDirectRegion *region)
{
    MapProbe *probe = (MapProbe *)userdata;
    probe->unmap_count++;
    probe->base = region->guest_base;
    probe->size = region->size;
    return 0;
}

static int board_map(void *userdata, uint32_t base, uint32_t size)
{
    MapProbe *probe = (MapProbe *)userdata;
    BellatrixDirectRegion rom = {
        .guest_base = base,
        .size = sizeof(s_z3_rom),
        .host_base = s_z3_rom,
        .flags = BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_EXECUTE |
                 BELLATRIX_DIRECT_CACHEABLE,
    };
    if (size < sizeof(s_z3_rom))
        return -1;
    return bellatrix_direct_region_install(&probe->direct_map, &rom);
}

static void board_unmap(void *userdata, uint32_t base, uint32_t size)
{
    MapProbe *probe = (MapProbe *)userdata;
    (void)size;
    (void)bellatrix_direct_region_remove(&probe->direct_map, base,
                                         sizeof(s_z3_rom));
}

int main(void)
{
    static uint8_t z2_config[AUTOCONFIG_DATA_SIZE];
    static uint8_t z3_config[AUTOCONFIG_DATA_SIZE];
    BellatrixZorro2BoardDesc z2;
    BellatrixZorro3BoardDesc z3;
    BellatrixZorro3BoardOps z3_ops;
    static const BellatrixDirectRegionBackendOps direct_ops = {
        .map = backend_map,
        .unmap = backend_unmap,
    };
    MapProbe probe;

    memset(z2_config, 0, sizeof(z2_config));
    memset(z3_config, 0, sizeof(z3_config));
    z2_config[0] = 0xc0u;
    /* First encoded byte of Emu68's 68040 support board: logical er_Type 0x91
     * (Z3 + diagnostic ROM + 64 KiB window). */
    z3_config[0] = 0x90u;
    z3_config[2] = 0x10u;

    memset(&z2, 0, sizeof(z2));
    z2.id = "test.z2";
    z2.config_data = z2_config;
    z2.config_size = sizeof(z2_config);
    z2.window_size = 0x00010000u;

    memset(&z3, 0, sizeof(z3));
    memset(&z3_ops, 0, sizeof(z3_ops));
    memset(&probe, 0, sizeof(probe));
    bellatrix_direct_region_map_init(&probe.direct_map, &direct_ops, &probe);
    z3_ops.map = board_map;
    z3_ops.unmap = board_unmap;
    z3.id = "test.z3";
    z3.config_data = z3_config;
    z3.config_size = sizeof(z3_config);
    z3.window_size = 0x00010000u;
    z3.userdata = &probe;
    z3.ops = &z3_ops;

    check_eq("register z2", 0u, (uint32_t)bellatrix_zorro2_register_board(&z2));
    check_eq("register z3", 0u, (uint32_t)bellatrix_zorro3_register_board(&z3));
    bellatrix_zorro2_init();
    bellatrix_zorro3_init();

    check_eq("z2 is first", 0xc0u,
             bellatrix_zorro_autoconfig_read8(0x00e80000u));
    bellatrix_zorro_autoconfig_write8(0x00e80048u, 0x20u);
    check_eq("z2 configured", 1u,
             (uint32_t)bellatrix_zorro2_board_configured("test.z2"));
    check_eq("z2 base", 0x00200000u,
             bellatrix_zorro2_board_base("test.z2"));

    check_eq("z3 follows z2", 0x90u,
             bellatrix_zorro_autoconfig_read8(0x00e80000u));
    check_eq("z3 ROM invisible before autoconfig", 0u,
             bellatrix_direct_region_find(&probe.direct_map, 0x45670000u,
                                          1u) != NULL);
    probe.fail_at_map_count = 1u;
    bellatrix_zorro_autoconfig_write16(0x00e80044u, 0x4567u);
    check_eq("failed map rolls back configured", 0u,
             (uint32_t)bellatrix_zorro3_board_configured("test.z3"));
    check_eq("failed map rolls back base", 0u,
             bellatrix_zorro3_board_base("test.z3"));
    check_eq("failed map keeps board pending", 0x90u,
             bellatrix_zorro_autoconfig_read8(0x00e80000u));
    check_eq("failed ROM map is not published", 0u,
             bellatrix_direct_region_find(&probe.direct_map, 0x45670000u,
                                          1u) != NULL);

    bellatrix_zorro_autoconfig_write16(0x00e80044u, 0x4567u);
    check_eq("z3 configured", 1u,
             (uint32_t)bellatrix_zorro3_board_configured("test.z3"));
    check_eq("z3 guest-assigned base", 0x45670000u,
             bellatrix_zorro3_board_base("test.z3"));
    check_eq("z3 backend maps attempted", 2u, probe.map_count);
    check_eq("z3 ROM direct after autoconfig", 1u,
             bellatrix_direct_region_find(&probe.direct_map, 0x45670000u,
                                          1u) != NULL);
    check_eq("z3 ROM remains read-only", 0u,
             bellatrix_direct_region_find(&probe.direct_map, 0x45670000u,
                                          1u)->flags & BELLATRIX_DIRECT_WRITE);

    check_eq("empty chain open bus", 0xffffffffu,
             bellatrix_zorro_autoconfig_read32(0x00e80000u));
    bellatrix_zorro3_reset();
    check_eq("z3 reset unmaps ROM", 1u, probe.unmap_count);
    check_eq("z3 ROM invisible after reset", 0u,
             bellatrix_direct_region_find(&probe.direct_map, 0x45670000u,
                                          1u) != NULL);
    puts("zorro autoconfig tests passed");
    return 0;
}
