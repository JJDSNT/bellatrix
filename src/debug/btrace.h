#ifndef BELLATRIX_DEBUG_BTRACE_H
#define BELLATRIX_DEBUG_BTRACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

#ifndef BTRACE_BUF_SIZE
#define BTRACE_BUF_SIZE 1024u
#endif

#if (BTRACE_BUF_SIZE & (BTRACE_BUF_SIZE - 1)) != 0
#error "BTRACE_BUF_SIZE must be a power of two"
#endif

#define BTRACE_BUF_MASK (BTRACE_BUF_SIZE - 1u)

/* ------------------------------------------------------------------------- */
/* Direction                                                                 */
/* ------------------------------------------------------------------------- */

typedef enum BTraceDir {
    BTRACE_READ  = 0,
    BTRACE_WRITE = 1
} BTraceDir;

/* ------------------------------------------------------------------------- */
/* Filter flags                                                              */
/* ------------------------------------------------------------------------- */

typedef enum BTraceFilter {
    BTRACE_F_OFF      = 0x0000,
    BTRACE_F_UNIMPL   = 0x0001,
    BTRACE_F_CIA      = 0x0002,
    BTRACE_F_CHIPSET  = 0x0004,
    BTRACE_F_ALL      = 0xFFFF
} BTraceFilter;

/* ------------------------------------------------------------------------- */
/* Entry                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct BTraceEntry
{
    uint32_t tick_lo;   /* lower 32 bits of machine tick_count */
    uint32_t pc;        /* M68K PC at time of access */

    uint32_t addr;      /* bus address */
    uint32_t value;     /* read/write value */

    uint8_t  size;      /* 1, 2, 4 */
    uint8_t  dir;       /* BTRACE_READ / BTRACE_WRITE */
    uint8_t  impl;      /* 1 if handled by component, 0 if unimplemented */
    uint8_t  reserved;
} BTraceEntry;

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct BTraceState
{
    uint32_t head;
    bool     paused;

    uint16_t filter;

    BTraceEntry buf[BTRACE_BUF_SIZE];
} BTraceState;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void btrace_init(BTraceState *t);
void btrace_reset(BTraceState *t);

void btrace_pause(BTraceState *t);
void btrace_resume(BTraceState *t);

/* ------------------------------------------------------------------------- */
/* Filter                                                                    */
/* ------------------------------------------------------------------------- */

void     btrace_set_filter(BTraceState *t, uint16_t filter);
uint16_t btrace_get_filter(const BTraceState *t);

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

void btrace_log(BTraceState *t,
                uint32_t tick_lo,
                uint32_t pc,
                uint32_t addr,
                uint32_t value,
                unsigned int size,
                uint8_t dir,
                uint8_t impl);

void btrace_log_read(BTraceState *t,
                     uint32_t tick_lo,
                     uint32_t pc,
                     uint32_t addr,
                     unsigned int size,
                     uint32_t value);

void btrace_log_write(BTraceState *t,
                      uint32_t tick_lo,
                      uint32_t pc,
                      uint32_t addr,
                      unsigned int size,
                      uint32_t value);

void btrace_log_unimpl(BTraceState *t,
                       uint32_t tick_lo,
                       uint32_t pc,
                       uint32_t addr,
                       unsigned int size,
                       uint8_t dir,
                       uint32_t value);

/* ------------------------------------------------------------------------- */
/* Dump                                                                      */
/* ------------------------------------------------------------------------- */

void btrace_dump(const BTraceState *t, uint32_t last_n);

const char *btrace_dir_name(uint8_t dir);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_DEBUG_BTRACE_H */