// src/debug/btrace.c
//
// Bus trace — instance-based ring buffer with JSON Lines output.
// Each BTraceState is self-contained; the machine owns one instance.

#include "btrace.h"
#include "host/pal.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Serial emit helpers (no printf on bare metal)                             */
/* ------------------------------------------------------------------------- */

static void put_hex32(uint32_t v)
{
    char buf[11];
    int i;

    buf[0] = '0';
    buf[1] = 'x';

    for (i = 9; i >= 2; i--) {
        uint32_t nibble = v & 0xFu;
        buf[i] = (char)((nibble < 10u) ? ('0' + nibble) : ('a' + nibble - 10u));
        v >>= 4;
    }

    buf[10] = '\0';
    PAL_Debug_Print(buf);
}

static void put_dec32(uint32_t v)
{
    char buf[11];
    int i = 10;

    buf[i] = '\0';

    if (v == 0) {
        buf[--i] = '0';
    }

    while (v) {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    PAL_Debug_Print(&buf[i]);
}

static void emit_json(uint32_t tick_lo, uint32_t pc,
                      uint32_t addr, uint32_t value,
                      unsigned int size, uint8_t dir, uint8_t impl)
{
    PAL_Debug_Print("{\"t\":\"btrace\",\"tick\":");
    put_dec32(tick_lo);

    PAL_Debug_Print(",\"m68k_pc\":\"");
    put_hex32(pc);

    PAL_Debug_Print("\",\"addr\":\"");
    put_hex32(addr);

    PAL_Debug_Print("\",\"dir\":\"");
    PAL_Debug_PutC((dir == BTRACE_WRITE) ? 'W' : 'R');

    PAL_Debug_Print("\",\"size\":");
    put_dec32((uint32_t)size);

    PAL_Debug_Print(",\"val\":\"");
    put_hex32(value);

    PAL_Debug_Print("\",\"impl\":");
    PAL_Debug_Print(impl ? "true" : "false");

    PAL_Debug_Print("}\n");
}

/* ------------------------------------------------------------------------- */
/* Filter helpers                                                             */
/* ------------------------------------------------------------------------- */

static int addr_is_cia(uint32_t addr)
{
    return ((addr >= 0x00BFD000u && addr <= 0x00BFDFFFu) ||
            (addr >= 0x00BFE001u && addr <= 0x00BFEFFFu));
}

static int addr_is_chipset(uint32_t addr)
{
    return (addr >= 0x00DFF000u && addr <= 0x00DFF1FFu);
}

static int should_emit(const BTraceState *t, uint32_t addr, uint8_t impl)
{
    if (t->paused || t->filter == BTRACE_F_OFF)
        return 0;

    if (!impl)
        return !!(t->filter & BTRACE_F_UNIMPL);

    if (addr_is_cia(addr))
        return !!(t->filter & BTRACE_F_CIA);

    if (addr_is_chipset(addr))
        return !!(t->filter & BTRACE_F_CHIPSET);

    /* For everything else (chip RAM, ROM, etc.) only log if ALL is set. */
    return (t->filter == BTRACE_F_ALL);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

void btrace_init(BTraceState *t)
{
    memset(t, 0, sizeof(*t));
    t->filter = BTRACE_F_UNIMPL;
}

void btrace_reset(BTraceState *t)
{
    t->head   = 0;
    t->paused = false;
    memset(t->buf, 0, sizeof(t->buf));
}

void btrace_pause(BTraceState *t)  { t->paused = true;  }
void btrace_resume(BTraceState *t) { t->paused = false; }

/* ------------------------------------------------------------------------- */
/* Filter                                                                     */
/* ------------------------------------------------------------------------- */

void btrace_set_filter(BTraceState *t, uint16_t filter)
{
    t->filter = filter;
}

uint16_t btrace_get_filter(const BTraceState *t)
{
    return t->filter;
}

/* ------------------------------------------------------------------------- */
/* Logging                                                                    */
/* ------------------------------------------------------------------------- */

void btrace_log(BTraceState *t,
                uint32_t tick_lo,
                uint32_t pc,
                uint32_t addr,
                uint32_t value,
                unsigned int size,
                uint8_t dir,
                uint8_t impl)
{
    BTraceEntry *e;

    if (!t)
        return;

    /* Always store in ring buffer for post-mortem, regardless of filter. */
    e = &t->buf[t->head & BTRACE_BUF_MASK];
    e->tick_lo  = tick_lo;
    e->pc       = pc;
    e->addr     = addr;
    e->value    = value;
    e->size     = (uint8_t)size;
    e->dir      = dir;
    e->impl     = impl;
    e->reserved = 0;
    t->head++;

    if (should_emit(t, addr, impl))
        emit_json(tick_lo, pc, addr, value, size, dir, impl);
}

void btrace_log_read(BTraceState *t,
                     uint32_t tick_lo,
                     uint32_t pc,
                     uint32_t addr,
                     unsigned int size,
                     uint32_t value)
{
    btrace_log(t, tick_lo, pc, addr, value, size, BTRACE_READ, 1);
}

void btrace_log_write(BTraceState *t,
                      uint32_t tick_lo,
                      uint32_t pc,
                      uint32_t addr,
                      unsigned int size,
                      uint32_t value)
{
    btrace_log(t, tick_lo, pc, addr, value, size, BTRACE_WRITE, 1);
}

void btrace_log_unimpl(BTraceState *t,
                       uint32_t tick_lo,
                       uint32_t pc,
                       uint32_t addr,
                       unsigned int size,
                       uint8_t dir,
                       uint32_t value)
{
    btrace_log(t, tick_lo, pc, addr, value, size, dir, 0);
}

/* ------------------------------------------------------------------------- */
/* Dump                                                                       */
/* ------------------------------------------------------------------------- */

void btrace_dump(const BTraceState *t, uint32_t last_n)
{
    uint32_t count, start, i;

    if (!t || last_n == 0)
        return;

    count = (t->head < BTRACE_BUF_SIZE) ? t->head : (uint32_t)BTRACE_BUF_SIZE;
    if (last_n < count)
        count = last_n;

    start = (t->head >= count) ? (t->head - count) : 0;

    PAL_Debug_Print("{\"t\":\"btrace_dump\",\"entries\":[\n");

    for (i = 0; i < count; i++) {
        const BTraceEntry *e = &t->buf[(start + i) & BTRACE_BUF_MASK];
        emit_json(e->tick_lo, e->pc, e->addr, e->value,
                  e->size, e->dir, e->impl);
    }

    PAL_Debug_Print("]}\n");
}

const char *btrace_dir_name(uint8_t dir)
{
    return (dir == BTRACE_WRITE) ? "W" : "R";
}
