// src/core/btrace.c
//
// Bus trace — logs every chipset/CIA access as JSON Lines via
// PAL_Debug_Print. Output is consumed by tools/btrace/btrace.py
// on the host machine.
//
// JSON event format (one line per access):
//   {"t":"btrace","tick":N,"m68k_pc":"0xXXXXXX","addr":"0xXXXXXX",
//    "dir":"R|W","size":N,"val":"0xXXXX","impl":true|false}

#include "btrace.h"
#include "host/pal.h"
#include "M68k.h"

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static uint16_t s_filter = BTRACE_UNIMPL; // default: log unimplemented only

// Ring buffer for watchdog post-mortem.
typedef struct {
    uint32_t addr;
    uint32_t value;
    uint8_t  size;
    uint8_t  dir;
    uint8_t  impl;
} BTraceEntry;

static BTraceEntry s_ring[BTRACE_RING_SIZE];
static int         s_ring_head = 0;

// Watchdog: auto-dump after 250 VBLs (5 s at 50 Hz) with no chipset access.
#define WATCHDOG_VBL_LIMIT 250
static int s_watchdog = WATCHDOG_VBL_LIMIT;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t read_tick(void)
{
    uint64_t t;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(t));
    return t;
}

static uint32_t read_m68k_pc(void)
{
    struct M68KState *ctx;
    asm volatile("mrs %0, TPIDRRO_EL0" : "=r"(ctx));
    if (!ctx) return 0;
    // The whole build is -mbig-endian: BE32 is a no-op, ctx->PC is the
    // natural M68K address. No byte-swap needed.
    return ctx->PC;
}

// Minimal hex formatter — avoids printf/sprintf (not available bare-metal).
static void put_hex32(uint32_t v)
{
    char buf[11];  // "0x" + 8 hex digits + NUL
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 9; i >= 2; i--) {
        int nibble = v & 0xF;
        buf[i] = (nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10);
        v >>= 4;
    }
    buf[10] = '\0';
    PAL_Debug_Print(buf);
}

static void put_dec64(uint64_t v)
{
    char buf[21];
    int i = 20;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    while (v) { buf[--i] = '0' + (v % 10); v /= 10; }
    PAL_Debug_Print(&buf[i]);
}

static void put_dec(int v)
{
    put_dec64((uint64_t)(unsigned)v);
}

// Emit one JSON btrace event.
static void emit_event(uint32_t addr, uint32_t value, int size,
                       int dir, int impl, uint64_t tick, uint32_t pc)
{
    PAL_Debug_Print("{\"t\":\"btrace\",\"tick\":");
    put_dec64(tick);
    PAL_Debug_Print(",\"m68k_pc\":\"");
    put_hex32(pc);
    PAL_Debug_Print("\",\"addr\":\"");
    put_hex32(addr);
    PAL_Debug_Print("\",\"dir\":\"");
    PAL_Debug_PutC(dir == BUS_READ ? 'R' : 'W');
    PAL_Debug_Print("\",\"size\":");
    put_dec(size);
    PAL_Debug_Print(",\"val\":\"");
    put_hex32(value);
    PAL_Debug_Print("\",\"impl\":");
    PAL_Debug_Print(impl ? "true" : "false");
    PAL_Debug_Print("}\n");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void btrace_init(void)
{
    s_filter   = BTRACE_ALL;
    s_ring_head = 0;
    s_watchdog  = WATCHDOG_VBL_LIMIT;
}

void btrace_set_filter(uint16_t filter)
{
    s_filter = filter;
}

void btrace_log(uint32_t addr, uint32_t value, int size, int dir, int impl)
{
    uint64_t tick = read_tick();
    uint32_t pc   = read_m68k_pc();

    // Reset watchdog on any chipset access.
    s_watchdog = WATCHDOG_VBL_LIMIT;

    // Store in ring buffer regardless of filter.
    BTraceEntry *e = &s_ring[s_ring_head % BTRACE_RING_SIZE];
    e->addr  = addr;
    e->value = value;
    e->size  = (uint8_t)size;
    e->dir   = (uint8_t)dir;
    e->impl  = (uint8_t)impl;
    s_ring_head++;

    // Apply verbosity filter.
    if (s_filter == BTRACE_OFF)
        return;
    if (!(s_filter & BTRACE_ALL))
        return;
    if (!impl && !(s_filter & BTRACE_UNIMPL))
        return;
    if ((addr == 0xBFE001 || addr == 0xBFD000) && !(s_filter & BTRACE_CIA))
        return;
    if (addr >= 0xDFF000 && addr <= 0xDFF1FF && !(s_filter & BTRACE_CHIPSET))
        return;

    emit_event(addr, value, size, dir, impl, tick, pc);
}

void btrace_dump_ring(void)
{
    PAL_Debug_Print("{\"t\":\"watchdog\",\"reason\":\"no_chipset_access_5s\","
                    "\"recent_accesses\":[\n");
    int count = s_ring_head < BTRACE_RING_SIZE ? s_ring_head : BTRACE_RING_SIZE;
    int start = (s_ring_head - count + BTRACE_RING_SIZE) % BTRACE_RING_SIZE;
    for (int i = 0; i < count; i++) {
        BTraceEntry *e = &s_ring[(start + i) % BTRACE_RING_SIZE];
        PAL_Debug_Print("{\"addr\":\"");
        put_hex32(e->addr);
        PAL_Debug_Print("\",\"dir\":\"");
        PAL_Debug_PutC(e->dir == BUS_READ ? 'R' : 'W');
        PAL_Debug_Print("\",\"val\":\"");
        put_hex32(e->value);
        PAL_Debug_Print("\"}");
        if (i < count - 1) PAL_Debug_PutC(',');
        PAL_Debug_PutC('\n');
    }
    PAL_Debug_Print("]}\n");
}

void btrace_watchdog_tick(void)
{
    if (s_watchdog > 0) {
        s_watchdog--;
        if (s_watchdog == 0)
            btrace_dump_ring();
    }
}
