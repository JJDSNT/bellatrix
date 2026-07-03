// tools/harness/musashi_backend.c
//
// Musashi CpuBackend — implements the two machine callbacks using Musashi,
// and provides the memory read/write callbacks that Musashi requires.
//
// Address map served by this dispatcher:
//   0x000000–BELLATRIX_CHIP_RAM_END  chip RAM (or ROM overlay at boot)
//   0xC00000–0xD7FFFF  slow RAM via shared Bellatrix memory map
//   0xE00000–0xEFFFFF  extended ROM (1 MB ROMs only — first 512 KB)
//   0xF80000–0xFFFFFF  standard ROM (Kickstart / AROS second 512 KB)
//   everything else    delegated to bellatrix_machine_read/write (chipset/CIA/RTC)
//
// ROM size → layout:
//   256 KB → single window at 0xFC0000
//   512 KB → single window at 0xF80000
//   1 MB   → first 512 KB at 0xE00000, second 512 KB at 0xF80000

#include "musashi_backend.h"
#include "cpu/cpu_bridge.h"
#include "machine/machine.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/memory/memory.h"
#include "rigel/rigel_cia.h"

#include "m68k.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * ROM storage — up to 1 MB
 * ------------------------------------------------------------------------- */

#define HARNESS_ROM_MAX   (1024u * 1024u)

static uint8_t  s_rom[HARNESS_ROM_MAX];
static uint32_t s_rom_size  = 0;
static unsigned int s_cpu_type = M68K_CPU_TYPE_68000;
static uint32_t s_boot_trace_until_pc = 0;
static uint32_t s_boot_trace_ack_count = 0;
static uint32_t s_boot_display_a5 = 0;
static uint32_t s_boot_display_ctx = 0;
static uint32_t s_boot_display_aux = 0;
static uint32_t s_boot_display_buf0 = 0;
static uint32_t s_boot_display_buf1 = 0;
static int s_low_mem_trace = -1;
static int s_watch_ranges_init = 0;
static uint32_t s_watch_range_lo[2] = {0, 0};
static uint32_t s_watch_range_hi[2] = {0, 0};
static int s_watch_range_enabled[2] = {0, 0};
static int s_rom_watch_ranges_init = 0;
static uint32_t s_rom_watch_range_lo[2] = {0, 0};
static uint32_t s_rom_watch_range_hi[2] = {0, 0};
static int s_rom_watch_range_enabled[2] = {0, 0};
static int s_trace_pc_ranges_init = 0;
static uint32_t s_trace_pc_lo[2] = {0, 0};
static uint32_t s_trace_pc_hi[2] = {0, 0};
static int s_trace_pc_enabled[2] = {0, 0};
static int s_run_sync_active = 0;
static uint32_t s_run_sync_published = 0;

#define BOOT_DISPLAY_BUF_SIZE 0x00001F40u

static uint32_t harness_chip_read(uint32_t addr, int size);
static uint32_t harness_cpu_ram_read(uint32_t addr, int size);

static void harness_init_watch_ranges(void);
static int harness_watch_custom_range_addr(uint32_t addr);
static int harness_watch_rom_range_addr(uint32_t addr);
static void harness_trace_pc_range(uint32_t pc);
static void harness_trace_exec_call(uint32_t pc);
static void harness_trace_library_call(uint32_t pc);

static int harness_boot_trace_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("HARNESS_BOOT_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    return enabled;
}

/* General-purpose diagnostic for AI_context/issue_paula_audio_cpu_chipset_sync.md:
 * the chipset (Rigel/Paula/CIA/...) only advances when this function runs,
 * which only happens on a CPU bus touch (see call sites in harness_read/
 * harness_write below). A long stretch of bus-touch-free M68K execution
 * (tight ALU loop, register-only work) means the chipset silently stalls
 * for that whole stretch, no matter how many CCK "should" have elapsed.
 * Not audio-specific — any subsystem that depends on regular chipset
 * stepping (disk, serial, CIA timers, ...) can stall the same way.
 * HARNESS_CCK_GAP_TRACE=1 to enable; off by default (one int check). */
static int harness_cck_gap_trace_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("HARNESS_CCK_GAP_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    return enabled;
}

/* Bucket edges in M68K cycles since the previous sync. Bucket i covers
 * (edges[i-1], edges[i]]; the last bucket is "bigger than all edges". */
static const uint32_t HARNESS_CCK_GAP_EDGES[] = {
    100u, 500u, 2000u, 10000u, 100000u
};
#define HARNESS_CCK_GAP_BUCKETS \
    (sizeof(HARNESS_CCK_GAP_EDGES) / sizeof(HARNESS_CCK_GAP_EDGES[0]) + 1u)

static void harness_cck_gap_trace(uint32_t delta)
{
    static uint64_t s_calls = 0;
    static uint64_t s_buckets[HARNESS_CCK_GAP_BUCKETS] = {0};
    static uint32_t s_max_delta = 0;
    unsigned i;

    s_calls++;

    for (i = 0; i < HARNESS_CCK_GAP_BUCKETS - 1u; i++) {
        if (delta <= HARNESS_CCK_GAP_EDGES[i]) {
            s_buckets[i]++;
            break;
        }
    }
    if (i == HARNESS_CCK_GAP_BUCKETS - 1u)
        s_buckets[i]++;

    /* Flag every new record stall immediately, with the PC that was
     * executing when the gap finally closed — directly points at the code
     * region responsible for a long chipset-stall stretch. */
    if (delta > s_max_delta && delta > HARNESS_CCK_GAP_EDGES[1]) {
        s_max_delta = delta;
        printf("[CCK-GAP-MAX] delta_m68k=%u (%u CCK) pc=%08x\n",
               (unsigned)delta, (unsigned)(delta / 2u),
               (unsigned)m68k_get_reg(NULL, M68K_REG_PC));
    }

    if ((s_calls % 200000u) == 0u) {
        printf("[CCK-GAP] calls=%llu max=%u (%u CCK)"
               " buckets(<=%u,<=%u,<=%u,<=%u,<=%u,>%u)="
               "%llu,%llu,%llu,%llu,%llu,%llu\n",
               (unsigned long long)s_calls,
               (unsigned)s_max_delta, (unsigned)(s_max_delta / 2u),
               (unsigned)HARNESS_CCK_GAP_EDGES[0],
               (unsigned)HARNESS_CCK_GAP_EDGES[1],
               (unsigned)HARNESS_CCK_GAP_EDGES[2],
               (unsigned)HARNESS_CCK_GAP_EDGES[3],
               (unsigned)HARNESS_CCK_GAP_EDGES[4],
               (unsigned)HARNESS_CCK_GAP_EDGES[4],
               (unsigned long long)s_buckets[0], (unsigned long long)s_buckets[1],
               (unsigned long long)s_buckets[2], (unsigned long long)s_buckets[3],
               (unsigned long long)s_buckets[4], (unsigned long long)s_buckets[5]);
    }
}

static void harness_sync_cpu_progress(void)
{
    int ran;
    uint32_t delta;

    if (!s_run_sync_active)
        return;

    ran = m68k_cycles_run();
    if (ran <= 0 || (uint32_t)ran <= s_run_sync_published)
        return;

    delta = (uint32_t)ran - s_run_sync_published;
    s_run_sync_published = (uint32_t)ran;

    if (harness_cck_gap_trace_enabled())
        harness_cck_gap_trace(delta);

    bellatrix_bridge_cpu_progress(delta);
}

static void harness_init_trace_pc_ranges(void)
{
    static const char *const env_names[2] = {
        "HARNESS_TRACE_PC_RANGE1",
        "HARNESS_TRACE_PC_RANGE2"
    };
    int i;

    if (s_trace_pc_ranges_init)
        return;
    s_trace_pc_ranges_init = 1;

    for (i = 0; i < 2; ++i)
    {
        const char *spec = getenv(env_names[i]);
        char *endptr = NULL;
        unsigned long lo;
        unsigned long hi;

        if (!spec || !*spec)
            continue;

        lo = strtoul(spec, &endptr, 0);
        if (!endptr || *endptr != ':')
            continue;
        hi = strtoul(endptr + 1, &endptr, 0);
        if (!endptr || *endptr != '\0')
            continue;
        if (hi < lo)
            continue;

        s_trace_pc_lo[i] = (uint32_t)lo & 0x00FFFFFFu;
        s_trace_pc_hi[i] = (uint32_t)hi & 0x00FFFFFFu;
        s_trace_pc_enabled[i] = 1;

        printf("[HARNESS-TRACE-PC-RANGE] slot=%d lo=%06x hi=%06x\n",
               i + 1,
               (unsigned)s_trace_pc_lo[i],
               (unsigned)s_trace_pc_hi[i]);
    }
}

static int harness_trace_pc_range_enabled(uint32_t pc)
{
    int i;

    harness_init_trace_pc_ranges();
    pc &= 0x00FFFFFFu;

    for (i = 0; i < 2; ++i)
    {
        if (!s_trace_pc_enabled[i])
            continue;
        if (pc >= s_trace_pc_lo[i] && pc <= s_trace_pc_hi[i])
            return 1;
    }

    return 0;
}

/* Standard ROM window (reset vectors, Kickstart entry) */
static uint32_t s_rom_std_base  = 0xF80000u;  /* standard: 0xF80000 or 0xFC0000 */
static uint32_t s_rom_std_off   = 0;           /* byte offset into s_rom */
static uint32_t s_rom_std_size  = 0;

/* Extended ROM window (1 MB ROMs: first half at 0xE00000) */
static uint32_t s_rom_ext_base  = 0;
static uint32_t s_rom_ext_off   = 0;
static uint32_t s_rom_ext_size  = 0;

void musashi_backend_load_rom(const uint8_t *data, uint32_t size, uint32_t base)
{
    if (size > HARNESS_ROM_MAX) size = HARNESS_ROM_MAX;
    memcpy(s_rom, data, size);
    s_rom_size = size;

    s_rom_ext_base = s_rom_ext_size = 0;

    if (size > 512u * 1024u) {
        /* 1 MB ROM: split extended (0xE00000) + standard (0xF80000) */
        uint32_t half = size / 2u;
        s_rom_ext_base = 0xE00000u;
        s_rom_ext_off  = 0;
        s_rom_ext_size = half;
        s_rom_std_base = 0xF80000u;
        s_rom_std_off  = half;
        s_rom_std_size = half;
    } else {
        /* Single window: use caller-supplied base (or default) */
        s_rom_std_base = base;
        s_rom_std_off  = 0;
        s_rom_std_size = size;
    }
}

/* ---------------------------------------------------------------------------
 * Overlay helper — CIA-A PRA bit 0 controls ROM/RAM at address 0
 * ------------------------------------------------------------------------- */

static int harness_overlay(void)
{
    struct RigelContext *ctx = bellatrix_machine_rigel_ctx();
    if (!ctx) return 1;
    if (!(rigel_cia_read(ctx, 0u, 0x2u) & 0x01u)) return 1; /* ddra=input → overlay on */
    return (rigel_cia_read(ctx, 0u, 0x0u) & 0x01u) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Unified address read (big-endian byte lanes)
 * ------------------------------------------------------------------------- */

static uint32_t rom_read_at(uint32_t byte_off, int size)
{
    if (size == 1) return s_rom[byte_off];
    if (size == 2) return ((uint32_t)s_rom[byte_off] << 8) | s_rom[byte_off + 1];
    if (size == 4) return ((uint32_t)s_rom[byte_off    ] << 24) |
                          ((uint32_t)s_rom[byte_off + 1] << 16) |
                          ((uint32_t)s_rom[byte_off + 2] <<  8) |
                           (uint32_t)s_rom[byte_off + 3];
    return 0;
}

static void harness_init_watch_ranges(void)
{
    static const char *const env_names[2] = {
        "HARNESS_WATCH_RANGE1",
        "HARNESS_WATCH_RANGE2"
    };
    int i;

    if (s_watch_ranges_init)
        return;
    s_watch_ranges_init = 1;

    for (i = 0; i < 2; ++i)
    {
        const char *spec = getenv(env_names[i]);
        char *endptr = NULL;
        unsigned long lo;
        unsigned long hi;

        if (!spec || !*spec)
            continue;

        lo = strtoul(spec, &endptr, 0);
        if (!endptr || *endptr != ':')
            continue;
        hi = strtoul(endptr + 1, &endptr, 0);
        if (!endptr || *endptr != '\0')
            continue;
        if (hi < lo)
            continue;

        s_watch_range_lo[i] = (uint32_t)lo & 0x00FFFFFFu;
        s_watch_range_hi[i] = (uint32_t)hi & 0x00FFFFFFu;
        s_watch_range_enabled[i] = 1;

        printf("[HARNESS-WATCH-RANGE] slot=%d lo=%06x hi=%06x\n",
               i + 1,
               (unsigned)s_watch_range_lo[i],
               (unsigned)s_watch_range_hi[i]);
    }
}

static int harness_watch_custom_range_addr(uint32_t addr)
{
    int i;

    harness_init_watch_ranges();
    addr &= 0x00FFFFFFu;

    for (i = 0; i < 2; ++i)
    {
        if (!s_watch_range_enabled[i])
            continue;
        if (addr >= s_watch_range_lo[i] && addr <= s_watch_range_hi[i])
            return 1;
    }

    return 0;
}

static void harness_init_rom_watch_ranges(void)
{
    static const char *const env_names[2] = {
        "HARNESS_ROM_WATCH_RANGE1",
        "HARNESS_ROM_WATCH_RANGE2"
    };
    int i;

    if (s_rom_watch_ranges_init)
        return;
    s_rom_watch_ranges_init = 1;

    for (i = 0; i < 2; ++i)
    {
        const char *spec = getenv(env_names[i]);
        char *endptr = NULL;
        unsigned long lo;
        unsigned long hi;

        if (!spec || !*spec)
            continue;

        lo = strtoul(spec, &endptr, 0);
        if (!endptr || *endptr != ':')
            continue;
        hi = strtoul(endptr + 1, &endptr, 0);
        if (!endptr || *endptr != '\0')
            continue;
        if (hi < lo)
            continue;

        s_rom_watch_range_lo[i] = (uint32_t)lo & 0x00FFFFFFu;
        s_rom_watch_range_hi[i] = (uint32_t)hi & 0x00FFFFFFu;
        s_rom_watch_range_enabled[i] = 1;

        printf("[HARNESS-ROM-WATCH-RANGE] slot=%d lo=%06x hi=%06x\n",
               i + 1,
               (unsigned)s_rom_watch_range_lo[i],
               (unsigned)s_rom_watch_range_hi[i]);
    }
}

static int harness_watch_rom_range_addr(uint32_t addr)
{
    int i;

    harness_init_rom_watch_ranges();
    addr &= 0x00FFFFFFu;

    for (i = 0; i < 2; ++i)
    {
        if (!s_rom_watch_range_enabled[i])
            continue;
        if (addr >= s_rom_watch_range_lo[i] && addr <= s_rom_watch_range_hi[i])
            return 1;
    }

    return 0;
}

static void harness_watch_rom_read(uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    if (!harness_watch_rom_range_addr(addr))
        return;

    printf("[WATCH-ROM-R] pc=%08x addr=%06x size=%d val=%08x "
           "D0=%08x D1=%08x D2=%08x D3=%08x "
           "A0=%06x A1=%06x A2=%06x A3=%06x A4=%06x A5=%06x A6=%06x A7=%06x\n",
           (unsigned)pc,
           (unsigned)(addr & 0x00FFFFFFu),
           size,
           (unsigned)value,
           (unsigned)m68k_get_reg(NULL, M68K_REG_D0),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D1),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D2),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D3),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A3) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A7) & 0x00FFFFFFu));
}

/* AROS-LOOP: count SERDATR reads near the TBE poll loop at 0xFE85FA */
static void aros_loop_check(uint32_t addr, uint32_t ret)
{
    if (addr != 0x00DFF018u) return;
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    if (pc < 0x00FE85E0u || pc > 0x00FE8610u) return;
    static int count = 0;
    count++;
    if (count <= 20 || count % 100000 == 0)
        printf("[AROS-LOOP] count=%-8d pc=%08x SERDATR=%04x TBE=%d\n",
               count, pc, (unsigned)ret, (ret >> 13) & 1);
}

static void aros_wait2_check(uint32_t addr, uint32_t ret)
{
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    if (pc < 0x00FE9800u || pc > 0x00FE9820u) return;
    static int count = 0;
    count++;
    if (count <= 40 || count % 100000 == 0)
        printf("[AROS-WAIT2] count=%-8d pc=%08x addr=%06x ret=%08x\n",
               count, pc, addr & 0x00FFFFFFu, ret);
}

static void aros_dump_copper_list_once(const char *name, uint32_t base)
{
    static int enabled = -1;
    static uint32_t dumped[8];
    static unsigned dumped_count;

    if (enabled < 0) {
        const char *env = getenv("AROS_COPLIST_DUMP");
        enabled = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    if (!enabled || base == 0)
        return;

    base &= 0x00FFFFFFu;
    for (unsigned i = 0; i < dumped_count; i++) {
        if (dumped[i] == base)
            return;
    }
    if (dumped_count < (sizeof(dumped) / sizeof(dumped[0])))
        dumped[dumped_count++] = base;

    printf("[AROS-COPLIST] %s base=%06x\n", name, (unsigned)base);
    for (unsigned off = 0; off < 0x100u; off += 4u) {
        uint16_t op = (uint16_t)harness_cpu_ram_read(base + off, 2);
        uint16_t val = (uint16_t)harness_cpu_ram_read(base + off + 2u, 2);

        if (op == 0xFFFFu && val == 0xFFFEu) {
            printf("[AROS-COPLIST]   +%03x END %04x %04x\n",
                   off, (unsigned)op, (unsigned)val);
            break;
        }
        if ((op & 1u) == 0u) {
            printf("[AROS-COPLIST]   +%03x MOVE reg=%03x val=%04x\n",
                   off, (unsigned)(op & 0x01FEu), (unsigned)val);
        } else {
            printf("[AROS-COPLIST]   +%03x WAIT/SKIP op=%04x mask=%04x\n",
                   off, (unsigned)op, (unsigned)val);
        }
    }
}

/* AROS-GFXBASE: capture GfxBase from A2 at the gfx_vblank LOFlist/SHFlist
 * comparison site (move.l ($32,A2),D0 / move.l ($36,A2),D1 at pc=0xfc95fc/0xfc9600,
 * AmigaVideo's gfx_vblank() in amigavideo_chipset.c), then watch every write to
 * GfxBase->LOFlist (+0x32) / GfxBase->SHFlist (+0x36) to find what (if anything)
 * ever populates them with a non-zero Copper-list address. */
static void aros_gfxbase_lof_check(uint32_t pc, uint32_t addr, uint32_t value, int is_write)
{
    static int enabled = -1;
    static uint32_t gfxbase = 0;

    if (enabled < 0) {
        const char *env = getenv("AROS_GFXBASE_TRACE");
        enabled = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    if (!enabled) return;

    if (!is_write && (pc == 0x00FC95FCu || pc == 0x00FC9600u)) {
        uint32_t a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
        uint32_t lof = (a2 != 0) ? harness_cpu_ram_read(a2 + 0x32u, 4) : 0;
        uint32_t shf = (a2 != 0) ? harness_cpu_ram_read(a2 + 0x36u, 4) : 0;
        static uint32_t last_pc = 0;
        static uint32_t last_a2 = 0;
        static uint32_t last_lof = 0;
        static uint32_t last_shf = 0;
        if (a2 != 0 && a2 != gfxbase) {
            gfxbase = a2;
            printf("[AROS-GFXBASE] GfxBase=%06x captured at pc=%06x  LOFlist@%06x SHFlist@%06x\n",
                   (unsigned)gfxbase, (unsigned)pc,
                   (unsigned)(gfxbase + 0x32u), (unsigned)(gfxbase + 0x36u));
        }
        if (a2 != 0 &&
            (pc != last_pc || a2 != last_a2 || lof != last_lof || shf != last_shf)) {
            printf("[AROS-GFXBASE-R] pc=%06x GfxBase=%06x addr=%06x value=%08x LOF=%08x SHF=%08x\n",
                   (unsigned)pc,
                   (unsigned)a2,
                   (unsigned)addr,
                   (unsigned)value,
                   (unsigned)lof,
                   (unsigned)shf);
            last_pc = pc;
            last_a2 = a2;
            last_lof = lof;
            last_shf = shf;
            aros_dump_copper_list_once("LOF", lof);
            aros_dump_copper_list_once("SHF", shf);
        }
    }

    if (!is_write || gfxbase == 0) return;

    {
        uint32_t lof_addr = gfxbase + 0x32u;
        uint32_t shf_addr = gfxbase + 0x36u;
        if (addr == lof_addr || addr == shf_addr) {
            printf("[AROS-GFXBASE-W] %s addr=%06x val=%08x pc=%06x\n",
                   (addr == lof_addr) ? "LOFlist" : "SHFlist",
                   (unsigned)addr, (unsigned)value, (unsigned)pc);
        }
    }
}

/* AROS-BPL-DUMP: one-shot hexdump of the bitplane buffers referenced by the
 * active screen Copper list (BPL1PT=0x000C2B38, BPL2PT=0x000C7B38, found by
 * decoding the list at COP2LC=0x000D9EFC). Fires N gfx_vblank invocations
 * after a screen first attaches (GfxBase captured), to check whether anything
 * was ever actually rendered into the bitmap (vs. an all-zero buffer). */
static void aros_bpl_dump_check(uint32_t pc)
{
    static int enabled = -1;
    static int armed = 0;
    static int fired = 0;
    static int vbl_count = 0;
    static int trigger_after = 60;

    if (enabled < 0) {
        const char *env = getenv("AROS_BPL_DUMP");
        enabled = (env != NULL && env[0] != '\0' && env[0] != '0') ? 1 : 0;
        if (enabled) {
            const char *after = getenv("AROS_BPL_DUMP_AFTER");
            if (after != NULL && after[0] != '\0')
                trigger_after = atoi(after);
        }
    }
    if (!enabled || fired) return;
    if (pc != 0x00FC95FCu && pc != 0x00FC9600u) return;

    if (!armed) {
        uint32_t a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
        if (a2 != 0) {
            uint32_t lof = harness_cpu_ram_read(a2 + 0x32u, 4);
            if (lof != 0) armed = 1;
        }
        return;
    }

    vbl_count++;
    if (vbl_count < trigger_after) return;

    fired = 1;
    {
        uint32_t addrs[4];
        static const char *names[4] = { "BPL1", "BPL2", "BPL3", "BPL4" };
        int p;
        addrs[0] = ((bellatrix_machine_read(0x00DFF0E0u, 2) & 0x001Fu) << 16) |
                   (bellatrix_machine_read(0x00DFF0E2u, 2) & 0xFFFEu);
        addrs[1] = ((bellatrix_machine_read(0x00DFF0E4u, 2) & 0x001Fu) << 16) |
                   (bellatrix_machine_read(0x00DFF0E6u, 2) & 0xFFFEu);
        addrs[2] = ((bellatrix_machine_read(0x00DFF0E8u, 2) & 0x001Fu) << 16) |
                   (bellatrix_machine_read(0x00DFF0EAu, 2) & 0xFFFEu);
        addrs[3] = ((bellatrix_machine_read(0x00DFF0ECu, 2) & 0x001Fu) << 16) |
                   (bellatrix_machine_read(0x00DFF0EEu, 2) & 0xFFFEu);

        for (p = 0; p < 4; p++) {
            uint32_t base = addrs[p];
            uint32_t off;
            uint32_t nonzero = 0;
            if (base == 0)
                continue;
            printf("[AROS-BPL-DUMP] %s buffer @ %06x (after %d gfx_vblank fires):\n",
                   names[p], (unsigned)base, vbl_count);
            for (off = 0; off < 256u; off += 16u) {
                uint8_t bytes[16];
                uint32_t k;
                for (k = 0; k < 16u; k++)
                    bytes[k] = (uint8_t)harness_chip_read(base + off + k, 1);
                printf("[AROS-BPL-DUMP]   +%04x:", (unsigned)off);
                for (k = 0; k < 16u; k++) {
                    printf(" %02x", bytes[k]);
                    if (bytes[k] != 0) nonzero++;
                }
                printf("\n");
            }
            /* scan a wider window (one full hires scanline = 80 bytes * 256 lines) for any non-zero byte */
            {
                uint32_t scan, total_nonzero = 0;
                uint32_t window = 80u * 256u;
                for (scan = 0; scan < window; scan++) {
                    if (harness_chip_read(base + scan, 1) != 0) total_nonzero++;
                }
                printf("[AROS-BPL-DUMP] %s non-zero bytes in first 256 bytes=%u, in full %u-byte window=%u\n",
                       names[p], (unsigned)nonzero, (unsigned)window, (unsigned)total_nonzero);
            }
        }
    }
}

static int harness_watch_gfx_pc(uint32_t pc)
{
    if (pc >= 0x00FC17C0u && pc <= 0x00FC18F0u)
        return 1;
    if (pc >= 0x00FC9C00u && pc <= 0x00FC9E40u)
        return 1;
    if (pc >= 0x00FCAC80u && pc <= 0x00FCAE40u)
        return 1;
    if (pc >= 0x00FCC980u && pc <= 0x00FCD620u)
        return 1;
    if (pc >= 0x00FC6300u && pc <= 0x00FC6500u)
        return 1;
    if (pc >= 0x00FE8800u && pc <= 0x00FE8848u)
        return 1;
    if (pc >= 0x00FCA480u && pc <= 0x00FCA568u)
        return 1;
    return 0;
}

static int harness_watch_gfx_addr(uint32_t addr)
{
    addr = bellatrix_bridge_normalize_addr(addr);

    if (addr >= 0x0000A4C0u && addr <= 0x0000A580u)
        return 1;
    if (addr >= 0x0000A572u && addr <= 0x0000A8C0u)
        return 1;
    if (addr >= 0x0000C4B2u && addr <= 0x0000C800u)
        return 1;

    return 0;
}

static int harness_watch_boot_pc(uint32_t pc)
{
    if (pc >= 0x00FC0718u && pc <= 0x00FC0788u)
        return 1;
    if (pc >= 0x00FE8530u && pc <= 0x00FE867Cu)
        return 1;
    if (pc >= 0x00FE960Cu && pc <= 0x00FE96FCu)
        return 1;
    if (pc >= 0x00FEA1D0u && pc <= 0x00FEA230u)
        return 1;
    if (pc >= 0x00FEAA5Cu && pc <= 0x00FEAAC2u)
        return 1;
    return 0;
}

static int harness_watch_boot_addr(uint32_t addr)
{
    addr = bellatrix_bridge_normalize_addr(addr);

    if (addr >= 0x000018B0u && addr <= 0x00001920u)
        return 1;

    switch (addr)
    {
        case 0x00BFE001u: /* CIAA PRA */
        case 0x00BFD100u: /* CIAB PRB */
        case 0x00DFF024u: /* DSKLEN */
        case 0x00DFF07Eu: /* DSKPTH */
        case 0x00DFF080u: /* DSKPTL */
        case 0x00DFF07Cu: /* DSKSYNC */
        case 0x00DFF09Au: /* INTENA */
        case 0x00DFF09Cu: /* INTREQ */
        case 0x00DFF09Eu: /* ADKCON */
        case 0x00DFF096u: /* DMACON */
            return 1;
        default:
            return 0;
    }
}

static int harness_watch_boot_payload_addr(uint32_t addr)
{
    addr = bellatrix_bridge_normalize_addr(addr);

    if (addr >= 0x0000A000u && addr <= 0x0000AFFFu)
        return 1;
    if (addr >= 0x0000C000u && addr <= 0x0000CFFFu)
        return 1;

    return 0;
}

static void harness_update_boot_display_block(uint32_t pc)
{
    uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
    uint32_t ctx = 0;
    uint32_t aux = 0;
    uint32_t buf0 = 0;
    uint32_t buf1 = 0;

    if (pc < 0x00FE8768u || pc > 0x00FE8960u)
        return;
    if (!bellatrix_chip_addr_contains_range(a5, 0x2Cu))
        return;

    ctx = harness_chip_read(a5 + 0x1Cu, 4);
    aux = harness_chip_read(a5 + 0x20u, 4);
    buf0 = harness_chip_read(a5 + 0x24u, 4);
    buf1 = harness_chip_read(a5 + 0x28u, 4);

    if (a5 == s_boot_display_a5 &&
        ctx == s_boot_display_ctx &&
        aux == s_boot_display_aux &&
        buf0 == s_boot_display_buf0 &&
        buf1 == s_boot_display_buf1)
        return;

    s_boot_display_a5 = a5;
    s_boot_display_ctx = ctx;
    s_boot_display_aux = aux;
    s_boot_display_buf0 = buf0;
    s_boot_display_buf1 = buf1;

    printf("[BOOT-DISPLAY-BLOCK] pc=%08x A5=%06x A5+1c=%08x A5+20=%08x "
           "A5+24=%08x A5+28=%08x\n",
           (unsigned)pc,
           (unsigned)a5,
           (unsigned)ctx,
           (unsigned)aux,
           (unsigned)buf0,
           (unsigned)buf1);
}

static int harness_is_a4d0_abort_pc(uint32_t pc)
{
    switch (pc)
    {
        case 0x00FCC992u:
        case 0x00FCC9E8u:
        case 0x00FCCB3Au:
        case 0x00FCCBF2u:
        case 0x00FCCF98u:
        case 0x00FCA660u:
        case 0x00FCD5BCu:
            return 1;
        default:
            return 0;
    }
}

static uint32_t harness_chip_read(uint32_t addr, int size)
{
    const BellatrixMemory *mem = &bellatrix_machine_get()->memory;

    if (size == 1) return bellatrix_chip_read8(mem, addr);
    if (size == 2) return bellatrix_chip_read16(mem, addr);
    return bellatrix_chip_read32(mem, addr);
}

static uint32_t harness_cpu_ram_read(uint32_t addr, int size)
{
    BellatrixMemory *mem = &bellatrix_machine_get()->memory;

    addr &= 0x00FFFFFFu;

    if (bellatrix_chip_addr_contains(addr))
        return harness_chip_read(addr, size);

    if (bellatrix_slow_contains(mem, addr, (unsigned int)size)) {
        if (size == 1) return bellatrix_slow_read8(mem, addr);
        if (size == 2) return bellatrix_slow_read16(mem, addr);
        return bellatrix_slow_read32(mem, addr);
    }

    if (size == 1) return bellatrix_mem_read8(mem, addr);
    if (size == 2) return bellatrix_mem_read16(mem, addr);
    return bellatrix_mem_read32(mem, addr);
}

static void harness_dump_regs(void)
{
    printf("[A4D0-REGS] SR=%04x"
           " D0=%08x D1=%08x D2=%08x D3=%08x D4=%08x D5=%08x D6=%08x D7=%08x"
           " A0=%08x A1=%08x A2=%08x A3=%08x A4=%08x A5=%08x A6=%08x A7=%08x\n",
           (unsigned)m68k_get_reg(NULL, M68K_REG_SR),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D0),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D1),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D2),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D3),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D4),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D5),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D6),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D7),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A0),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A1),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A2),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A3),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A4),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A5),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A6),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A7));
}

static void harness_dump_disasm(const char *tag, uint32_t pc)
{
    char buff[256];
    unsigned int ppc = (unsigned int)m68k_get_reg(NULL, M68K_REG_PPC);

    m68k_disassemble(buff, pc, s_cpu_type);
    printf("[A4D0-%s] PC %08x: %s\n", tag, (unsigned)pc, buff);

    if (ppc && ppc != pc)
    {
        m68k_disassemble(buff, ppc, s_cpu_type);
        printf("[A4D0-%s] PPC %08x: %s\n", tag, ppc, buff);
    }
}

static void harness_dump_callers(void)
{
    uint32_t sp = (uint32_t)m68k_get_reg(NULL, M68K_REG_A7) & 0x00FFFFFFu;
    uint32_t ret0 = 0;
    uint32_t ret1 = 0;
    uint32_t ret2 = 0;

    if (sp + 12u < bellatrix_machine_get()->memory.chip_ram_size)
    {
        ret0 = harness_chip_read(sp + 0u, 4);
        ret1 = harness_chip_read(sp + 4u, 4);
        ret2 = harness_chip_read(sp + 8u, 4);
    }

    printf("[A4D0-CALLER] A7=%08x RET0=%08x RET1=%08x RET2=%08x\n",
           (unsigned)sp,
           (unsigned)ret0,
           (unsigned)ret1,
           (unsigned)ret2);
}

static void harness_dump_a4d0_state(uint32_t pc)
{
    uint32_t addr;

    printf("[A4D0-DUMP] pc=%08x range=00a4c0..00a560\n", (unsigned)pc);
    for (addr = 0x0000A4C0u; addr <= 0x0000A560u; addr += 0x10u)
    {
        printf("[A4D0-DUMP] %06x: %08x %08x %08x %08x\n",
               (unsigned)addr,
               (unsigned)harness_chip_read(addr + 0x0u, 4),
               (unsigned)harness_chip_read(addr + 0x4u, 4),
               (unsigned)harness_chip_read(addr + 0x8u, 4),
               (unsigned)harness_chip_read(addr + 0xCu, 4));
    }

    printf("[A4D0-DUMP] a542.long=%08x a542.word=%04x a552=%08x a556=%08x a4d0.word=%04x a4d0.long=%08x\n",
           (unsigned)harness_chip_read(0x0000A542u, 4),
           (unsigned)harness_chip_read(0x0000A542u, 2),
           (unsigned)harness_chip_read(0x0000A552u, 4),
           (unsigned)harness_chip_read(0x0000A556u, 4),
           (unsigned)harness_chip_read(0x0000A4D0u, 2),
           (unsigned)harness_chip_read(0x0000A4D0u, 4));
}

static void harness_watch_a4d0(const char *tag, uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    if (!harness_boot_trace_enabled())
        return;

    if ((addr & 0x00FFFFFFu) != 0x0000A4D0u)
        return;

    printf("[A4D0-%s] pc=%08x addr=%06x size=%d val=%08x\n",
           tag,
           (unsigned)pc,
           (unsigned)addr,
           size,
           (unsigned)value);
    harness_dump_disasm(tag, pc);
    harness_dump_regs();
    harness_dump_callers();

    if (strstr(tag, "-R") != NULL && value == 0 && harness_is_a4d0_abort_pc(pc))
        harness_dump_a4d0_state(pc);
}

static void harness_watch_rw(const char *tag, uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    int boot_trace = harness_boot_trace_enabled();

    harness_watch_a4d0(tag, pc, addr, size, value);

    if (boot_trace && harness_watch_boot_pc(pc) && harness_watch_boot_addr(addr))
    {
        uint32_t a4 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu;
        uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
        uint32_t a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
        uint32_t a5_04 = 0;
        uint32_t a5_4c = 0;
        uint32_t a4_00 = 0;
        uint32_t a4_04 = 0;
        uint32_t vec_1c8 = 0;
        uint16_t vec_1c4 = 0;

        if (bellatrix_chip_addr_contains_range(a5, 0x50u))
        {
            a5_04 = harness_chip_read(a5 + 0x04u, 4);
            a5_4c = harness_chip_read(a5 + 0x4Cu, 4);
        }

        if (bellatrix_chip_addr_contains_range(a4, 0x08u))
        {
            a4_00 = harness_chip_read(a4 + 0x00u, 4);
            a4_04 = harness_chip_read(a4 + 0x04u, 4);
        }

        if (a6 >= 0x000001C8u &&
            bellatrix_chip_addr_contains_range(a6 - 0x000001C8u, 6u))
        {
            uint32_t vaddr = a6 - 0x000001C8u;
            vec_1c8 = harness_chip_read(vaddr + 0x00u, 4);
            vec_1c4 = (uint16_t)harness_chip_read(vaddr + 0x04u, 2);
        }

        printf("[BOOT-WATCH] tag=%s pc=%08x addr=%08x size=%d val=%08x "
               "D0=%08x D2=%08x A4=%06x A4+00=%08x A4+04=%08x "
               "A5=%06x A5+04=%08x A5+4c=%08x "
               "A6=%06x V-1c8=%08x:%04x\n",
               tag,
               (unsigned)pc,
               (unsigned)(bellatrix_bridge_normalize_addr(addr) & 0x00FFFFFFu),
               size,
               (unsigned)value,
               (unsigned)d0,
               (unsigned)d2,
               (unsigned)a4,
               (unsigned)a4_00,
               (unsigned)a4_04,
               (unsigned)a5,
               (unsigned)a5_04,
               (unsigned)a5_4c,
               (unsigned)a6,
               (unsigned)vec_1c8,
               (unsigned)vec_1c4);
    }

    if (!boot_trace) {
        if (strstr(tag, "WATCH-BPL-RAM") != NULL) {
            printf("[%s] pc=%08x addr=%08x size=%d val=%08x\n",
                   tag,
                   (unsigned)pc,
                   (unsigned)(addr & 0x00FFFFFFu),
                   size,
                   (unsigned)value);
        }
        return;
    }

    if (!harness_watch_gfx_pc(pc) && !harness_watch_gfx_addr(addr))
        return;

    printf("[%s] pc=%08x addr=%08x size=%d val=%08x\n",
           tag,
           (unsigned)pc,
           (unsigned)(addr & 0x00FFFFFFu),
           size,
           (unsigned)value);
}

static int harness_is_boot_bitplane_payload_addr(uint32_t addr)
{
    addr &= 0x00FFFFFFu;

    if (addr >= 0x0000A000u && addr < 0x0000B000u)
        return 1;
    if (addr >= 0x0000C000u && addr < 0x0000D000u)
        return 1;

    return 0;
}

static void harness_watch_boot_bitplane_write(uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    const char *target = "static";
    uint32_t addr24 = addr & 0x00FFFFFFu;

    if (!harness_is_boot_bitplane_payload_addr(addr24) || value == 0)
        return;

    if (addr24 >= s_boot_display_buf0 &&
        addr24 < s_boot_display_buf0 + BOOT_DISPLAY_BUF_SIZE)
        target = "BUF0";
    else if (addr24 >= s_boot_display_buf1 &&
             addr24 < s_boot_display_buf1 + BOOT_DISPLAY_BUF_SIZE)
        target = "BUF1";

    printf("[BOOT-DISPLAY-BLOCK-W] pc=%08x target=%s addr=%06x size=%d val=%08x\n",
           (unsigned)pc,
           target,
           (unsigned)addr24,
           size,
           (unsigned)value);
}

static void harness_watch_boot_dynamic_buffer_write(uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    uint32_t addr24 = addr & 0x00FFFFFFu;
    const char *target = NULL;

    if (value == 0)
        return;

    if (s_boot_display_buf0 != 0 &&
        addr24 >= s_boot_display_buf0 &&
        addr24 < s_boot_display_buf0 + BOOT_DISPLAY_BUF_SIZE)
        target = "BUF0";
    else if (s_boot_display_buf1 != 0 &&
             addr24 >= s_boot_display_buf1 &&
             addr24 < s_boot_display_buf1 + BOOT_DISPLAY_BUF_SIZE)
        target = "BUF1";

    if (!target)
        return;

    printf("[BOOT-DISPLAY-BUFFER-W] pc=%08x target=%s addr=%06x size=%d val=%08x\n",
           (unsigned)pc,
           target,
           (unsigned)addr24,
           size,
           (unsigned)value);
}

static uint32_t harness_mfm_long_at(uint32_t base, uint32_t word_index)
{
    return harness_chip_read(base + word_index * 2u, 4) & 0x55555555u;
}

static uint32_t harness_mfm_decode_long(uint32_t base, uint32_t word_index, uint32_t offset_words)
{
    uint32_t odd = harness_mfm_long_at(base, word_index);
    uint32_t even = harness_mfm_long_at(base, word_index + offset_words);
    return (odd << 1) | even;
}

static uint32_t harness_mfm_decode_long_chk(
    uint32_t base,
    uint32_t word_index,
    uint32_t offset_words,
    uint32_t *checksum)
{
    uint32_t odd = harness_mfm_long_at(base, word_index);
    uint32_t even = harness_mfm_long_at(base, word_index + offset_words);
    *checksum ^= odd ^ even;
    return (odd << 1) | even;
}

static void harness_decode_boot_dma(uint32_t *err_out, uint32_t *sector_bits_out, uint32_t *last_id_out)
{
    const uint32_t base = 0x0001660Cu;
    const uint32_t words = 0x397Cu / 2u;
    uint32_t raw = 0;
    uint32_t sector_bits = 0;
    uint32_t err = 0;
    uint32_t last_id = 0;

    while (sector_bits != 0x7ffu) {
        uint32_t rawnext;
        uint32_t checksum;
        uint32_t id;
        uint32_t dlong;
        uint32_t trackoffs;

        if (raw != 0u) {
            while (raw < words && (harness_chip_read(base + raw * 2u, 2) & 0xffffu) != 0x4489u)
                raw++;
        }
        while (raw < words && (harness_chip_read(base + raw * 2u, 2) & 0xffffu) == 0x4489u)
            raw++;
        if (raw + 544u >= words) {
            if (err == 0u)
                err = 26u;
            break;
        }

        rawnext = raw + 544u - 3u;
        checksum = 0;
        id = harness_mfm_decode_long_chk(base, raw, 2u, &checksum);
        last_id = id;
        raw += 4u;

        trackoffs = (id >> 8) & 0xffu;
        if (trackoffs >= 11u || (id & 0xff000000u) != 0xff000000u) {
            err = 27u;
            continue;
        }
        if ((sector_bits & (1u << trackoffs)) != 0u) {
            raw = rawnext;
            continue;
        }

        for (uint32_t i = 0; i < 4u; i++) {
            (void)harness_mfm_decode_long_chk(base, raw, 8u, &checksum);
            raw += 2u;
        }
        raw += 8u;
        dlong = harness_mfm_decode_long(base, raw, 2u);
        raw += 4u;
        if (dlong != checksum) {
            err = 24u;
            continue;
        }
        if (((id >> 16) & 0xffu) != 0u) {
            err = 27u;
            continue;
        }

        checksum = harness_mfm_decode_long(base, raw, 2u);
        raw += 4u;
        for (uint32_t i = 0; i < 128u; i++) {
            (void)harness_mfm_decode_long_chk(base, raw, 256u, &checksum);
            raw += 2u;
        }
        if (checksum != 0u) {
            err = 25u;
            continue;
        }
        sector_bits |= 1u << trackoffs;
    }

    *err_out = err;
    *sector_bits_out = sector_bits;
    *last_id_out = last_id;
}

static void harness_watch_dskblk_ack(uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
    uint32_t a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
    uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t bpl1 = (((bellatrix_machine_read(0x00DFF0E0u, 2) & 0x001Fu) << 16) |
                     (bellatrix_machine_read(0x00DFF0E2u, 2) & 0xFFFEu));
    uint32_t bpl2 = (((bellatrix_machine_read(0x00DFF0E4u, 2) & 0x001Fu) << 16) |
                     (bellatrix_machine_read(0x00DFF0E6u, 2) & 0xFFFEu));
    uint32_t cop1 = (((bellatrix_machine_read(0x00DFF080u, 2) & 0x001Fu) << 16) |
                     (bellatrix_machine_read(0x00DFF082u, 2) & 0xFFFEu));
    uint32_t cop2 = (((bellatrix_machine_read(0x00DFF084u, 2) & 0x001Fu) << 16) |
                     (bellatrix_machine_read(0x00DFF086u, 2) & 0xFFFEu));
    uint32_t p0 = harness_chip_read(0x0000A572u, 4);
    uint32_t p1 = harness_chip_read(0x0000C4B2u, 4);
    uint32_t boot0 = harness_chip_read(0x00015738u, 4);
    uint32_t boot1 = harness_chip_read(0x0001573Cu, 4);
    uint32_t boot2 = harness_chip_read(0x00015740u, 4);
    uint32_t boot3 = harness_chip_read(0x00015744u, 4);
    uint32_t raw0 = harness_chip_read(0x0001660Cu, 4);
    uint32_t raw1 = harness_chip_read(0x00016610u, 4);
    uint32_t dma0 = harness_chip_read(0x00006B14u, 4);
    uint32_t dma1 = harness_chip_read(0x00006B18u, 4);
    uint32_t sync0 = harness_chip_read(0x00007190u, 4);
    uint32_t sync1 = harness_chip_read(0x00007194u, 4);
    uint32_t decode_err = 0;
    uint32_t decode_bits = 0;
    uint32_t decode_id = 0;

    if ((addr & 0x00FFFFFFu) != 0x00DFF09Cu)
        return;
    if (size != 2)
        return;
    if ((value & 0xFFFFu) != 0x0002u &&
        (value & 0xFFFFu) != 0x1002u)
        return;

    s_boot_trace_ack_count++;
    s_boot_trace_until_pc = pc + 0x00002000u;
    harness_decode_boot_dma(&decode_err, &decode_bits, &decode_id);

    printf("[BOOT-DSKBLK-ACK] pc=%08x raw=%04x D0=%08x D1=%08x D2=%08x "
           "A5=%06x A6=%06x intena=%04x intreq=%04x dmacon=%04x bplcon0=%04x "
           "bpl1=%05x bpl2=%05x cop1=%05x cop2=%05x p0=%08x p1=%08x "
           "boot=%08x:%08x:%08x:%08x raw=%08x:%08x dma=%08x:%08x sync=%08x:%08x "
           "dec_err=%u dec_bits=%03x dec_id=%08x\n",
           (unsigned)pc,
           (unsigned)(value & 0xFFFFu),
           (unsigned)d0,
           (unsigned)d1,
           (unsigned)d2,
           (unsigned)a5,
           (unsigned)a6,
           (unsigned)(bellatrix_machine_read(0x00DFF01Cu, 2) & 0xFFFFu), /* INTENAR */
           (unsigned)(bellatrix_machine_read(0x00DFF01Eu, 2) & 0xFFFFu), /* INTREQR */
           (unsigned)(bellatrix_machine_read(0x00DFF002u, 2) & 0xFFFFu), /* DMACONR */
           (unsigned)(bellatrix_machine_read(0x00DFF102u, 2) & 0xFFFFu), /* BPLCON0 */
           (unsigned)bpl1,
           (unsigned)bpl2,
           (unsigned)cop1,
           (unsigned)cop2,
           (unsigned)p0,
           (unsigned)p1,
           (unsigned)boot0,
           (unsigned)boot1,
           (unsigned)boot2,
           (unsigned)boot3,
           (unsigned)raw0,
           (unsigned)raw1,
           (unsigned)dma0,
           (unsigned)dma1,
           (unsigned)sync0,
           (unsigned)sync1,
           (unsigned)decode_err,
           (unsigned)decode_bits,
           (unsigned)decode_id);
}

static void harness_trace_post_dskblk_window(uint32_t pc)
{
    uint32_t p0 = harness_chip_read(0x0000A572u, 4);
    uint32_t p1 = harness_chip_read(0x0000C4B2u, 4);

    if (s_boot_trace_until_pc == 0)
        return;
    if (pc < 0x00FC0000u || pc > s_boot_trace_until_pc)
        return;

    switch (pc)
    {
    case 0x00FC4AF0u:
    case 0x00FC5A6Cu:
    case 0x00FC5A78u:
    case 0x00FE881Au:
    case 0x00FE8820u:
    case 0x00FE8844u:
    case 0x00FCCBA2u:
    case 0x00FCCBB8u:
    case 0x00FCCBBEu:
    case 0x00FCCBC6u:
    case 0x00FCCBD2u:
    case 0x00FCCBF2u:
        break;
    default:
        return;
    }

    printf("[BOOT-AFTER-DSK] ack=%u pc=%08x dmacon=%04x bplcon0=%04x "
           "bpl1=%05x bpl2=%05x cop1=%05x cop2=%05x p0=%08x p1=%08x\n",
           (unsigned)s_boot_trace_ack_count,
           (unsigned)pc,
           (unsigned)(bellatrix_machine_read(0x00DFF002u, 2) & 0xFFFFu),
           (unsigned)(bellatrix_machine_read(0x00DFF102u, 2) & 0xFFFFu),
           (unsigned)((((uint32_t)bellatrix_machine_read(0x00DFF0E0u, 2) & 0x001Fu) << 16) |
                      ((uint32_t)bellatrix_machine_read(0x00DFF0E2u, 2) & 0xFFFEu)),
           (unsigned)((((uint32_t)bellatrix_machine_read(0x00DFF0E4u, 2) & 0x001Fu) << 16) |
                      ((uint32_t)bellatrix_machine_read(0x00DFF0E6u, 2) & 0xFFFEu)),
           (unsigned)((((uint32_t)bellatrix_machine_read(0x00DFF080u, 2) & 0x001Fu) << 16) |
                      ((uint32_t)bellatrix_machine_read(0x00DFF082u, 2) & 0xFFFEu)),
           (unsigned)((((uint32_t)bellatrix_machine_read(0x00DFF084u, 2) & 0x001Fu) << 16) |
                      ((uint32_t)bellatrix_machine_read(0x00DFF086u, 2) & 0xFFFEu)),
           (unsigned)p0,
           (unsigned)p1);
}

static void harness_probe_display_setup(uint32_t pc)
{
    uint32_t a5;
    uint32_t a5_08;
    uint32_t a5_0c;
    uint32_t a5_10;
    uint32_t a5_14;
    uint32_t a5_18;
    uint32_t a5_1c;
    uint32_t a5_20;
    uint32_t a5_24;
    uint32_t a5_28;

    if (pc < 0x00FE8768u || pc > 0x00FE8960u)
        return;

    a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
    if (!bellatrix_chip_addr_contains_range(a5, 0x2Cu))
        return;

    a5_08 = harness_chip_read(a5 + 0x08u, 4);
    a5_0c = harness_chip_read(a5 + 0x0Cu, 4);
    a5_10 = harness_chip_read(a5 + 0x10u, 4);
    a5_14 = harness_chip_read(a5 + 0x14u, 4);
    a5_18 = harness_chip_read(a5 + 0x18u, 4);
    a5_1c = harness_chip_read(a5 + 0x1Cu, 4);
    a5_20 = harness_chip_read(a5 + 0x20u, 4);
    a5_24 = harness_chip_read(a5 + 0x24u, 4);
    a5_28 = harness_chip_read(a5 + 0x28u, 4);

    printf("[BOOT-DISPLAY-SETUP] pc=%08x A5=%06x "
           "+08=%08x +0c=%08x +10=%08x +14=%08x +18=%08x "
           "+1c=%08x +20=%08x +24=%08x +28=%08x\n",
           (unsigned)pc,
           (unsigned)a5,
           (unsigned)a5_08,
           (unsigned)a5_0c,
           (unsigned)a5_10,
           (unsigned)a5_14,
           (unsigned)a5_18,
           (unsigned)a5_1c,
           (unsigned)a5_20,
           (unsigned)a5_24,
           (unsigned)a5_28);
}

static void harness_probe_display_writer(uint32_t pc)
{
    uint32_t a0;
    uint32_t a1;
    uint32_t a4;
    uint32_t a5;
    uint32_t a6;
    uint32_t d0;
    uint32_t d1;
    uint32_t d2;
    uint32_t d3;
    uint32_t a5_24 = 0;
    uint32_t a5_28 = 0;
    uint32_t sample0 = 0;
    uint32_t sample1 = 0;

    if (pc != 0x00FE9AAAu)
        return;

    a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
    a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
    a4 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu;
    a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
    a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
    d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
    d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
    d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
    d3 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D3);

    if (bellatrix_chip_addr_contains_range(a5, 0x2Cu))
    {
        a5_24 = harness_chip_read(a5 + 0x24u, 4);
        a5_28 = harness_chip_read(a5 + 0x28u, 4);
    }

    if (bellatrix_chip_addr_contains_range(a0, 8u))
    {
        sample0 = harness_chip_read(a0 + 0x00u, 4);
        sample1 = harness_chip_read(a0 + 0x04u, 4);
    }

    printf("[BOOT-DISPLAY-WRITER] pc=%08x D0=%08x D1=%08x D2=%08x D3=%08x "
           "A0=%06x A1=%06x A4=%06x A5=%06x A6=%06x "
           "A5+24=%08x A5+28=%08x A0[0]=%08x A0[4]=%08x\n",
           (unsigned)pc,
           (unsigned)d0,
           (unsigned)d1,
           (unsigned)d2,
           (unsigned)d3,
           (unsigned)a0,
           (unsigned)a1,
           (unsigned)a4,
           (unsigned)a5,
           (unsigned)a6,
           (unsigned)a5_24,
           (unsigned)a5_28,
           (unsigned)sample0,
           (unsigned)sample1);
}

static void harness_probe_happy_builder(void)
{
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

    if (pc < 0x00FCA484u || pc > 0x00FCA568u)
        return;

    if (pc == 0x00FCA484u || pc == 0x00FCA4B0u || pc == 0x00FCA4C6u ||
        pc == 0x00FCA4CCu || pc == 0x00FCA4F0u || pc == 0x00FCA528u ||
        pc == 0x00FCA52Cu || pc == 0x00FCA556u)
    {
        harness_dump_disasm("HH-BUILD", pc);
        printf("[HH-BUILD] pc=%08x A1=%08x A2=%08x A3=%08x "
               "D0=%08x D1=%08x D2=%08x D3=%08x D4=%08x "
               "a2+0e=%04x a2+10=%04x a2+11=%02x a2+13=%02x a2+18=%04x "
               "a3+02=%04x a3+04=%04x\n",
               (unsigned)pc,
               (unsigned)m68k_get_reg(NULL, M68K_REG_A1),
               (unsigned)m68k_get_reg(NULL, M68K_REG_A2),
               (unsigned)m68k_get_reg(NULL, M68K_REG_A3),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D0),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D1),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D2),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D3),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D4),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x0Eu) & 0x00FFFFFFu, 2),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x10u) & 0x00FFFFFFu, 2),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x11u) & 0x00FFFFFFu, 1),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x13u) & 0x00FFFFFFu, 1),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x18u) & 0x00FFFFFFu, 2),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A3)) + 0x02u) & 0x00FFFFFFu, 2),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A3)) + 0x04u) & 0x00FFFFFFu, 2));
    }
}

static void harness_probe_gfx_builder(void)
{
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

    switch (pc)
    {
        case 0x00FE881Au:
        case 0x00FE8820u:
        case 0x00FE8844u:
        case 0x00FCCBA2u:
        case 0x00FCCBB8u:
        case 0x00FCCBBEu:
        case 0x00FCCBC6u:
        case 0x00FCCBD2u:
        case 0x00FCCBF2u:
        case 0x00FC5A6Cu:
        case 0x00FC5A78u:
            break;
        default:
            return;
    }

    {
        uint32_t a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
        uint32_t a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
        uint32_t a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
        uint32_t a3 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A3) & 0x00FFFFFFu;
        uint32_t a4 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu;
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
        uint32_t d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
        uint32_t d3 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D3);
        uint32_t a542 = harness_chip_read(0x0000A542u, 4);
        uint32_t a552 = harness_chip_read(0x0000A552u, 4);
        uint32_t a556 = harness_chip_read(0x0000A556u, 4);
        uint32_t p0 = harness_chip_read(0x0000A572u, 4);
        uint32_t p1 = harness_chip_read(0x0000C4B2u, 4);

        harness_dump_disasm("GFX-BUILD", pc);
        printf("[GFX-BUILD] pc=%08x D0=%08x D1=%08x D2=%08x D3=%08x "
               "A0=%06x A1=%06x A2=%06x A3=%06x A4=%06x "
               "a542=%08x a552=%08x a556=%08x "
               "p0@a572=%08x p1@c4b2=%08x\n",
               (unsigned)pc,
               (unsigned)d0,
               (unsigned)d1,
               (unsigned)d2,
               (unsigned)d3,
               (unsigned)a0,
               (unsigned)a1,
               (unsigned)a2,
               (unsigned)a3,
               (unsigned)a4,
               (unsigned)a542,
               (unsigned)a552,
               (unsigned)a556,
               (unsigned)p0,
               (unsigned)p1);
    }
}

static void harness_probe_gfx_calls(uint32_t pc)
{
    switch (pc)
    {
        case 0x00FE8824u:
        case 0x00FE8836u:
        case 0x00FC5E2Cu:
        case 0x00FC63C6u:
            break;
        default:
            return;
    }

    {
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
        uint32_t a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
        uint32_t a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
        uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
        uint32_t a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
        uint32_t a5_10 = bellatrix_chip_addr_contains_range(a5 + 0x10u, 0x1cu)
                             ? harness_chip_read(a5 + 0x10u, 4)
                             : 0;
        uint32_t a5_14 = bellatrix_chip_addr_contains_range(a5 + 0x14u, 0x18u)
                             ? harness_chip_read(a5 + 0x14u, 4)
                             : 0;
        uint32_t a5_1c = bellatrix_chip_addr_contains_range(a5 + 0x1cu, 0x10u)
                             ? harness_chip_read(a5 + 0x1cu, 4)
                             : 0;
        uint32_t a5_24 = bellatrix_chip_addr_contains_range(a5 + 0x24u, 0x08u)
                             ? harness_chip_read(a5 + 0x24u, 4)
                             : 0;
        uint32_t a5_28 = bellatrix_chip_addr_contains_range(a5 + 0x28u, 0x04u)
                             ? harness_chip_read(a5 + 0x28u, 4)
                             : 0;

        harness_dump_disasm("GFX-CALL", pc);
        printf("[GFX-CALL] pc=%08x D0=%08x D1=%08x A0=%06x A1=%06x A5=%06x A6=%06x "
               "A5+10=%08x A5+14=%08x A5+1c=%08x A5+24=%08x A5+28=%08x\n",
               (unsigned)pc,
               (unsigned)d0,
               (unsigned)d1,
               (unsigned)a0,
               (unsigned)a1,
               (unsigned)a5,
               (unsigned)a6,
               (unsigned)a5_10,
               (unsigned)a5_14,
               (unsigned)a5_1c,
               (unsigned)a5_24,
               (unsigned)a5_28);
    }
}

static void harness_probe_boot_path(uint32_t pc)
{
    switch (pc)
    {
        case 0x00FE8560u:
        case 0x00FE8574u:
        case 0x00FE85A0u:
        case 0x00FE85AAu:
        case 0x00FE85C0u:
        case 0x00FE85CCu:
        case 0x00FE8600u:
        case 0x00FE8610u:
        case 0x00FE8616u:
        case 0x00FE861Au:
            break;
        default:
            return;
    }

    {
        uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
        uint32_t a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
        uint32_t a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
        uint32_t a4 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu;
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
        uint32_t a5_04 = 0;
        uint32_t a5_4c = 0;
        uint16_t a1_1c = 0;

        if (bellatrix_chip_addr_contains_range(a5, 0x50u))
        {
            a5_04 = harness_chip_read(a5 + 0x04u, 4);
            a5_4c = harness_chip_read(a5 + 0x4Cu, 4);
        }

        if (bellatrix_chip_addr_contains_range(a1, 0x1Eu))
            a1_1c = (uint16_t)harness_chip_read(a1 + 0x1Cu, 2);

        printf("[BOOT-PATH] pc=%08x D0=%08x D2=%08x A1=%06x A2=%06x A4=%06x A5=%06x "
               "A5+04=%08x A5+4c=%08x A1+1c=%04x\n",
               (unsigned)pc,
               (unsigned)d0,
               (unsigned)d2,
               (unsigned)a1,
               (unsigned)a2,
               (unsigned)a4,
               (unsigned)a5,
               (unsigned)a5_04,
               (unsigned)a5_4c,
               (unsigned)a1_1c);
    }
}

static void harness_probe_exec_io(uint32_t pc)
{
    switch (pc)
    {
        case 0x00FC0718u:
        case 0x00FC0726u:
        case 0x00FC072Eu:
        case 0x00FC0746u:
        case 0x00FC075Au:
        case 0x00FC0760u:
        case 0x00FC0780u:
            break;
        default:
            return;
    }

    {
        uint32_t a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
        uint32_t a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
        uint32_t a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
        uint32_t a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
        uint32_t req_ln_succ = 0;
        uint32_t req_ln_pred = 0;
        uint32_t req_dev = 0;
        uint32_t req_unit = 0;
        uint32_t req_reply_port = 0;
        uint16_t req_cmd = 0;
        uint8_t req_flags = 0;
        uint8_t req_err_raw = 0;
        int8_t req_err = 0;
        uint32_t req_actual = 0;
        uint32_t req_length = 0;
        uint32_t req_data = 0;
        uint32_t req_offset = 0;
        uint32_t unit_msgport = 0;
        uint8_t unit_num = 0;
        uint8_t req_rp_sigbit = 0;
        uint32_t req_rp_sigtask = 0;
        uint32_t req_rp_head = 0;
        uint8_t unit_mp_sigbit = 0;
        uint32_t unit_mp_sigtask = 0;
        uint32_t unit_mp_head = 0;

        if (bellatrix_chip_addr_contains_range(a1, 0x30u))
        {
            req_ln_succ = harness_chip_read(a1 + 0x00u, 4);
            req_ln_pred = harness_chip_read(a1 + 0x04u, 4);
            req_reply_port = harness_chip_read(a1 + 0x0Eu, 4);
            req_dev = harness_chip_read(a1 + 0x10u, 4);
            req_unit = harness_chip_read(a1 + 0x14u, 4);
            req_cmd = (uint16_t)harness_chip_read(a1 + 0x1Cu, 2);
            req_flags = (uint8_t)harness_chip_read(a1 + 0x1Eu, 1);
            req_err_raw = (uint8_t)harness_chip_read(a1 + 0x1Fu, 1);
            req_err = (int8_t)req_err_raw;
            req_actual = harness_chip_read(a1 + 0x20u, 4);
            req_length = harness_chip_read(a1 + 0x24u, 4);
            req_data = harness_chip_read(a1 + 0x28u, 4);
            req_offset = harness_chip_read(a1 + 0x2Cu, 4);
        }

        if (bellatrix_chip_addr_contains_range(req_unit, 0x10u))
        {
            unit_msgport = harness_chip_read(req_unit + 0x00u, 4);
            unit_num = (uint8_t)harness_chip_read(req_unit + 0x09u, 1);
        }
        if (bellatrix_chip_addr_contains_range(req_reply_port, 0x20u))
        {
            req_rp_sigbit = (uint8_t)harness_chip_read(req_reply_port + 0x0Fu, 1);
            req_rp_sigtask = harness_chip_read(req_reply_port + 0x10u, 4);
            req_rp_head = harness_chip_read(req_reply_port + 0x14u, 4);
        }
        if (bellatrix_chip_addr_contains_range(unit_msgport, 0x20u))
        {
            unit_mp_sigbit = (uint8_t)harness_chip_read(unit_msgport + 0x0Fu, 1);
            unit_mp_sigtask = harness_chip_read(unit_msgport + 0x10u, 4);
            unit_mp_head = harness_chip_read(unit_msgport + 0x14u, 4);
        }

        printf("[EXEC-IO] pc=%08x D0=%08x D1=%08x A0=%06x A1=%06x A2=%06x A6=%06x "
               "ln=%08x/%08x dev=%06x unit=%06x cmd=%04x flags=%02x err=%d/%02x "
               "actual=%08x length=%08x data=%06x offset=%08x "
               "reply=%06x rp_sig=%u rp_task=%06x rp_head=%06x "
               "unit_mp=%06x unit_num=%u ump_sig=%u ump_task=%06x ump_head=%06x%s\n",
               (unsigned)pc,
               (unsigned)d0,
               (unsigned)d1,
               (unsigned)a0,
               (unsigned)a1,
               (unsigned)a2,
               (unsigned)a6,
               (unsigned)req_ln_succ,
               (unsigned)req_ln_pred,
               (unsigned)req_dev,
               (unsigned)req_unit,
               (unsigned)req_cmd,
               (unsigned)req_flags,
               (int)req_err,
               (unsigned)req_err_raw,
               (unsigned)req_actual,
               (unsigned)req_length,
               (unsigned)req_data,
               (unsigned)req_offset,
               (unsigned)req_reply_port,
               (unsigned)req_rp_sigbit,
               (unsigned)req_rp_sigtask,
               (unsigned)req_rp_head,
               (unsigned)unit_msgport,
               (unsigned)unit_num,
               (unsigned)unit_mp_sigbit,
               (unsigned)unit_mp_sigtask,
               (unsigned)unit_mp_head,
               req_err_raw == 26u ? " err_name=TDERR_TooFewSecs" : "");
    }
}

static void harness_trace_pc_range(uint32_t pc)
{
    char disasm[256];
    uint32_t a1;
    uint32_t a5;
    uint16_t req_cmd = 0;
    uint8_t req_err_raw = 0;
    int8_t req_err = 0;
    uint32_t req_actual = 0;
    uint32_t req_length = 0;
    uint32_t req_data = 0;
    uint32_t req_offset = 0;
    uint32_t a5_4c = 0;

    if (!harness_trace_pc_range_enabled(pc))
        return;

    a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
    a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;

    if (bellatrix_chip_addr_contains_range(a1, 0x30u))
    {
        req_cmd = (uint16_t)harness_chip_read(a1 + 0x1Cu, 2);
        req_err_raw = (uint8_t)harness_chip_read(a1 + 0x1Fu, 1);
        req_err = (int8_t)req_err_raw;
        req_actual = harness_chip_read(a1 + 0x20u, 4);
        req_length = harness_chip_read(a1 + 0x24u, 4);
        req_data = harness_chip_read(a1 + 0x28u, 4);
        req_offset = harness_chip_read(a1 + 0x2Cu, 4);
    }

    if (bellatrix_chip_addr_contains_range(a5, 0x50u))
        a5_4c = harness_chip_read(a5 + 0x4Cu, 4);

    m68k_disassemble(disasm, pc, s_cpu_type);
    printf("[TRACE-PC] pc=%08x sr=%04x %s "
           "D0=%08x D1=%08x D2=%08x D3=%08x "
           "A0=%06x A1=%06x A2=%06x A3=%06x A4=%06x A5=%06x A6=%06x A7=%06x "
           "cmd=%04x err=%d/%02x actual=%08x length=%08x data=%06x offset=%08x A5+4c=%08x\n",
           (unsigned)pc,
           (unsigned)m68k_get_reg(NULL, M68K_REG_SR),
           disasm,
           (unsigned)m68k_get_reg(NULL, M68K_REG_D0),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D1),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D2),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D3),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu),
           (unsigned)a1,
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A3) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu),
           (unsigned)a5,
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu),
           (unsigned)((uint32_t)m68k_get_reg(NULL, M68K_REG_A7) & 0x00FFFFFFu),
           (unsigned)req_cmd,
           (int)req_err,
           (unsigned)req_err_raw,
           (unsigned)req_actual,
           (unsigned)req_length,
           (unsigned)req_data,
           (unsigned)req_offset,
           (unsigned)a5_4c);
}

static void harness_probe_vbl_dispatch_node(uint32_t pc)
{
    static int s_done = 0;
    if (pc != 0x00FC182Au)
        return;

    uint32_t a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;

    /* Only fire when A0 is in the callback node area we care about */
    if (a0 < 0x1880u || a0 > 0x1900u)
        return;
    if (s_done)
        return;
    s_done = 1;

    printf("[VBL-DISP-NODE] pc=%08x A0=%08x\n", (unsigned)pc, (unsigned)a0);

    /* Dump 64 bytes starting 16 bytes before A0 */
    uint32_t base = (a0 >= 0x10u) ? (a0 - 0x10u) : 0u;
    uint32_t i;
    for (i = 0; i < 64u; i += 4u) {
        uint32_t addr = base + i;
        uint32_t v = harness_chip_read(addr, 4);
        printf("[VBL-DISP-NODE]   %06x: %08x\n", (unsigned)addr, (unsigned)v);
    }
}

static int harness_exec_call_trace_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("HARNESS_EXEC_CALL_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    return enabled;
}

static int harness_signal_probe_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("HARNESS_SIGNAL_PROBE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    return enabled;
}

static int harness_library_call_trace_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("HARNESS_LIBRARY_CALL_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    return enabled;
}

static uint32_t harness_read_ram32(uint32_t addr)
{
    return harness_cpu_ram_read(addr, 4);
}

static uint16_t harness_read_ram16(uint32_t addr)
{
    return (uint16_t)harness_cpu_ram_read(addr, 2);
}

static uint8_t harness_read_ram8(uint32_t addr)
{
    return (uint8_t)harness_cpu_ram_read(addr, 1);
}

static void harness_write_ram32(uint32_t addr, uint32_t value)
{
    BellatrixMemory *mem = &bellatrix_machine_get()->memory;

    addr &= 0x00FFFFFFu;
    if (bellatrix_chip_addr_contains(addr)) {
        bellatrix_chip_write32(mem, addr, value);
        return;
    }
    bellatrix_mem_write32(mem, addr, value);
}

static int harness_ram_addr_valid(uint32_t addr, unsigned int size)
{
    BellatrixMemory *mem = &bellatrix_machine_get()->memory;

    addr &= 0x00FFFFFFu;
    if (bellatrix_chip_addr_contains(addr))
        return 1;
    return bellatrix_slow_contains(mem, addr, size);
}

/* HARNESS_WATCH_MEM=addr:len[,addr:len...] — log every CPU write that lands
 * inside a watched range, with the PC that issued it. */
#define HARNESS_MAX_MEM_WATCH 8
static struct { uint32_t base; uint32_t len; } s_mem_watch[HARNESS_MAX_MEM_WATCH];
static int s_mem_watch_count = -1;

static void harness_mem_watch_init(void)
{
    const char *env = getenv("HARNESS_WATCH_MEM");
    char buf[256];
    char *item, *save = NULL;

    s_mem_watch_count = 0;
    if (!env || !env[0])
        return;

    strncpy(buf, env, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    for (item = strtok_r(buf, ",", &save);
         item && s_mem_watch_count < HARNESS_MAX_MEM_WATCH;
         item = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(item, ':');
        uint32_t base, len = 4;
        if (colon) {
            *colon = '\0';
            len = (uint32_t)strtoul(colon + 1, NULL, 0);
        }
        base = (uint32_t)strtoul(item, NULL, 0);
        if (!len) len = 4;
        s_mem_watch[s_mem_watch_count].base = base & 0x00FFFFFFu;
        s_mem_watch[s_mem_watch_count].len = len;
        s_mem_watch_count++;
        fprintf(stderr, "[WATCH-MEM] armed 0x%06x..0x%06x\n",
                s_mem_watch[s_mem_watch_count - 1].base,
                s_mem_watch[s_mem_watch_count - 1].base + len - 1);
    }
}

static void harness_mem_watch_write(uint32_t pc, uint32_t addr, uint32_t value, int size)
{
    int i;

    if (s_mem_watch_count < 0)
        harness_mem_watch_init();

    for (i = 0; i < s_mem_watch_count; i++) {
        if (addr + (uint32_t)size > s_mem_watch[i].base &&
            addr < s_mem_watch[i].base + s_mem_watch[i].len) {
            fprintf(stderr,
                    "[WATCH-MEM] W%d 0x%06x = 0x%0*x  pc=0x%06x\n",
                    size, addr, size * 2, value, pc);
            break;
        }
    }
}

static int harness_msgport_owner_fix_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("HARNESS_MSGPORT_OWNER_FIX");
        const char *legacy = getenv("HARNESS_TRACKDISK_WAITPORT_OWNER_FIX");
        enabled =
            ((env && env[0] != '\0' && env[0] != '0') ||
             (legacy && legacy[0] != '\0' && legacy[0] != '0')) ? 1 : 0;
    }

    return enabled;
}

static uint32_t harness_exec_lvo_target(uint32_t eb, uint32_t lvo)
{
    uint32_t stub = (eb - lvo * 6u) & 0x00FFFFFFu;
    uint16_t op = harness_read_ram16(stub);

    if (op == 0x4EF9u)
        return harness_read_ram32(stub + 2u) & 0x00FFFFFFu;
    return 0;
}

static void harness_task_name(uint32_t task, char out[32])
{
    uint32_t name;
    int i;

    out[0] = '\0';
    if (task < 0x400u)
        return;

    name = harness_read_ram32(task + 10u) & 0x00FFFFFFu;
    if (name < 0x400u)
        return;

    for (i = 0; i < 31; i++) {
        char c = (char)harness_read_ram8(name + (uint32_t)i);
        if (!c)
            break;
        if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7eu)
            c = '?';
        out[i] = c;
        out[i + 1] = '\0';
    }
}

static void harness_read_cstring(uint32_t addr, char out[64])
{
    int i;

    out[0] = '\0';
    if (addr < 0x400u)
        return;

    for (i = 0; i < 63; i++) {
        char c = (char)harness_read_ram8(addr + (uint32_t)i);
        if (!c)
            break;
        if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7eu)
            c = '?';
        out[i] = c;
        out[i + 1] = '\0';
    }
}

static int harness_exec_call_seen(int call_id, uint32_t task, uint32_t d0,
                                  uint32_t d1, uint32_t a0, uint32_t a1,
                                  uint32_t ret)
{
    enum { MAX_SEEN = 1024 };
    struct SeenCall {
        int call_id;
        uint32_t task;
        uint32_t d0;
        uint32_t d1;
        uint32_t a0;
        uint32_t a1;
        uint32_t ret;
    };
    static struct SeenCall seen[MAX_SEEN];
    static unsigned count = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        if (seen[i].call_id == call_id && seen[i].task == task &&
            seen[i].d0 == d0 && seen[i].d1 == d1 &&
            seen[i].a0 == a0 && seen[i].a1 == a1 &&
            seen[i].ret == ret) {
            return 1;
        }
    }

    if (count < MAX_SEEN) {
        seen[count].call_id = call_id;
        seen[count].task = task;
        seen[count].d0 = d0;
        seen[count].d1 = d1;
        seen[count].a0 = a0;
        seen[count].a1 = a1;
        seen[count].ret = ret;
        count++;
        return 0;
    }

    return 1;
}

static void harness_trace_exec_call(uint32_t pc)
{
    static uint32_t s_eb = 0;
    static uint32_t s_setsignal = 0;
    static uint32_t s_wait = 0;
    static uint32_t s_putmsg = 0;
    static uint32_t s_replymsg = 0;
    static uint32_t s_signal = 0;
    static uint32_t s_waitport = 0;
    static uint32_t s_waitio = 0;
    const char *name = NULL;
    int call_id = 0;
    uint32_t eb;
    uint32_t task;
    uint32_t sp;
    uint32_t ret;
    uint32_t d0;
    uint32_t d1;
    uint32_t a0;
    uint32_t a1;
    uint32_t a6;
    char task_name[32];
    int trace_enabled = harness_exec_call_trace_enabled();
    int owner_fix_enabled = harness_msgport_owner_fix_enabled();
    int signal_probe = harness_signal_probe_enabled();

    if (!trace_enabled && !owner_fix_enabled && !signal_probe)
        return;

    eb = harness_chip_read(4u, 4) & 0x00FFFFFFu;
    if (!harness_ram_addr_valid(eb, 4u))
        return;

    if (eb != s_eb || s_wait == 0u || s_signal == 0u ||
            s_putmsg == 0u || s_replymsg == 0u ||
            s_waitport == 0u || s_waitio == 0u) {
        uint32_t old_setsignal = s_setsignal;
        uint32_t old_wait = s_wait;
        uint32_t old_signal = s_signal;
        uint32_t old_putmsg = s_putmsg;
        uint32_t old_replymsg = s_replymsg;
        uint32_t old_waitport = s_waitport;
        uint32_t old_waitio = s_waitio;
        s_eb = eb;
        s_setsignal = harness_exec_lvo_target(eb, 51);
        s_wait = harness_exec_lvo_target(eb, 53);
        s_putmsg = harness_exec_lvo_target(eb, 61);
        s_replymsg = harness_exec_lvo_target(eb, 63);
        s_signal = harness_exec_lvo_target(eb, 127);
        s_waitport = harness_exec_lvo_target(eb, 64);
        s_waitio = harness_exec_lvo_target(eb, 79);
        if (trace_enabled && s_wait != 0u &&
                (s_setsignal != old_setsignal || s_wait != old_wait ||
                 s_signal != old_signal || s_putmsg != old_putmsg ||
                 s_replymsg != old_replymsg || s_waitport != old_waitport ||
                 s_waitio != old_waitio)) {
            printf("[EXEC-CALL-TRACE] eb=%08x SetSignal=%08x Wait=%08x Signal=%08x PutMsg=%08x ReplyMsg=%08x WaitPort=%08x WaitIO=%08x\n",
                   (unsigned)eb, (unsigned)s_setsignal, (unsigned)s_wait,
                   (unsigned)s_signal, (unsigned)s_putmsg,
                   (unsigned)s_replymsg, (unsigned)s_waitport,
                   (unsigned)s_waitio);
        }
    }

    pc &= 0x00FFFFFFu;
    if (pc == s_setsignal) { name = "SetSignal"; call_id = 1; }
    else if (pc == s_wait) { name = "Wait"; call_id = 2; }
    else if (pc == s_putmsg) { name = "PutMsg"; call_id = 3; }
    else if (pc == s_replymsg) { name = "ReplyMsg"; call_id = 4; }
    else if (pc == s_signal) { name = "Signal"; call_id = 5; }
    else if (pc == s_waitport) { name = "WaitPort"; call_id = 6; }
    else if (pc == s_waitio) { name = "WaitIO"; call_id = 7; }
    else return;

    task = harness_read_ram32(s_eb + 276u) & 0x00FFFFFFu;
    harness_task_name(task, task_name);
    sp = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP) & 0x00FFFFFFu;
    ret = harness_ram_addr_valid(sp, 4u) ? (harness_read_ram32(sp) & 0x00FFFFFFu) : 0;
    d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
    d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
    a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0);
    a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1);
    a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6);

    if (call_id == 6 && owner_fix_enabled) {
        uint32_t port = a0 & 0x00FFFFFFu;
        if (harness_ram_addr_valid(port + 16u, 4u)) {
            uint32_t sigtask = harness_read_ram32(port + 16u) & 0x00FFFFFFu;
            uint8_t flags = harness_read_ram8(port + 14u);
            if (flags == 0u && sigtask != 0u && sigtask != task) {
                printf("[MSGPORT-OWNER-FIX] WaitPort port=%08x sigtask:%08x->%08x\n",
                       (unsigned)port, (unsigned)sigtask, (unsigned)task);
                harness_write_ram32(port + 16u, task);
            }
        }
    }
    if (call_id == 7 && owner_fix_enabled) {
        uint32_t ioreq = a1 & 0x00FFFFFFu;
        if (harness_ram_addr_valid(ioreq + 18u, 4u)) {
            uint32_t port = harness_read_ram32(ioreq + 14u) & 0x00FFFFFFu;
            if (port != 0u && harness_ram_addr_valid(port + 16u, 4u)) {
                uint32_t sigtask = harness_read_ram32(port + 16u) & 0x00FFFFFFu;
                uint8_t flags = harness_read_ram8(port + 14u);
                if (flags == 0u && sigtask != 0u && sigtask != task) {
                    printf("[MSGPORT-OWNER-FIX] WaitIO ioreq=%08x port=%08x sigtask:%08x->%08x\n",
                           (unsigned)ioreq, (unsigned)port,
                           (unsigned)sigtask, (unsigned)task);
                    harness_write_ram32(port + 16u, task);
                }
            }
        }
    }

    /* HARNESS_SIGNAL_PROBE: print Signal/ReplyMsg calls that deliver to bits
     * outside the target task's tc_SigWait (spurious signal detection).
     * tc_SigWait is at offset 22 (0x16) in the Task struct. */
    if (signal_probe) {
        if (call_id == 5) {
            /* Signal(task=a0, mask=d0) */
            uint32_t tgt = a0 & 0x00FFFFFFu;
            if (harness_ram_addr_valid(tgt + 26u, 4u)) {
                uint32_t sigwait  = harness_read_ram32(tgt + 22u);
                uint32_t sigrecvd = harness_read_ram32(tgt + 26u);
                char tgt_name[32];
                harness_task_name(tgt, tgt_name);
                if (sigwait != 0u && (d0 & sigwait) == 0u) {
                    printf("[SPURIOUS-SIGNAL] caller_task=%08x \"%s\" "
                           "→ Signal(tgt=%08x \"%s\", mask=%08x) "
                           "sigwait=%08x sigrecvd=%08x ret=%08x\n",
                           (unsigned)task, task_name,
                           (unsigned)tgt, tgt_name,
                           (unsigned)d0,
                           (unsigned)sigwait, (unsigned)sigrecvd,
                           (unsigned)ret);
                }
            }
        } else if (call_id == 4) {
            /* ReplyMsg(msg=a1): will signal msg->mn_ReplyPort->mp_SigTask */
            uint32_t msg = a1 & 0x00FFFFFFu;
            if (harness_ram_addr_valid(msg + 18u, 4u)) {
                uint32_t rport = harness_read_ram32(msg + 14u) & 0x00FFFFFFu;
                if (rport && harness_ram_addr_valid(rport + 20u, 4u)) {
                    uint8_t  sigbit  = harness_read_ram8(rport + 15u);
                    uint32_t sigtask = harness_read_ram32(rport + 16u) & 0x00FFFFFFu;
                    uint32_t smask   = 1u << sigbit;
                    if (sigtask && harness_ram_addr_valid(sigtask + 26u, 4u)) {
                        uint32_t sigwait  = harness_read_ram32(sigtask + 22u);
                        uint32_t sigrecvd = harness_read_ram32(sigtask + 26u);
                        char tgt_name[32];
                        harness_task_name(sigtask, tgt_name);
                        if (sigwait != 0u && (smask & sigwait) == 0u) {
                            printf("[SPURIOUS-REPLY] caller_task=%08x \"%s\" "
                                   "→ ReplyMsg→Signal(tgt=%08x \"%s\", bit=%u mask=%08x) "
                                   "sigwait=%08x sigrecvd=%08x msg=%08x rport=%08x ret=%08x\n",
                                   (unsigned)task, task_name,
                                   (unsigned)sigtask, tgt_name,
                                   (unsigned)sigbit, (unsigned)smask,
                                   (unsigned)sigwait, (unsigned)sigrecvd,
                                   (unsigned)msg, (unsigned)rport,
                                   (unsigned)ret);
                        }
                    }
                }
            }
        }
    }

    if (!trace_enabled)
        return;

    if (harness_exec_call_seen(call_id, task, d0, d1, a0, a1, ret))
        return;

    printf("[EXEC-CALL] pc=%08x %-9s task=%08x \"%s\" "
           "ret=%08x SP=%08x D0=%08x D1=%08x A0=%08x A1=%08x A6=%08x",
           (unsigned)pc,
           name,
           (unsigned)task,
           task_name,
           (unsigned)ret,
           (unsigned)sp,
           (unsigned)d0,
           (unsigned)d1,
           (unsigned)a0,
           (unsigned)a1,
           (unsigned)a6);
    if (call_id == 2) {
        printf(" stk=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
               (unsigned)harness_read_ram32(sp + 0u),
               (unsigned)harness_read_ram32(sp + 4u),
               (unsigned)harness_read_ram32(sp + 8u),
               (unsigned)harness_read_ram32(sp + 12u),
               (unsigned)harness_read_ram32(sp + 16u),
               (unsigned)harness_read_ram32(sp + 20u),
               (unsigned)harness_read_ram32(sp + 24u),
               (unsigned)harness_read_ram32(sp + 28u));
    }
    if (call_id == 3 || call_id == 6 || call_id == 7) {
        uint32_t port = (call_id == 3) ? (a0 & 0x00FFFFFFu) : (a0 & 0x00FFFFFFu);
        if (call_id == 7 && harness_ram_addr_valid(a1 + 18u, 4u))
            port = harness_read_ram32((a1 & 0x00FFFFFFu) + 14u) & 0x00FFFFFFu;
        uint32_t sigtask = harness_read_ram32(port + 16u) & 0x00FFFFFFu;
        uint8_t flags = harness_read_ram8(port + 14u);
        uint8_t sigbit = harness_read_ram8(port + 15u);
        uint32_t head = harness_read_ram32(port + 20u) & 0x00FFFFFFu;
        uint32_t tailpred = harness_read_ram32(port + 28u) & 0x00FFFFFFu;
        char sigtask_name[32];

        harness_task_name(sigtask, sigtask_name);
        printf(" port=%08x port_flags=%02x port_sigbit=%u port_sigtask=%08x \"%s\""
               " msg_head=%08x msg_tailpred=%08x msg_reply=%08x",
               (unsigned)port,
               (unsigned)flags,
               (unsigned)sigbit,
               (unsigned)sigtask,
               sigtask_name,
               (unsigned)head,
               (unsigned)tailpred,
               (unsigned)(harness_ram_addr_valid(a1 + 14u, 4u)
                          ? (harness_read_ram32(a1 + 14u) & 0x00FFFFFFu)
                          : 0u));
    }
    if (call_id == 4 && harness_ram_addr_valid(a1, 20u)) {
        uint32_t reply = harness_read_ram32(a1 + 14u) & 0x00FFFFFFu;
        printf(" msg_reply=%08x", (unsigned)reply);
    }
    printf("\n");
}

static void harness_trace_library_call(uint32_t pc)
{
    static uint32_t s_a6;
    static uint32_t s_createproc;
    static uint32_t s_loadseg;
    static uint32_t s_execute;
    static uint32_t s_createnewproc;
    static uint32_t s_addsegment;
    static uint32_t s_findsegment;
    static uint32_t s_startworkbench;
    static uint32_t s_printfault;
    static uint32_t s_displayerror;
    static uint32_t s_easyrequestargs;
    static uint32_t s_builderequestargs;
    static uint32_t s_sysreqhandler;
    const char *name = NULL;
    int call_id = 0;
    uint32_t task;
    uint32_t sp;
    uint32_t ret;
    uint32_t d0;
    uint32_t d1;
    uint32_t d2;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
    uint32_t a6;
    char task_name[32];
    char a0s[64];
    char d1s[64];

    if (!harness_library_call_trace_enabled())
        return;

    pc &= 0x00FFFFFFu;
    a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
    if (!harness_ram_addr_valid(a6, 4u))
        return;

    if (a6 != s_a6) {
        s_a6 = a6;
        s_createproc = harness_exec_lvo_target(a6, 23);
        s_loadseg = harness_exec_lvo_target(a6, 25);
        s_execute = harness_exec_lvo_target(a6, 37);
        s_createnewproc = harness_exec_lvo_target(a6, 83);
        s_addsegment = harness_exec_lvo_target(a6, 129);
        s_findsegment = harness_exec_lvo_target(a6, 130);
        s_startworkbench = harness_exec_lvo_target(a6, 7);
        s_printfault = harness_exec_lvo_target(a6, 79);
        s_displayerror = harness_exec_lvo_target(a6, 81);
        s_easyrequestargs = harness_exec_lvo_target(a6, 98);
        s_builderequestargs = harness_exec_lvo_target(a6, 99);
        s_sysreqhandler = harness_exec_lvo_target(a6, 100);
    }

    if (pc == s_createproc) { name = "CreateProc"; call_id = 101; }
    else if (pc == s_loadseg) { name = "LoadSeg"; call_id = 102; }
    else if (pc == s_execute) { name = "Execute"; call_id = 103; }
    else if (pc == s_createnewproc) { name = "CreateNewProc"; call_id = 104; }
    else if (pc == s_addsegment) { name = "AddSegment"; call_id = 105; }
    else if (pc == s_findsegment) { name = "FindSegment"; call_id = 106; }
    else if (pc == s_startworkbench) { name = "StartWorkbench"; call_id = 107; }
    else if (pc == s_printfault) { name = "PrintFault"; call_id = 108; }
    else if (pc == s_displayerror) { name = "DisplayError"; call_id = 109; }
    else if (pc == s_easyrequestargs) { name = "EasyRequestArgs"; call_id = 110; }
    else if (pc == s_builderequestargs) { name = "BuildEasyRequestArgs"; call_id = 111; }
    else if (pc == s_sysreqhandler) { name = "SysReqHandler"; call_id = 112; }
    else return;

    task = harness_read_ram32(harness_chip_read(4u, 4) + 276u) & 0x00FFFFFFu;
    harness_task_name(task, task_name);
    sp = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP) & 0x00FFFFFFu;
    ret = harness_ram_addr_valid(sp, 4u) ? (harness_read_ram32(sp) & 0x00FFFFFFu) : 0;
    d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
    d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
    d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
    a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
    a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
    a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
    a3 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A3) & 0x00FFFFFFu;
    harness_read_cstring(a0, a0s);
    harness_read_cstring(d1 & 0x00FFFFFFu, d1s);

    if (harness_exec_call_seen(call_id, task, d0, d1, a0, a1, ret))
        return;

    printf("[LIB-CALL-TRACE] %s base=%08x pc=%08x ret=%08x task=%08x \"%s\" "
           "D0=%08x D1=%08x \"%s\" D2=%08x D3=%08x A0=%08x \"%s\" A1=%08x\n",
           name,
           (unsigned)a6,
           (unsigned)pc,
           (unsigned)ret,
           (unsigned)task,
           task_name,
           (unsigned)d0,
           (unsigned)d1,
           d1s,
           (unsigned)m68k_get_reg(NULL, M68K_REG_D2),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D3),
           (unsigned)a0,
           a0s,
           (unsigned)a1);

    if (call_id == 108) {
        char header[64];
        harness_read_cstring(d2 & 0x00FFFFFFu, header);
        printf("[LIB-CALL-DETAIL] PrintFault code=%d header_ptr=%08x header=\"%s\"\n",
               (int32_t)d1,
               (unsigned)(d2 & 0x00FFFFFFu),
               header);
    } else if (call_id == 109) {
        char fmt[64];
        harness_read_cstring(a0, fmt);
        printf("[LIB-CALL-DETAIL] DisplayError format=\"%s\" flags=%08x args=%08x\n",
               fmt,
               (unsigned)d0,
               (unsigned)a1);
    } else if ((call_id == 110 || call_id == 111) &&
            harness_ram_addr_valid(a1 + 20u, 4u)) {
        char title[64];
        char text[64];
        char gadgets[64];
        uint32_t title_ptr = harness_read_ram32(a1 + 8u) & 0x00FFFFFFu;
        uint32_t text_ptr = harness_read_ram32(a1 + 12u) & 0x00FFFFFFu;
        uint32_t gadgets_ptr = harness_read_ram32(a1 + 16u) & 0x00FFFFFFu;
        harness_read_cstring(title_ptr, title);
        harness_read_cstring(text_ptr, text);
        harness_read_cstring(gadgets_ptr, gadgets);
        printf("[LIB-CALL-DETAIL] %s easy=%08x title=\"%s\" text=\"%s\" gadgets=\"%s\" idcmp=%08x args=%08x\n",
               name,
               (unsigned)a1,
               title,
               text,
               gadgets,
               (unsigned)((call_id == 110 && harness_ram_addr_valid(a2, 4u))
                          ? harness_read_ram32(a2) : d0),
               (unsigned)a3);
    } else if (call_id == 112) {
        printf("[LIB-CALL-DETAIL] SysReqHandler window=%08x idcmp_ptr=%08x wait=%u\n",
               (unsigned)a0,
               (unsigned)a1,
               (unsigned)(d0 & 0xFFu));
    }
}

/* HARNESS_PC_BURST=pc:count[:skip] — when PC first hits `pc` (after `skip`
 * hits), log the next `count` instruction PCs. One-shot. */
static void harness_pc_burst(uint32_t pc)
{
    static int inited = 0;
    static uint32_t trig = 0, remaining = 0, skip = 0;
    static int armed = 0;

    if (!inited) {
        const char *env = getenv("HARNESS_PC_BURST");
        inited = 1;
        if (env && env[0]) {
            char buf[64];
            strncpy(buf, env, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *c1 = strchr(buf, ':');
            if (c1) {
                *c1 = '\0';
                char *c2 = strchr(c1 + 1, ':');
                if (c2) { *c2 = '\0'; skip = (uint32_t)strtoul(c2 + 1, NULL, 0); }
                trig = (uint32_t)strtoul(buf, NULL, 0) & 0x00FFFFFFu;
                remaining = (uint32_t)strtoul(c1 + 1, NULL, 0);
                armed = (trig && remaining) ? 1 : 0;
                if (armed)
                    fprintf(stderr, "[PC-BURST] armed trig=0x%06x count=%u skip=%u\n",
                            trig, remaining, skip);
            }
        }
    }

    if (!armed)
        return;

    if (armed == 1) {
        if ((pc & 0x00FFFFFFu) != trig)
            return;
        if (skip) { skip--; return; }
        armed = 2;
    }

    fprintf(stderr, "[PC-BURST] pc=%06x sr=%04x\n",
            pc & 0x00FFFFFFu, (unsigned)m68k_get_reg(NULL, M68K_REG_SR));
    if (--remaining == 0)
        armed = 0;
}

/* HARNESS_PC_RING=trigpc:depth:d0 — keep a ring of the last `depth` PCs;
 * when PC hits trigpc with D0 == d0, dump the ring (history) and disarm. */
#define HARNESS_PC_RING_MAX 4096
static void harness_pc_ring(uint32_t pc)
{
    static int inited = 0, armed = 0;
    static uint32_t trig = 0, depth = 0, d0_match = 0;
    static uint32_t ring[HARNESS_PC_RING_MAX];
    static uint16_t ring_sr[HARNESS_PC_RING_MAX];
    static uint32_t head = 0, filled = 0;

    if (!inited) {
        const char *env = getenv("HARNESS_PC_RING");
        inited = 1;
        if (env && env[0]) {
            unsigned long a = 0, b = 0, c = 0;
            if (sscanf(env, "%lx:%lu:%lx", &a, &b, &c) >= 2) {
                trig = (uint32_t)a & 0x00FFFFFFu;
                depth = (uint32_t)(b > HARNESS_PC_RING_MAX ? HARNESS_PC_RING_MAX : b);
                d0_match = (uint32_t)c;
                armed = (trig && depth) ? 1 : 0;
                if (armed)
                    fprintf(stderr, "[PC-RING] armed trig=0x%06x depth=%u d0=0x%08x\n",
                            trig, depth, d0_match);
            }
        }
    }

    if (!armed)
        return;

    ring[head] = pc & 0x00FFFFFFu;
    ring_sr[head] = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
    head = (head + 1) % depth;
    if (filled < depth) filled++;

    if ((pc & 0x00FFFFFFu) == trig &&
        (uint32_t)m68k_get_reg(NULL, M68K_REG_D0) == d0_match) {
        uint32_t i, idx = (head + depth - filled) % depth;
        fprintf(stderr, "[PC-RING] === dump (%u entries, oldest first) ===\n", filled);
        for (i = 0; i < filled; i++) {
            fprintf(stderr, "[PC-RING] pc=%06x sr=%04x\n", ring[idx], ring_sr[idx]);
            idx = (idx + 1) % depth;
        }
        fprintf(stderr, "[PC-RING] === end ===\n");
        armed = 0;
    }
}

static void harness_instr_hook(unsigned int pc)
{
    harness_pc_burst((uint32_t)pc);
    harness_pc_ring((uint32_t)pc);
    harness_trace_pc_range((uint32_t)pc);
    harness_trace_exec_call((uint32_t)pc);
    harness_trace_library_call((uint32_t)pc);

    if (!harness_boot_trace_enabled())
        return;

    harness_update_boot_display_block((uint32_t)pc);
    harness_probe_display_setup((uint32_t)pc);
    harness_probe_display_writer((uint32_t)pc);
    harness_probe_happy_builder();
    harness_probe_gfx_builder();
    harness_probe_gfx_calls((uint32_t)pc);
    harness_probe_boot_path((uint32_t)pc);
    harness_probe_vbl_dispatch_node((uint32_t)pc);
    harness_probe_exec_io((uint32_t)pc);
    harness_trace_post_dskblk_window((uint32_t)pc);
}

static uint32_t harness_read(uint32_t addr, int size)
{
    /* Zorro III / 32-bit space is unimplemented (ISSUE-0032) — must read
     * as open bus, not alias into 24-bit space. AROS's Z3 autoconfig
     * probe (0xFF000000+) was previously masking onto live chip RAM and
     * the RTG board's own DiagArea, reading mutable state instead of
     * "nothing here" and causing non-deterministic boot behavior across
     * runs (found 2026-07-03; same fix applied in cpu_bridge.c for the
     * non-harness backends). */
    if (addr > 0x00FFFFFFu) {
        static int s_hits = 0;
        if (s_hits < 20) {
            s_hits++;
            uint32_t pc0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
            printf("[Z3-OPENBUS] pc=%08x addr=%08x size=%d\n",
                   (unsigned)pc0, (unsigned)addr, size);
        }
        return (size == 1) ? 0xFFu : (size == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    }
    addr &= 0x00FFFFFFu;
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    uint32_t ret = 0;

    /* Standard ROM window (0xF80000 or 0xFC0000) */
    if (s_rom_std_size && addr >= s_rom_std_base &&
        addr < s_rom_std_base + s_rom_std_size) {
        ret = rom_read_at(s_rom_std_off + (addr - s_rom_std_base), size);
        harness_watch_rom_read(pc, addr, size, ret);
        return ret;
    }

    /* Extended ROM window (1 MB ROMs only) */
    if (s_rom_ext_size && addr >= s_rom_ext_base &&
        addr < s_rom_ext_base + s_rom_ext_size) {
        ret = rom_read_at(s_rom_ext_off + (addr - s_rom_ext_base), size);
        harness_watch_rom_read(pc, addr, size, ret);
        return ret;
    }

    /* Chip RAM window */
    if (bellatrix_chip_addr_contains(addr)) {
        /* Overlay: reads from low page redirect to standard ROM */
        if (harness_overlay() && addr < BELLATRIX_CHIP_BOOT_SIZE && s_rom_std_size) {
            uint32_t rom_off = s_rom_std_off + (addr & (s_rom_std_size - 1u));
            ret = rom_read_at(rom_off, size);
            harness_watch_rom_read(pc, s_rom_std_base + (addr & (s_rom_std_size - 1u)), size, ret);
            return ret;
        }
        harness_sync_cpu_progress();
        const BellatrixMemory *mem = &bellatrix_machine_get()->memory;
        if (size == 1) ret = bellatrix_chip_read8(mem, addr);
        else if (size == 2) ret = bellatrix_chip_read16(mem, addr);
        else if (size == 4) ret = bellatrix_chip_read32(mem, addr);
        harness_watch_rw("WATCH-BUS-R", pc, addr, size, ret);
        aros_gfxbase_lof_check(pc, addr, ret, 0);
        aros_bpl_dump_check(pc);
        return ret;
    }

    /* Zorro II fast RAM: respond only after autoconfig assigned the board
     * a base, so early memory probes do not find RAM before AddMemList.
     * Bounded to the configured window: other Zorro II boards (RTG) can
     * own part of the 0x200000-0x9FFFFF space. */
    {
        uint32_t fr_base, fr_size;
        if (bellatrix_zorro2_fast_ram_window(&fr_base, &fr_size) &&
            addr >= fr_base && addr < fr_base + fr_size) {
            const BellatrixMemory *fmem = &bellatrix_machine_get()->memory;
            if (size == 1) return bellatrix_fast_read8(fmem, addr);
            if (size == 2) return bellatrix_fast_read16(fmem, addr);
            return bellatrix_fast_read32(fmem, addr);
        }
    }

    if (bellatrix_slow_contains(&bellatrix_machine_get()->memory,
                                addr,
                                (unsigned int)size)) {
        harness_sync_cpu_progress();
        ret = bellatrix_bridge_cpu_read(addr, (unsigned int)size);
        harness_watch_rw("WATCH-BUS-R", pc, addr, size, ret);
        aros_gfxbase_lof_check(pc, addr, ret, 0);
        aros_bpl_dump_check(pc);
        return ret;
    }

    /* Chipset / CIA / RTC */
    harness_sync_cpu_progress();
    ret = bellatrix_bridge_cpu_read(addr, (unsigned int)size);
    harness_watch_rw("WATCH-BUS-R", pc, addr, size, ret);
    aros_loop_check(addr, ret);
    aros_wait2_check(addr, ret);
    return ret;
}

static void harness_write(uint32_t addr, uint32_t value, int size)
{
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

    /* Zorro III / 32-bit space open bus — see harness_read. */
    if (addr > 0x00FFFFFFu) {
        static int s_hits = 0;
        if (s_hits < 20) {
            s_hits++;
            printf("[Z3-OPENBUS] pc=%08x addr=%08x size=%d val=%08x (write, discarded)\n",
                   (unsigned)pc, (unsigned)addr, size, (unsigned)value);
        }
        return;
    }

    harness_mem_watch_write(pc, addr & 0x00FFFFFFu, value, size);

    if (bellatrix_slow_contains(&bellatrix_machine_get()->memory,
                                addr,
                                (unsigned int)size)) {
        harness_sync_cpu_progress();
        harness_watch_rw("WATCH-BUS-W", pc, addr, size, value);
        aros_gfxbase_lof_check(pc, addr, value, 1);
        bellatrix_bridge_cpu_write(addr, value, (unsigned int)size);
        return;
    }

    addr = bellatrix_bridge_normalize_addr(addr);

    /* ROM windows are read-only */
    if (s_rom_std_size && addr >= s_rom_std_base && addr < s_rom_std_base + s_rom_std_size) return;
    if (s_rom_ext_size && addr >= s_rom_ext_base && addr < s_rom_ext_base + s_rom_ext_size) return;

    /* Chip RAM */
    if (bellatrix_chip_addr_contains(addr)) {
        BellatrixMemory *mem = &bellatrix_machine_get()->memory;
        uint32_t wend = addr + (uint32_t)size - 1u;
        harness_sync_cpu_progress();
        harness_watch_rw("WATCH-BUS-W", pc, addr, size, value);
        aros_gfxbase_lof_check(pc, addr, value, 1);
        if (harness_boot_trace_enabled()) {
            harness_watch_boot_bitplane_write(pc, addr, size, value);
            harness_watch_boot_dynamic_buffer_write(pc, addr, size, value);
        }
        if (harness_boot_trace_enabled() &&
            harness_watch_boot_payload_addr(addr) && value != 0)
            harness_watch_rw("WATCH-BOOT-PAYLOAD-W", pc, addr, size, value);
        if (harness_watch_custom_range_addr(addr))
            harness_watch_rw("WATCH-BPL-RAM-W", pc, addr, size, value);
        if (s_low_mem_trace < 0) {
            const char *env = getenv("HARNESS_LOW_MEM_TRACE");
            s_low_mem_trace = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
        }
        if (s_low_mem_trace && addr <= 0x000001ffu) {
            printf("[LOW-MEM-W] pc=%08x addr=%06x size=%d val=%08x "
                   "v0=%08x v4=%08x v6c=%08x v128=%08x\n",
                   (unsigned)pc,
                   (unsigned)addr,
                   size,
                   (unsigned)value,
                   (unsigned)harness_chip_read(0x000000u, 4),
                   (unsigned)harness_chip_read(0x000004u, 4),
                   (unsigned)harness_chip_read(0x00006cu, 4),
                   (unsigned)harness_chip_read(0x000128u, 4));
        }
        if (size == 1) bellatrix_chip_write8 (mem, addr, (uint8_t)value);
        if (size == 2) bellatrix_chip_write16(mem, addr, (uint16_t)value);
        if (size == 4) bellatrix_chip_write32(mem, addr, value);
        /* Track writes that may touch bytes 0x1892..0x1895 */
        if (harness_boot_trace_enabled() && wend >= 0x1892u && addr <= 0x1895u) {
            uint32_t b92 = bellatrix_chip_read32(mem, 0x1892u);
            printf("[1892-WATCH] pc=%08x write addr=%06x size=%d val=%08x → [1892]=%08x\n",
                   (unsigned)pc, (unsigned)addr, size, (unsigned)value, (unsigned)b92);
        }
        return;
    }

    /* Zorro II fast RAM (see harness_read) */
    {
        uint32_t fr_base, fr_size;
        if (bellatrix_zorro2_fast_ram_window(&fr_base, &fr_size) &&
            addr >= fr_base && addr < fr_base + fr_size) {
            BellatrixMemory *fmem = &bellatrix_machine_get()->memory;
            if (size == 1)      bellatrix_fast_write8(fmem, addr, (uint8_t)value);
            else if (size == 2) bellatrix_fast_write16(fmem, addr, (uint16_t)value);
            else                bellatrix_fast_write32(fmem, addr, value);
            return;
        }
    }

    /* Chipset / CIA / RTC */
    harness_sync_cpu_progress();
    harness_watch_rw("WATCH-BUS-W", pc, addr, size, value);
    bellatrix_bridge_cpu_write(addr, value, (unsigned int)size);
    if (harness_boot_trace_enabled())
        harness_watch_dskblk_ack(pc, addr, size, value);
}

/* ---------------------------------------------------------------------------
 * Musashi memory callbacks
 * ------------------------------------------------------------------------- */

unsigned int m68k_read_memory_8(unsigned int address)
{
    return harness_read((uint32_t)address, 1);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    return harness_read((uint32_t)address, 2);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    return harness_read((uint32_t)address, 4);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    harness_write((uint32_t)address, value, 1);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    harness_write((uint32_t)address, value, 2);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    harness_write((uint32_t)address, value, 4);
}

/* Disassembler read (same as normal reads) */
unsigned int m68k_read_disassembler_8 (unsigned int a) { return m68k_read_memory_8(a);  }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ---------------------------------------------------------------------------
 * CpuBackend callbacks
 * ------------------------------------------------------------------------- */

static uint32_t musashi_get_pc(void *ctx)
{
    (void)ctx;
    return (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
}

static int harness_ipl_trace_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("HARNESS_IPL_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    return enabled;
}

static int harness_int_ack(int level)
{
    if (harness_ipl_trace_enabled())
        fprintf(stderr, "[IPL] int-ack level=%d pc=0x%06x sr=%04x\n", level,
                (unsigned)m68k_get_reg(NULL, M68K_REG_PC) & 0x00FFFFFF,
                (unsigned)m68k_get_reg(NULL, M68K_REG_SR));
    return M68K_INT_ACK_AUTOVECTOR;
}

static void musashi_set_ipl(void *ctx, int level)
{
    (void)ctx;
    if (harness_boot_trace_enabled() && level > 0)
        printf("[IPL] set_ipl level=%d  pc=0x%08x\n", level,
               (unsigned)m68k_get_reg(NULL, M68K_REG_PC));
    if (harness_ipl_trace_enabled()) {
        static int last_level = -1;
        if (level != last_level) {
            fprintf(stderr, "[IPL] set_ipl level=%d pc=0x%06x sr=%04x\n", level,
                    (unsigned)m68k_get_reg(NULL, M68K_REG_PC) & 0x00FFFFFF,
                    (unsigned)m68k_get_reg(NULL, M68K_REG_SR));
            last_level = level;
        }
    }
    m68k_set_irq((unsigned int)level);

    /* Musashi only services pending interrupts at the start of an execute
     * timeslice (or when an instruction writes SR).  An IPL raise arriving
     * mid-slice would otherwise wait until the slice ends — long enough for
     * the guest to Disable() and rescind it (lost preemption, see
     * ISSUE-0026).  End the slice so the IRQ is taken on the next
     * instruction boundary, as real hardware would. */
    if (level > 0)
        m68k_end_timeslice();
}

static void musashi_reset(void *ctx)
{
    (void)ctx;
    m68k_pulse_reset();
}

static int musashi_run(void *ctx, uint32_t cycles)
{
    int used;

    (void)ctx;

    s_run_sync_active = 1;
    s_run_sync_published = 0;
    used = m68k_execute((int)cycles);
    harness_sync_cpu_progress();
    s_run_sync_active = 0;

    return used;
}

static CpuBackend g_musashi_backend = {
    .ctx     = NULL,
    .get_pc  = musashi_get_pc,
    .set_ipl = musashi_set_ipl,
    .reset   = musashi_reset,
    .run     = musashi_run,
    .progress_in_run = 1,
};

CpuBackend *musashi_backend_get(void)
{
    return &g_musashi_backend;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void musashi_backend_init(void)
{
    const char *cpu = getenv("HARNESS_CPU");

    s_cpu_type = M68K_CPU_TYPE_68000;
    if (cpu && strcmp(cpu, "68010") == 0)
        s_cpu_type = M68K_CPU_TYPE_68010;
    else if (cpu && strcmp(cpu, "68ec020") == 0)
        s_cpu_type = M68K_CPU_TYPE_68EC020;
    else if (cpu && strcmp(cpu, "68020") == 0)
        s_cpu_type = M68K_CPU_TYPE_68020;
    else if (cpu && strcmp(cpu, "68030") == 0)
        s_cpu_type = M68K_CPU_TYPE_68030;
    else if (cpu && strcmp(cpu, "68040") == 0)
        s_cpu_type = M68K_CPU_TYPE_68040;

    m68k_init();
    m68k_set_cpu_type(s_cpu_type);
    printf("[HARNESS] CPU: %s\n",
           s_cpu_type == M68K_CPU_TYPE_68040 ? "68040" :
           s_cpu_type == M68K_CPU_TYPE_68030 ? "68030" :
           s_cpu_type == M68K_CPU_TYPE_68020 ? "68020" :
           s_cpu_type == M68K_CPU_TYPE_68EC020 ? "68ec020" :
           s_cpu_type == M68K_CPU_TYPE_68010 ? "68010" : "68000");
    m68k_set_instr_hook_callback(harness_instr_hook);
    if (harness_ipl_trace_enabled())
        m68k_set_int_ack_callback(harness_int_ack);
}
