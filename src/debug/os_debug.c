#include "debug/os_debug.h"

#include <stdint.h>

#include "core/machine.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* Big-endian RAM access                                                     */
/* ------------------------------------------------------------------------- */

static uint8_t ram8(BellatrixMachine *m, uint32_t a)
{
    if (!m || !m->memory.chip_ram || a >= m->memory.chip_ram_size) {
        return 0xFF;
    }
    return m->memory.chip_ram[a];
}

static uint16_t ram16(BellatrixMachine *m, uint32_t a)
{
    return ((uint16_t)ram8(m, a) << 8) | ram8(m, a + 1);
}

static uint32_t ram32(BellatrixMachine *m, uint32_t a)
{
    return ((uint32_t)ram8(m, a) << 24) |
           ((uint32_t)ram8(m, a + 1) << 16) |
           ((uint32_t)ram8(m, a + 2) << 8) |
           (uint32_t)ram8(m, a + 3);
}

static int is_ram_ptr(BellatrixMachine *m, uint32_t a)
{
    return m && a >= 0x400 && a < m->memory.chip_ram_size;
}

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static const char *task_state_str(uint8_t s)
{
    switch (s) {
        case 1: return "ADDED";
        case 2: return "RUN";
        case 3: return "READY";
        case 4: return "WAIT";
        case 5: return "EXCEPT";
        case 6: return "REMOVED";
        default: return "?";
    }
}

static void read_name(BellatrixMachine *m, uint32_t ptr, char out[32])
{
    int i;

    out[0] = '\0';

    if (!is_ram_ptr(m, ptr)) {
        return;
    }

    for (i = 0; i < 31; i++) {
        char c = (char)ram8(m, ptr + (uint32_t)i);
        if (!c) {
            break;
        }
        out[i] = c;
        out[i + 1] = '\0';
    }
}

/* ------------------------------------------------------------------------- */
/* Exec offsets                                                              */
/* ------------------------------------------------------------------------- */

/* Node */
#define NODE_SUCC(a)    ram32(m, (a) + 0)
#define NODE_TYPE(a)    ram8 (m, (a) + 8)
#define NODE_NAME(a)    ram32(m, (a) + 10)

/* List */
#define LIST_HEAD(a)    ram32(m, (a) + 0)

/* Task */
#define TASK_STATE(a)   ram8 (m, (a) + 15)
#define TASK_IDNEST(a)  ((int8_t)ram8(m, (a) + 16))
#define TASK_TDNEST(a)  ((int8_t)ram8(m, (a) + 17))

/* Library */
#define LIB_VERSION(a)  ram16(m, (a) + 20)
#define LIB_OPENCNT(a)  ram16(m, (a) + 32)

/* ExecBase */
#define EXEC_VERSION(a)   ram16(m, (a) + 20)
#define EXEC_IDNEST(a)    ((int8_t)ram8(m, (a) + 294))
#define EXEC_TDNEST(a)    ((int8_t)ram8(m, (a) + 295))
#define EXEC_ATTNFLAGS(a) ram16(m, (a) + 296)
#define EXEC_THISTASK(a)  ram32(m, (a) + 276)
#define EXEC_LIBLIST(a)   ((a) + 378)
#define EXEC_TASKREADY(a) ((a) + 406)
#define EXEC_TASKWAIT(a)  ((a) + 420)

#define NT_TASK      1
#define NT_LIBRARY   9
#define NT_PROCESS  13

/* ------------------------------------------------------------------------- */

static void dump_task(BellatrixMachine *m, uint32_t addr, const char *prefix)
{
    char name[32];
    uint8_t state;
    int8_t idnest;
    int8_t tdnest;
    uint8_t nt;

    read_name(m, NODE_NAME(addr), name);
    state = TASK_STATE(addr);
    idnest = TASK_IDNEST(addr);
    tdnest = TASK_TDNEST(addr);
    nt = NODE_TYPE(addr);

    kprintf("[OSDBG] %s@%08x \"%s\" %s state=%s IDN=%d TDN=%d\n",
            prefix,
            addr,
            name,
            nt == NT_PROCESS ? "Process" : "Task",
            task_state_str(state),
            idnest,
            tdnest);
}

static void dump_library(BellatrixMachine *m, uint32_t addr)
{
    char name[32];
    uint16_t ver;
    uint16_t cnt;

    read_name(m, NODE_NAME(addr), name);
    ver = LIB_VERSION(addr);
    cnt = LIB_OPENCNT(addr);

    kprintf("[OSDBG]   lib @%08x \"%s\" v%u.%u open=%u\n",
            addr,
            name,
            (unsigned)(ver >> 8),
            (unsigned)(ver & 0xFF),
            (unsigned)cnt);
}

static void dump_list(BellatrixMachine *m, uint32_t list_addr, const char *label, int is_lib)
{
    uint32_t node;
    int count = 0;

    kprintf("[OSDBG] %s\n", label);

    node = LIST_HEAD(list_addr);
    while (is_ram_ptr(m, node) && count < 32) {
        if (is_lib) {
            dump_library(m, node);
        } else {
            dump_task(m, node, "  task ");
        }
        node = NODE_SUCC(node);
        count++;
    }

    if (count == 0) {
        kprintf("[OSDBG]   (empty)\n");
    }
}

void os_debug_dump(BellatrixMachine *m)
{
    uint32_t eb_ptr;

    kprintf("[OSDBG] ---- OS state ----\n");

    eb_ptr = ram32(m, 4);
    if (!is_ram_ptr(m, eb_ptr)) {
        kprintf("[OSDBG] ExecBase invalid: %08x\n", eb_ptr);
        return;
    }

    kprintf("[OSDBG] ExecBase @%08x exec v%u.%u\n",
            eb_ptr,
            (unsigned)(EXEC_VERSION(eb_ptr) >> 8),
            (unsigned)(EXEC_VERSION(eb_ptr) & 0xFF));

    kprintf("[OSDBG] IDNestCnt=%d TDNestCnt=%d AttnFlags=%04x\n",
            EXEC_IDNEST(eb_ptr),
            EXEC_TDNEST(eb_ptr),
            EXEC_ATTNFLAGS(eb_ptr));

    {
        uint32_t tt = EXEC_THISTASK(eb_ptr);
        if (is_ram_ptr(m, tt)) {
            dump_task(m, tt, "ThisTask ");
        } else {
            kprintf("[OSDBG] ThisTask: none\n");
        }
    }

    dump_list(m, EXEC_LIBLIST(eb_ptr), "LibList:", 1);
    dump_list(m, EXEC_TASKREADY(eb_ptr), "TaskReady:", 0);
    dump_list(m, EXEC_TASKWAIT(eb_ptr), "TaskWait:", 0);

    kprintf("[OSDBG] ---- end ----\n");
}