#include "cpu/direct_region.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct BackendProbe {
    uint32_t map_count;
    uint32_t unmap_count;
    int fail_map;
    int fail_unmap;
} BackendProbe;

static void check(const char *name, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(1);
    }
}

static int probe_map(void *opaque, const BellatrixDirectRegion *region)
{
    BackendProbe *probe = (BackendProbe *)opaque;
    (void)region;
    probe->map_count++;
    return probe->fail_map ? -1 : 0;
}

static int probe_unmap(void *opaque, const BellatrixDirectRegion *region)
{
    BackendProbe *probe = (BackendProbe *)opaque;
    (void)region;
    probe->unmap_count++;
    return probe->fail_unmap ? -1 : 0;
}

int main(void)
{
    static uint8_t rom[0x10000] __attribute__((aligned(0x1000)));
    static const BellatrixDirectRegionBackendOps ops = {
        .map = probe_map,
        .unmap = probe_unmap,
    };
    BellatrixDirectRegionMap map;
    BellatrixDirectRegion region = {
        .guest_base = 0x45670000u,
        .size = sizeof(rom),
        .host_base = rom,
        .flags = BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_EXECUTE |
                 BELLATRIX_DIRECT_CACHEABLE,
    };
    BellatrixDirectRegion overlap = region;
    BellatrixDirectRegion unaligned = region;
    BackendProbe probe;
    uint32_t value;

    memset(&probe, 0, sizeof(probe));
    bellatrix_direct_region_map_init(&map, &ops, &probe);

    unaligned.guest_base++;
    check("reject unaligned region",
          bellatrix_direct_region_install(&map, &unaligned) == -1);
    check("invalid region never reaches backend", probe.map_count == 0u);

    check("install readonly ROM",
          bellatrix_direct_region_install(&map, &region) == 0);
    check("backend map called", probe.map_count == 1u);
    check("find complete access",
          bellatrix_direct_region_find(&map, 0x45670004u, 4u) != NULL);
    check("reject crossing access",
          bellatrix_direct_region_find(&map, 0x4567ffffu, 2u) == NULL);

    rom[4] = 0x12u;
    rom[5] = 0x34u;
    rom[6] = 0x56u;
    rom[7] = 0x78u;
    check("read direct ROM long",
          bellatrix_direct_region_read(&map, 0x45670004u, 4u, &value));
    check("direct ROM is big endian", value == 0x12345678u);
    check("readonly write is handled",
          bellatrix_direct_region_write(&map, 0x45670004u, 4u,
                                        0xaabbccddu));
    check("readonly write is ignored", rom[4] == 0x12u && rom[7] == 0x78u);

    overlap.guest_base += 0x8000u;
    check("reject overlap",
          bellatrix_direct_region_install(&map, &overlap) == -2);
    check("overlap never reaches backend", probe.map_count == 1u);

    probe.fail_unmap = 1;
    check("failed unmap rolls back",
          bellatrix_direct_region_remove(&map, region.guest_base,
                                         region.size) == -2);
    check("failed unmap remains visible",
          bellatrix_direct_region_find(&map, region.guest_base, 1u) != NULL);
    probe.fail_unmap = 0;
    check("unmap succeeds",
          bellatrix_direct_region_remove(&map, region.guest_base,
                                         region.size) == 0);
    check("removed region is invisible",
          bellatrix_direct_region_find(&map, region.guest_base, 1u) == NULL);

    probe.fail_map = 1;
    check("failed map is reported",
          bellatrix_direct_region_install(&map, &region) == -4);
    check("failed map is not published",
          bellatrix_direct_region_find(&map, region.guest_base, 1u) == NULL);

    puts("direct region tests passed");
    return 0;
}
