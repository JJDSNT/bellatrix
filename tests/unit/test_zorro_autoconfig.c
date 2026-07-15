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
    uint32_t map_count;
    uint32_t unmap_count;
    uint32_t base;
    uint32_t size;
    int fail_next;
} MapProbe;

static int probe_map(void *userdata, uint32_t base, uint32_t size)
{
    MapProbe *probe = (MapProbe *)userdata;
    probe->map_count++;
    probe->base = base;
    probe->size = size;
    if (probe->fail_next) {
        probe->fail_next = 0;
        return -1;
    }
    return 0;
}

static void probe_unmap(void *userdata, uint32_t base, uint32_t size)
{
    MapProbe *probe = (MapProbe *)userdata;
    probe->unmap_count++;
    probe->base = base;
    probe->size = size;
}

int main(void)
{
    static uint8_t z2_config[AUTOCONFIG_DATA_SIZE];
    static uint8_t z3_config[AUTOCONFIG_DATA_SIZE];
    BellatrixZorro2BoardDesc z2;
    BellatrixZorro3BoardDesc z3;
    BellatrixZorro3BoardOps z3_ops;
    MapProbe probe;

    memset(z2_config, 0, sizeof(z2_config));
    memset(z3_config, 0, sizeof(z3_config));
    z2_config[0] = 0xc0u;
    z3_config[0] = 0x80u;

    memset(&z2, 0, sizeof(z2));
    z2.id = "test.z2";
    z2.config_data = z2_config;
    z2.config_size = sizeof(z2_config);
    z2.window_size = 0x00010000u;

    memset(&z3, 0, sizeof(z3));
    memset(&z3_ops, 0, sizeof(z3_ops));
    memset(&probe, 0, sizeof(probe));
    z3_ops.map = probe_map;
    z3_ops.unmap = probe_unmap;
    z3.id = "test.z3";
    z3.config_data = z3_config;
    z3.config_size = sizeof(z3_config);
    z3.window_size = 0x00100000u;
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

    check_eq("z3 follows z2", 0x80u,
             bellatrix_zorro_autoconfig_read8(0x00e80000u));
    probe.fail_next = 1;
    bellatrix_zorro_autoconfig_write16(0x00e80044u, 0x4567u);
    check_eq("failed map rolls back configured", 0u,
             (uint32_t)bellatrix_zorro3_board_configured("test.z3"));
    check_eq("failed map rolls back base", 0u,
             bellatrix_zorro3_board_base("test.z3"));
    check_eq("failed map keeps board pending", 0x80u,
             bellatrix_zorro_autoconfig_read8(0x00e80000u));

    bellatrix_zorro_autoconfig_write16(0x00e80044u, 0x4567u);
    check_eq("z3 configured", 1u,
             (uint32_t)bellatrix_zorro3_board_configured("test.z3"));
    check_eq("z3 guest-assigned base", 0x45670000u,
             bellatrix_zorro3_board_base("test.z3"));
    check_eq("z3 map called", 2u, probe.map_count);
    check_eq("z3 map base", 0x45670000u, probe.base);
    check_eq("z3 map size", 0x00100000u, probe.size);

    check_eq("empty chain open bus", 0xffffffffu,
             bellatrix_zorro_autoconfig_read32(0x00e80000u));
    bellatrix_zorro3_reset();
    check_eq("z3 reset unmaps", 1u, probe.unmap_count);
    puts("zorro autoconfig tests passed");
    return 0;
}
