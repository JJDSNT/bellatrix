#include "cpu/cpu_backend.h"
#include "cpu/direct_region.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/expansions/z2_fast_ram/z2_fast_ram.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t s_backing[0x00100000u] __attribute__((aligned(0x1000)));
static BellatrixDirectRegionMap s_regions;

int kprintf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

int cpu_backend_map_direct(CpuBackend *backend,
                           const BellatrixDirectRegion *region)
{
    return backend && backend->map_direct
        ? backend->map_direct(backend->ctx, region) : -1;
}

int cpu_backend_unmap_direct(CpuBackend *backend, uint32_t base, uint32_t size)
{
    return backend && backend->unmap_direct
        ? backend->unmap_direct(backend->ctx, base, size) : -1;
}

static void check(const char *name, int condition)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(1);
    }
}

static int host_map(void *opaque, const BellatrixDirectRegion *region)
{
    (void)opaque;
    (void)region;
    return 0;
}

static int host_unmap(void *opaque, const BellatrixDirectRegion *region)
{
    (void)opaque;
    (void)region;
    return 0;
}

static int backend_map(void *ctx, const BellatrixDirectRegion *region)
{
    return bellatrix_direct_region_install(
        (BellatrixDirectRegionMap *)ctx, region);
}

static int backend_unmap(void *ctx, uint32_t base, uint32_t size)
{
    return bellatrix_direct_region_remove(
        (BellatrixDirectRegionMap *)ctx, base, size);
}

int main(void)
{
    static const BellatrixDirectRegionBackendOps host_ops = {
        .map = host_map,
        .unmap = host_unmap,
    };
    BellatrixMemory memory;
    CpuBackend backend;
    uint32_t value = 0u;
    const uint32_t assigned_base = 0x00400000u;

    memset(&memory, 0, sizeof(memory));
    memset(&backend, 0, sizeof(backend));
    memset(s_backing, 0, sizeof(s_backing));
    memory.fast_ram = s_backing;
    memory.fast_ram_size = sizeof(s_backing);
    memory.fast_ram_mask = sizeof(s_backing) - 1u;
    bellatrix_direct_region_map_init(&s_regions, &host_ops, NULL);
    backend.ctx = &s_regions;
    backend.map_direct = backend_map;
    backend.unmap_direct = backend_unmap;

    check("register", bellatrix_z2_fast_ram_register(
              &backend, &memory, sizeof(s_backing)) == 0);
    bellatrix_zorro2_init();
    check("not configured before assignment",
          !bellatrix_zorro2_fast_ram_configured());
    check("no direct region before assignment",
          bellatrix_direct_region_find(&s_regions, assigned_base, 1u) == NULL);

    bellatrix_zorro2_config_write8(0x00e80048u,
                                   (uint8_t)(assigned_base >> 16));
    check("configured after assignment",
          bellatrix_zorro2_fast_ram_configured());
    check("memory records guest base",
          memory.fast_ram_configured && memory.fast_ram_base == assigned_base);
    check("direct region uses assigned base",
          bellatrix_direct_region_find(&s_regions, assigned_base, 4u) != NULL);
    check("typical first base remains absent",
          bellatrix_direct_region_find(&s_regions, 0x00200000u, 1u) == NULL);
    check("direct write", bellatrix_direct_region_write(
              &s_regions, assigned_base, 4u, 0x12345678u));
    check("direct read", bellatrix_direct_region_read(
              &s_regions, assigned_base, 4u, &value) &&
          value == 0x12345678u);

    bellatrix_zorro2_reset();
    check("reset clears memory window", !memory.fast_ram_configured &&
          memory.fast_ram_base == 0u);
    check("reset removes direct region",
          bellatrix_direct_region_find(&s_regions, assigned_base, 1u) == NULL);
    puts("z2 fast RAM lifecycle tests passed");
    return 0;
}
