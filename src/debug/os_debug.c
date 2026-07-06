#include "debug/os_debug.h"

#include <stdint.h>
#include <string.h>

#include "machine/machine.h"
#include "machine/memory/memory.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* Big-endian RAM access                                                     */
/* ------------------------------------------------------------------------- */

static uint8_t ram8(BellatrixMachine *m, uint32_t a)
{
    if (!m) {
        return 0xFF;
    }
    return bellatrix_mem_read8(&m->memory, a);
}

static uint16_t ram16(BellatrixMachine *m, uint32_t a)
{
    if (!m) {
        return 0xFFFFu;
    }
    return bellatrix_mem_read16(&m->memory, a);
}

static uint32_t ram32(BellatrixMachine *m, uint32_t a)
{
    if (!m) {
        return 0xFFFFFFFFu;
    }
    return bellatrix_mem_read32(&m->memory, a);
}

static int is_ram_ptr(BellatrixMachine *m, uint32_t a)
{
    if (!m || a < 0x400u) {
        return 0;
    }
    if (bellatrix_chip_addr_contains(a)) {
        return 1;
    }
#if defined(BELLATRIX_HARNESS)
    /* AROS relocates ExecBase and most OS structures into Z2 fast RAM. */
    if (m->memory.fast_ram &&
        a >= BELLATRIX_FAST_RAM_BASE && a <= BELLATRIX_FAST_RAM_END) {
        return 1;
    }
#endif
    return bellatrix_slow_contains(&m->memory, a, 1u);
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
    ptr &= 0x00FFFFFFu;

    if (!m || ptr < 0x400u) {
        return;
    }

    for (i = 0; i < 31; i++) {
        char c = (char)ram8(m, ptr + (uint32_t)i);
        if (!c) {
            break;
        }
        if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7eu) {
            c = '?';
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
#define NODE_PRED(a)    ram32(m, (a) + 4)
#define NODE_TYPE(a)    ram8 (m, (a) + 8)
#define NODE_PRI(a)     ((int8_t)ram8(m, (a) + 9))
#define NODE_NAME(a)    ram32(m, (a) + 10)

/* List */
#define LIST_HEAD(a)    ram32(m, (a) + 0)

/* Task */
#define TASK_STATE(a)   ram8 (m, (a) + 15)
#define TASK_IDNEST(a)  ((int8_t)ram8(m, (a) + 16))
#define TASK_TDNEST(a)  ((int8_t)ram8(m, (a) + 17))
#define TASK_SIGWAIT(a) ram32(m, (a) + 22)
#define TASK_SIGRECVD(a) ram32(m, (a) + 26)
#define TASK_FLAGS(a)   ram8 (m, (a) + 14)
#define TASK_ETASK(a)   ram32(m, (a) + 34)
#define TASK_SPREG(a)   ram32(m, (a) + 54)
#define TASK_SPLOWER(a) ram32(m, (a) + 58)
#define TASK_SPUPPER(a) ram32(m, (a) + 62)

#define TASK_STATE_OFF 15u
#define TF_ETASK       (1u << 3)

/* m68k ExceptionContext: d[8] (32), a[8] (32), sr (2), pc (4). */
#define M68K_CTX_A7_OFF 60u
#define M68K_CTX_PC_OFF 66u

/* Non-SMP m68k ETask layout from exec/tasks.h. The SMP MsgPort spinlock layout
 * is larger; keep it as a fallback because the ROM build flags are external. */
#define ETASK_REGFRAME_OFF_M68K 98u
#define ETASK_REGFRAME_OFF_SMP  320u

/* Library */
#define LIB_VERSION(a)  ram16(m, (a) + 20)
#define LIB_OPENCNT(a)  ram16(m, (a) + 32)

/* Intuition/graphics public structures, 32-bit m68k AROS layout. */
#define INTUITION_VIEWLORD_OFF     34u
#define VIEW_SIZE                  18u
#define INTUITION_ACTIVEWINDOW(a)  ram32(m, (a) + INTUITION_VIEWLORD_OFF + VIEW_SIZE + 0u)
#define INTUITION_ACTIVESCREEN(a)  ram32(m, (a) + INTUITION_VIEWLORD_OFF + VIEW_SIZE + 4u)
#define INTUITION_FIRSTSCREEN(a)   ram32(m, (a) + INTUITION_VIEWLORD_OFF + VIEW_SIZE + 8u)

#define SCREEN_NEXT(a)        ram32(m, (a) + 0u)
#define SCREEN_FIRSTWINDOW(a) ram32(m, (a) + 4u)
#define SCREEN_LEFT(a)        ((int16_t)ram16(m, (a) + 8u))
#define SCREEN_TOP(a)         ((int16_t)ram16(m, (a) + 10u))
#define SCREEN_WIDTH(a)       ((int16_t)ram16(m, (a) + 12u))
#define SCREEN_HEIGHT(a)      ((int16_t)ram16(m, (a) + 14u))
#define SCREEN_FLAGS(a)       ram16(m, (a) + 20u)
#define SCREEN_TITLE(a)       ram32(m, (a) + 22u)
#define SCREEN_VIEWPORT(a)    ((a) + 44u)
#define SCREEN_RASTPORT(a)    ((a) + 84u)
#define SCREEN_BITMAP(a)      ((a) + 186u)

#define RASTPORT_LAYER(a)     ram32(m, (a) + 0u)
#define RASTPORT_BITMAP(a)    ram32(m, (a) + 4u)

#define BITMAP_BPR(a)         ram16(m, (a) + 0u)
#define BITMAP_ROWS(a)        ram16(m, (a) + 2u)
#define BITMAP_FLAGS(a)       ram8 (m, (a) + 4u)
#define BITMAP_DEPTH(a)       ram8 (m, (a) + 5u)
#define BITMAP_PLANE(a, p)    ram32(m, (a) + 8u + ((uint32_t)(p) * 4u))

#define WINDOW_NEXT(a)         ram32(m, (a) + 0u)
#define WINDOW_LEFT(a)         ((int16_t)ram16(m, (a) + 4u))
#define WINDOW_TOP(a)          ((int16_t)ram16(m, (a) + 6u))
#define WINDOW_WIDTH(a)        ((int16_t)ram16(m, (a) + 8u))
#define WINDOW_HEIGHT(a)       ((int16_t)ram16(m, (a) + 10u))
#define WINDOW_FLAGS(a)        ram32(m, (a) + 24u)
#define WINDOW_TITLE(a)        ram32(m, (a) + 32u)
#define WINDOW_FIRSTREQUEST(a) ram32(m, (a) + 36u)
#define WINDOW_REQCOUNT(a)     ((int16_t)ram16(m, (a) + 44u))
#define WINDOW_WSCREEN(a)      ram32(m, (a) + 46u)
#define WINDOW_RPORT(a)        ram32(m, (a) + 50u)
#define WINDOW_USERPORT(a)     ram32(m, (a) + 86u)
#define WINDOW_WINDOWPORT(a)   ram32(m, (a) + 90u)
#define WINDOW_WLAYER(a)       ram32(m, (a) + 124u)

#define LAYER_FRONT(a)         ram32(m, (a) + 0u)
#define LAYER_BACK(a)          ram32(m, (a) + 4u)
#define LAYER_CLIPRECT(a)      ram32(m, (a) + 8u)
#define LAYER_RP(a)            ram32(m, (a) + 12u)
#define LAYER_MINX(a)          ((int16_t)ram16(m, (a) + 16u))
#define LAYER_MINY(a)          ((int16_t)ram16(m, (a) + 18u))
#define LAYER_MAXX(a)          ((int16_t)ram16(m, (a) + 20u))
#define LAYER_MAXY(a)          ((int16_t)ram16(m, (a) + 22u))
#define LAYER_FLAGS(a)         ram16(m, (a) + 30u)
#define LAYER_SUPERBM(a)       ram32(m, (a) + 32u)
#define LAYER_WINDOW(a)        ram32(m, (a) + 40u)

#define CLIPRECT_NEXT(a)       ram32(m, (a) + 0u)
#define CLIPRECT_LAYER(a)      ram32(m, (a) + 8u)
#define CLIPRECT_BITMAP(a)     ram32(m, (a) + 12u)
#define CLIPRECT_MINX(a)       ((int16_t)ram16(m, (a) + 16u))
#define CLIPRECT_MINY(a)       ((int16_t)ram16(m, (a) + 18u))
#define CLIPRECT_MAXX(a)       ((int16_t)ram16(m, (a) + 20u))
#define CLIPRECT_MAXY(a)       ((int16_t)ram16(m, (a) + 22u))

/* DOS offsets, m68k AROS BPTR/BSTR layout. */
#define BPTR_ADDR(x)    (((x) << 2) & 0x00FFFFFFu)
#define DOS_DL_ROOT(a)  (ram32(m, (a) + 34) & 0x00FFFFFFu)
#define ROOT_RN_INFO(a) BPTR_ADDR(ram32(m, (a) + 24))
#define DOSINFO_RESLIST(a) BPTR_ADDR(ram32(m, (a) + 16))
#define SEG_NEXT(a)     BPTR_ADDR(ram32(m, (a) + 0))
#define SEG_UC(a)       ((int32_t)ram32(m, (a) + 4))
#define SEG_SEG(a)      BPTR_ADDR(ram32(m, (a) + 8))
#define SEG_BSTR(a)     ((a) + 13)

/* Process offset on 32-bit m68k AROS. pr_MsgPort is observed at +0x5c;
 * pr_WindowPtr follows the fixed 32-bit DOS fields at +0xb8. */
#define PROCESS_WINDOWPTR(a) ((a) + 0xb8u)

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
#define TS_READY     3
#define TS_WAIT      4

/* ------------------------------------------------------------------------- */

static int is_code_ptr(uint32_t pc)
{
    pc &= 0x00FFFFFFu;
    return (pc >= 0x00e00000u && pc <= 0x00ffffffu) ||
           (pc >= 0x00000400u && pc <= 0x00d7ffffu);
}

static int read_saved_context(BellatrixMachine *m, uint32_t etask,
                              uint32_t *regframe, uint32_t *savedpc,
                              uint32_t *savedsp)
{
    static const uint32_t offsets[] = {
        ETASK_REGFRAME_OFF_M68K,
        ETASK_REGFRAME_OFF_SMP
    };
    unsigned i;

    if (regframe) {
        *regframe = 0;
    }
    if (savedpc) {
        *savedpc = 0;
    }
    if (savedsp) {
        *savedsp = 0;
    }

    if (!is_ram_ptr(m, etask)) {
        return 0;
    }

    for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint32_t frame = ram32(m, etask + offsets[i]) & 0x00FFFFFFu;
        uint32_t pc;
        uint32_t sp;

        if (!is_ram_ptr(m, frame)) {
            continue;
        }

        pc = ram32(m, frame + M68K_CTX_PC_OFF) & 0x00FFFFFFu;
        sp = ram32(m, frame + M68K_CTX_A7_OFF) & 0x00FFFFFFu;
        if (!is_code_ptr(pc)) {
            continue;
        }

        if (regframe) {
            *regframe = frame;
        }
        if (savedpc) {
            *savedpc = pc;
        }
        if (savedsp) {
            *savedsp = sp;
        }
        return 1;
    }

    return 0;
}

static void ramw8(BellatrixMachine *m, uint32_t a, uint8_t v)
{
    if (m) {
        bellatrix_mem_write8(&m->memory, a, v);
    }
}

static void ramw32(BellatrixMachine *m, uint32_t a, uint32_t v)
{
    if (m) {
        bellatrix_mem_write32(&m->memory, a, v);
    }
}

static void remove_node(BellatrixMachine *m, uint32_t node)
{
    uint32_t succ = NODE_SUCC(node);
    uint32_t pred = NODE_PRED(node);

    if (!is_ram_ptr(m, succ) || !is_ram_ptr(m, pred)) {
        return;
    }

    ramw32(m, pred + 0u, succ);
    ramw32(m, succ + 4u, pred);
}

static void enqueue_task(BellatrixMachine *m, uint32_t list, uint32_t node)
{
    uint32_t next;
    int8_t pri = NODE_PRI(node);

    for (next = LIST_HEAD(list);
         is_ram_ptr(m, next) && next != list + 4u && pri <= NODE_PRI(next);
         next = NODE_SUCC(next)) {
    }

    if (!is_ram_ptr(m, next)) {
        return;
    }

    ramw32(m, node + 0u, next);
    ramw32(m, node + 4u, NODE_PRED(next));
    ramw32(m, NODE_PRED(next) + 0u, node);
    ramw32(m, next + 4u, node);
}

static void dump_task(BellatrixMachine *m, uint32_t addr, const char *prefix)
{
    char name[32];
    uint8_t state;
    uint8_t flags;
    int8_t idnest;
    int8_t tdnest;
    uint8_t nt;
    uint32_t sigwait;
    uint32_t sigrecvd;
    uint32_t etask;
    uint32_t regframe = 0;
    uint32_t savedpc = 0;
    uint32_t savedsp = 0;
    uint32_t spreg;
    uint32_t splower;
    uint32_t spupper;

    read_name(m, NODE_NAME(addr), name);
    state = TASK_STATE(addr);
    flags = TASK_FLAGS(addr);
    idnest = TASK_IDNEST(addr);
    tdnest = TASK_TDNEST(addr);
    nt = NODE_TYPE(addr);
    sigwait = TASK_SIGWAIT(addr);
    sigrecvd = TASK_SIGRECVD(addr);
    etask = (flags & TF_ETASK) ? TASK_ETASK(addr) : 0;
    spreg = TASK_SPREG(addr) & 0x00FFFFFFu;
    splower = TASK_SPLOWER(addr) & 0x00FFFFFFu;
    spupper = TASK_SPUPPER(addr) & 0x00FFFFFFu;
    read_saved_context(m, etask, &regframe, &savedpc, &savedsp);

    kprintf("[OSDBG] %s@%08x \"%s\" %s state=%s flags=%02x IDN=%d TDN=%d wait=%08x recvd=%08x etask=%08x frame=%08x pc=%08x ctxsp=%08x sp=%08x stack=%08x-%08x",
            prefix,
            addr,
            name,
            nt == NT_PROCESS ? "Process" : "Task",
            task_state_str(state),
            (unsigned)flags,
            idnest,
            tdnest,
            (unsigned)sigwait,
            (unsigned)sigrecvd,
            (unsigned)etask,
            (unsigned)regframe,
            (unsigned)savedpc,
            (unsigned)savedsp,
            (unsigned)spreg,
            (unsigned)splower,
            (unsigned)spupper);
    if (is_ram_ptr(m, spreg) && spreg >= splower && spreg + 48u <= spupper) {
        kprintf(" stk=%08x,%08x,%08x,%08x",
                (unsigned)ram32(m, spreg + 0u),
                (unsigned)ram32(m, spreg + 4u),
                (unsigned)ram32(m, spreg + 8u),
                (unsigned)ram32(m, spreg + 12u));
        kprintf("\n[OSDBG]     stk+10=%08x,%08x,%08x,%08x stk+20=%08x,%08x,%08x,%08x",
                (unsigned)ram32(m, spreg + 16u),
                (unsigned)ram32(m, spreg + 20u),
                (unsigned)ram32(m, spreg + 24u),
                (unsigned)ram32(m, spreg + 28u),
                (unsigned)ram32(m, spreg + 32u),
                (unsigned)ram32(m, spreg + 36u),
                (unsigned)ram32(m, spreg + 40u),
                (unsigned)ram32(m, spreg + 44u));
    }
    kprintf("\n");
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

static void dump_lvo(BellatrixMachine *m, uint32_t eb, const char *name, uint32_t lvo)
{
    uint32_t stub = eb - lvo * 6u;
    uint16_t op = ram16(m, stub);
    uint32_t target = 0;

    if (op == 0x4EF9u) {
        target = ram32(m, stub + 2u);
    } else if ((op & 0xFFC0u) == 0x4EC0u) {
        target = 0;
    }

    kprintf("[OSDBG-LVO] %-10s lvo=%u stub=%08x bytes=%04x %04x %04x target=%08x\n",
            name,
            (unsigned)lvo,
            stub,
            (unsigned)op,
            (unsigned)ram16(m, stub + 2u),
            (unsigned)ram16(m, stub + 4u),
            (unsigned)target);
}

static void dump_msgport(BellatrixMachine *m, uint32_t port, const char *label)
{
    char sigtask_name[32];
    uint32_t sigtask;
    uint32_t head;
    uint32_t tailpred;

    if (!is_ram_ptr(m, port)) {
        return;
    }

    sigtask = ram32(m, port + 16u) & 0x00FFFFFFu;
    head = ram32(m, port + 20u) & 0x00FFFFFFu;
    tailpred = ram32(m, port + 28u) & 0x00FFFFFFu;
    read_name(m, is_ram_ptr(m, sigtask) ? NODE_NAME(sigtask) : 0u, sigtask_name);

    kprintf("[OSDBG-PORT] %s @%08x flags=%02x sigbit=%u sigtask=%08x \"%s\""
            " head=%08x tailpred=%08x raw=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
            label,
            (unsigned)port,
            (unsigned)ram8(m, port + 14u),
            (unsigned)ram8(m, port + 15u),
            (unsigned)sigtask,
            sigtask_name,
            (unsigned)head,
            (unsigned)tailpred,
            (unsigned)ram32(m, port + 0u),
            (unsigned)ram32(m, port + 4u),
            (unsigned)ram32(m, port + 8u),
            (unsigned)ram32(m, port + 12u),
            (unsigned)ram32(m, port + 16u),
            (unsigned)ram32(m, port + 20u),
            (unsigned)ram32(m, port + 24u),
            (unsigned)ram32(m, port + 28u));
}

static void dump_list(BellatrixMachine *m, uint32_t list_addr, const char *label, int is_lib)
{
    uint32_t node;
    int count = 0;

    kprintf("[OSDBG] %s\n", label);

    node = LIST_HEAD(list_addr);
    while (is_ram_ptr(m, node) && node != list_addr + 4u && count < 32) {
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

static void read_bstr_name(BellatrixMachine *m, uint32_t ptr, char out[32])
{
    uint8_t len;
    uint8_t i;

    out[0] = '\0';
    ptr &= 0x00FFFFFFu;
    if (!is_ram_ptr(m, ptr - 1u)) {
        return;
    }

    len = ram8(m, ptr - 1u);
    if (len > 31u) {
        len = 31u;
    }
    for (i = 0; i < len; i++) {
        char c = (char)ram8(m, ptr + i);
        if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7eu) {
            c = '?';
        }
        out[i] = c;
    }
    out[len] = '\0';
}

static uint32_t find_library(BellatrixMachine *m, uint32_t liblist, const char *want)
{
    uint32_t node = LIST_HEAD(liblist);
    int count = 0;

    while (is_ram_ptr(m, node) && node != liblist + 4u && count < 96) {
        char name[32];
        read_name(m, NODE_NAME(node), name);
        if (strcmp(name, want) == 0) {
            return node;
        }
        node = NODE_SUCC(node);
        count++;
    }

    return 0;
}

static int plausible_screen(BellatrixMachine *m, uint32_t scr)
{
    int16_t width;
    int16_t height;

    scr &= 0x00FFFFFFu;
    if (!is_ram_ptr(m, scr)) {
        return 0;
    }

    width = SCREEN_WIDTH(scr);
    height = SCREEN_HEIGHT(scr);
    if (width < 64 || width > 2048 || height < 32 || height > 1024) {
        return 0;
    }

    return is_ram_ptr(m, SCREEN_RASTPORT(scr)) || is_ram_ptr(m, SCREEN_BITMAP(scr));
}

static int library_list_contains(BellatrixMachine *m, uint32_t liblist, uint32_t candidate)
{
    uint32_t node = LIST_HEAD(liblist);
    int count = 0;

    candidate &= 0x00FFFFFFu;
    while (is_ram_ptr(m, node) && node != liblist + 4u && count < 96) {
        if ((node & 0x00FFFFFFu) == candidate) {
            return 1;
        }
        node = NODE_SUCC(node);
        count++;
    }

    return 0;
}

static uint32_t find_intuition_base_from_task_stack(BellatrixMachine *m,
                                                    uint32_t eb,
                                                    uint32_t liblist)
{
    uint32_t wait = EXEC_TASKWAIT(eb);
    uint32_t node = LIST_HEAD(wait);
    int count = 0;

    while (is_ram_ptr(m, node) && node != wait + 4u && count < 96) {
        char name[32];
        read_name(m, NODE_NAME(node), name);
        if (strcmp(name, "Intuition menu handler") == 0) {
            uint32_t sp = TASK_SPREG(node) & 0x00FFFFFFu;
            uint32_t candidate = is_ram_ptr(m, sp + 4u) ?
                                 (ram32(m, sp + 4u) & 0x00FFFFFFu) : 0;
            if (library_list_contains(m, liblist, candidate)) {
                kprintf("[OSDBG-INTUI] using IntuitionBase from menu-handler stack @%08x\n",
                        (unsigned)candidate);
                return candidate;
            }
        }
        node = NODE_SUCC(node);
        count++;
    }

    return 0;
}

static uint32_t find_intuition_library(BellatrixMachine *m, uint32_t eb, uint32_t liblist)
{
    uint32_t by_name = find_library(m, liblist, "intuition.library");
    uint32_t by_task;
    uint32_t node;
    int count = 0;

    if (by_name) {
        return by_name;
    }

    by_task = find_intuition_base_from_task_stack(m, eb, liblist);
    if (by_task) {
        return by_task;
    }

    node = LIST_HEAD(liblist);
    while (is_ram_ptr(m, node) && node != liblist + 4u && count < 96) {
        uint32_t first_screen = INTUITION_FIRSTSCREEN(node) & 0x00FFFFFFu;
        if (plausible_screen(m, first_screen)) {
            kprintf("[OSDBG-INTUI] using library candidate @%08x v%u.%u open=%u firstScreen=%08x\n",
                    (unsigned)node,
                    (unsigned)(LIB_VERSION(node) >> 8),
                    (unsigned)(LIB_VERSION(node) & 0xFF),
                    (unsigned)LIB_OPENCNT(node),
                    (unsigned)first_screen);
            return node;
        }
        node = NODE_SUCC(node);
        count++;
    }

    return 0;
}

static uint8_t bitmap_mem8(BellatrixMachine *m, uint32_t addr)
{
    addr &= 0x00FFFFFFu;
    if (m && m->memory.chip_ram && addr < m->memory.chip_ram_size) {
        return m->memory.chip_ram[addr & m->memory.chip_ram_mask];
    }
    return ram8(m, addr);
}

static uint32_t bitmap_mem32(BellatrixMachine *m, uint32_t addr)
{
    return ((uint32_t)bitmap_mem8(m, addr + 0u) << 24) |
           ((uint32_t)bitmap_mem8(m, addr + 1u) << 16) |
           ((uint32_t)bitmap_mem8(m, addr + 2u) << 8) |
           (uint32_t)bitmap_mem8(m, addr + 3u);
}

static uint32_t count_bitmap_nonzero_bytes(BellatrixMachine *m, uint32_t base, uint32_t len)
{
    uint32_t i;
    uint32_t nonzero = 0;

    if (!is_ram_ptr(m, base)) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        if (bitmap_mem8(m, base + i) != 0u) {
            nonzero++;
        }
    }

    return nonzero;
}

static void dump_bitmap(BellatrixMachine *m, uint32_t bm, const char *label)
{
    uint16_t bpr;
    uint16_t rows;
    uint8_t depth;
    uint32_t plane_len;
    unsigned p;

    if (!is_ram_ptr(m, bm)) {
        kprintf("[OSDBG-INTUI]     %s bitmap invalid @%08x\n",
                label,
                (unsigned)bm);
        return;
    }

    bpr = BITMAP_BPR(bm);
    rows = BITMAP_ROWS(bm);
    depth = BITMAP_DEPTH(bm);
    plane_len = (uint32_t)bpr * (uint32_t)rows;
    if (plane_len > 0x100000u) {
        plane_len = 0x100000u;
    }

    kprintf("[OSDBG-INTUI]     %s bitmap @%08x bpr=%u rows=%u depth=%u flags=%02x\n",
            label,
            (unsigned)bm,
            (unsigned)bpr,
            (unsigned)rows,
            (unsigned)depth,
            (unsigned)BITMAP_FLAGS(bm));

    for (p = 0; p < depth && p < 8u; p++) {
        uint32_t plane = BITMAP_PLANE(bm, p) & 0x00FFFFFFu;
        uint32_t nonzero = 0;
        uint32_t first = 0;
        if (is_ram_ptr(m, plane)) {
            first = bitmap_mem32(m, plane);
            nonzero = count_bitmap_nonzero_bytes(m, plane, plane_len);
        }
        kprintf("[OSDBG-INTUI]       plane%u=%08x first=%08x nonzero=%u/%u\n",
                p,
                (unsigned)plane,
                (unsigned)first,
                (unsigned)nonzero,
                (unsigned)plane_len);
    }
}

static void dump_layer(BellatrixMachine *m, uint32_t layer)
{
    uint32_t cr;
    int count = 0;

    layer &= 0x00FFFFFFu;
    if (!is_ram_ptr(m, layer)) {
        return;
    }

    kprintf("[OSDBG-INTUI]       layer @%08x front=%08x back=%08x rp=%08x window=%08x bounds=%d,%d..%d,%d flags=%04x superBM=%08x clip=%08x\n",
            (unsigned)layer,
            (unsigned)(LAYER_FRONT(layer) & 0x00FFFFFFu),
            (unsigned)(LAYER_BACK(layer) & 0x00FFFFFFu),
            (unsigned)(LAYER_RP(layer) & 0x00FFFFFFu),
            (unsigned)(LAYER_WINDOW(layer) & 0x00FFFFFFu),
            (int)LAYER_MINX(layer),
            (int)LAYER_MINY(layer),
            (int)LAYER_MAXX(layer),
            (int)LAYER_MAXY(layer),
            (unsigned)LAYER_FLAGS(layer),
            (unsigned)(LAYER_SUPERBM(layer) & 0x00FFFFFFu),
            (unsigned)(LAYER_CLIPRECT(layer) & 0x00FFFFFFu));

    cr = LAYER_CLIPRECT(layer) & 0x00FFFFFFu;
    while (is_ram_ptr(m, cr) && count < 8) {
        kprintf("[OSDBG-INTUI]         clip @%08x next=%08x layer=%08x bm=%08x bounds=%d,%d..%d,%d\n",
                (unsigned)cr,
                (unsigned)(CLIPRECT_NEXT(cr) & 0x00FFFFFFu),
                (unsigned)(CLIPRECT_LAYER(cr) & 0x00FFFFFFu),
                (unsigned)(CLIPRECT_BITMAP(cr) & 0x00FFFFFFu),
                (int)CLIPRECT_MINX(cr),
                (int)CLIPRECT_MINY(cr),
                (int)CLIPRECT_MAXX(cr),
                (int)CLIPRECT_MAXY(cr));
        cr = CLIPRECT_NEXT(cr) & 0x00FFFFFFu;
        count++;
    }
}

static void dump_intuition(BellatrixMachine *m, uint32_t eb)
{
    uint32_t liblist = EXEC_LIBLIST(eb);
    uint32_t ib = find_intuition_library(m, eb, liblist);
    uint32_t scr;
    int scount = 0;

    if (!ib) {
        kprintf("[OSDBG-INTUI] intuition.library not found\n");
        return;
    }

    kprintf("[OSDBG-INTUI] IntuitionBase @%08x activeWindow=%08x activeScreen=%08x firstScreen=%08x\n",
            (unsigned)ib,
            (unsigned)(INTUITION_ACTIVEWINDOW(ib) & 0x00FFFFFFu),
            (unsigned)(INTUITION_ACTIVESCREEN(ib) & 0x00FFFFFFu),
            (unsigned)(INTUITION_FIRSTSCREEN(ib) & 0x00FFFFFFu));

    scr = INTUITION_FIRSTSCREEN(ib) & 0x00FFFFFFu;
    while (is_ram_ptr(m, scr) && scount < 8) {
        char title[32];
        uint32_t rp = SCREEN_RASTPORT(scr);
        uint32_t bm = RASTPORT_BITMAP(rp) & 0x00FFFFFFu;
        uint32_t win;
        int wcount = 0;

        read_name(m, SCREEN_TITLE(scr), title);
        kprintf("[OSDBG-INTUI]   screen @%08x next=%08x firstWindow=%08x \"%s\" pos=%d,%d size=%dx%d flags=%04x vp=%08x rp=%08x layer=%08x rpBM=%08x embeddedBM=%08x\n",
                (unsigned)scr,
                (unsigned)(SCREEN_NEXT(scr) & 0x00FFFFFFu),
                (unsigned)(SCREEN_FIRSTWINDOW(scr) & 0x00FFFFFFu),
                title,
                (int)SCREEN_LEFT(scr),
                (int)SCREEN_TOP(scr),
                (int)SCREEN_WIDTH(scr),
                (int)SCREEN_HEIGHT(scr),
                (unsigned)SCREEN_FLAGS(scr),
                (unsigned)SCREEN_VIEWPORT(scr),
                (unsigned)rp,
                (unsigned)(RASTPORT_LAYER(rp) & 0x00FFFFFFu),
                (unsigned)bm,
                (unsigned)SCREEN_BITMAP(scr));
        dump_bitmap(m, bm ? bm : SCREEN_BITMAP(scr), "screen");

        win = SCREEN_FIRSTWINDOW(scr) & 0x00FFFFFFu;
        while (is_ram_ptr(m, win) && wcount < 16) {
            char wtitle[32];
            uint32_t wrp = WINDOW_RPORT(win) & 0x00FFFFFFu;
            uint32_t wbm = is_ram_ptr(m, wrp) ? (RASTPORT_BITMAP(wrp) & 0x00FFFFFFu) : 0;

            read_name(m, WINDOW_TITLE(win), wtitle);
            kprintf("[OSDBG-INTUI]     window @%08x next=%08x \"%s\" pos=%d,%d size=%dx%d flags=%08x req=%08x reqcnt=%d screen=%08x rp=%08x bm=%08x layer=%08x userPort=%08x windowPort=%08x\n",
                    (unsigned)win,
                    (unsigned)(WINDOW_NEXT(win) & 0x00FFFFFFu),
                    wtitle,
                    (int)WINDOW_LEFT(win),
                    (int)WINDOW_TOP(win),
                    (int)WINDOW_WIDTH(win),
                    (int)WINDOW_HEIGHT(win),
                    (unsigned)WINDOW_FLAGS(win),
                    (unsigned)(WINDOW_FIRSTREQUEST(win) & 0x00FFFFFFu),
                    (int)WINDOW_REQCOUNT(win),
                    (unsigned)(WINDOW_WSCREEN(win) & 0x00FFFFFFu),
                    (unsigned)wrp,
                    (unsigned)wbm,
                    (unsigned)(WINDOW_WLAYER(win) & 0x00FFFFFFu),
                    (unsigned)(WINDOW_USERPORT(win) & 0x00FFFFFFu),
                    (unsigned)(WINDOW_WINDOWPORT(win) & 0x00FFFFFFu));
            if (wbm && wbm != bm) {
                dump_bitmap(m, wbm, "window");
            }
            dump_layer(m, WINDOW_WLAYER(win));
            win = WINDOW_NEXT(win) & 0x00FFFFFFu;
            wcount++;
        }

        scr = SCREEN_NEXT(scr) & 0x00FFFFFFu;
        scount++;
    }

    if (scount == 0) {
        kprintf("[OSDBG-INTUI]   no screens\n");
    }
}

static int dump_dos_segments_for_base(BellatrixMachine *m, uint32_t base)
{
    uint32_t root = DOS_DL_ROOT(base);
    uint32_t info;
    uint32_t seg;
    int count = 0;
    int plausible = 0;

    if (!is_ram_ptr(m, root)) {
        return 0;
    }

    info = ROOT_RN_INFO(root);
    if (!is_ram_ptr(m, info)) {
        return 0;
    }

    seg = DOSINFO_RESLIST(info);
    if (!is_ram_ptr(m, seg)) {
        return 0;
    }

    while (is_ram_ptr(m, seg) && count < 64) {
        char name[32];
        int32_t uc = SEG_UC(seg);
        uint32_t next = SEG_NEXT(seg);

        read_bstr_name(m, SEG_BSTR(seg), name);
        if (name[0] != '\0') {
            plausible = 1;
        }

        kprintf("[OSDBG-DOSSEG] dosbase=%08x root=%08x info=%08x seg=%08x "
                "uc=%d seglist=%08x name=\"%s\" next=%08x\n",
                (unsigned)base,
                (unsigned)root,
                (unsigned)info,
                (unsigned)seg,
                (int)uc,
                (unsigned)SEG_SEG(seg),
                name,
                (unsigned)next);

        if (next == 0u || next == seg) {
            break;
        }
        seg = next;
        count++;
    }

    return plausible;
}

static void dump_dos_segments(BellatrixMachine *m, uint32_t liblist)
{
    uint32_t node;
    int count = 0;
    int found = 0;

    kprintf("[OSDBG] DOS resident segments:\n");
    node = LIST_HEAD(liblist);
    while (is_ram_ptr(m, node) && node != liblist + 4u && count < 64) {
        if (dump_dos_segments_for_base(m, node)) {
            found = 1;
        }
        node = NODE_SUCC(node);
        count++;
    }

    if (!found) {
        kprintf("[OSDBG-DOSSEG] no plausible DOS segment list found\n");
    }
}

void os_debug_dump(BellatrixMachine *m)
{
    uint32_t eb_ptr;

    kprintf("[OSDBG] ---- OS state ----\n");

    if (!m || !m->memory.chip_ram || m->memory.chip_ram_size < 8u) {
        kprintf("[OSDBG] ExecBase invalid: no chip RAM\n");
        return;
    }

    eb_ptr = ((uint32_t)m->memory.chip_ram[4] << 24) |
             ((uint32_t)m->memory.chip_ram[5] << 16) |
             ((uint32_t)m->memory.chip_ram[6] << 8) |
             (uint32_t)m->memory.chip_ram[7];
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

    dump_lvo(m, eb_ptr, "SetSignal", 51);
    dump_lvo(m, eb_ptr, "Wait", 53);
    dump_lvo(m, eb_ptr, "PutMsg", 61);
    dump_lvo(m, eb_ptr, "ReplyMsg", 63);
    dump_lvo(m, eb_ptr, "Signal", 127);

    dump_list(m, EXEC_LIBLIST(eb_ptr), "LibList:", 1);
    dump_dos_segments(m, EXEC_LIBLIST(eb_ptr));
    dump_intuition(m, eb_ptr);
    dump_list(m, EXEC_TASKREADY(eb_ptr), "TaskReady:", 0);
    dump_list(m, EXEC_TASKWAIT(eb_ptr), "TaskWait:", 0);
    dump_msgport(m, 0x00c28f74u, "blocked bootstrap WaitPort");
    dump_msgport(m, 0x00c93ad0u, "last observed PutMsg port");

    kprintf("[OSDBG] ---- end ----\n");
}

int os_debug_rescue_waiting_signaled_tasks(BellatrixMachine *m)
{
    uint32_t eb_ptr;
    uint32_t ready;
    uint32_t wait;
    uint32_t node;
    int moved = 0;
    int count = 0;

    if (!m || !m->memory.chip_ram || m->memory.chip_ram_size < 8u) {
        return 0;
    }

    eb_ptr = ((uint32_t)m->memory.chip_ram[4] << 24) |
             ((uint32_t)m->memory.chip_ram[5] << 16) |
             ((uint32_t)m->memory.chip_ram[6] << 8) |
             (uint32_t)m->memory.chip_ram[7];
    if (!is_ram_ptr(m, eb_ptr)) {
        return 0;
    }

    ready = EXEC_TASKREADY(eb_ptr);
    wait = EXEC_TASKWAIT(eb_ptr);

    node = LIST_HEAD(wait);
    while (is_ram_ptr(m, node) && node != wait + 4u && count < 64) {
        uint32_t next = NODE_SUCC(node);
        uint32_t sigwait = TASK_SIGWAIT(node);
        uint32_t sigrecvd = TASK_SIGRECVD(node);
        uint8_t state = TASK_STATE(node);
        if (state == TS_WAIT &&
                (sigwait & sigrecvd) != 0u) {
            char name[32];

            read_name(m, NODE_NAME(node), name);
            remove_node(m, node);
            ramw8(m, node + TASK_STATE_OFF, TS_READY);
            enqueue_task(m, ready, node);
            moved++;

            kprintf("[OSDBG-RESCUE] task @%08x \"%s\" wait=%08x recvd=%08x -> READY\n",
                    node,
                    name,
                    (unsigned)sigwait,
                    (unsigned)sigrecvd);
        }

        node = next;
        count++;
    }

    return moved;
}

int os_debug_suppress_process_requesters(BellatrixMachine *m)
{
    uint32_t eb_ptr;
    uint32_t ready;
    uint32_t wait;
    uint32_t lists[2];
    int changed = 0;
    unsigned li;

    if (!m || !m->memory.chip_ram || m->memory.chip_ram_size < 8u) {
        return 0;
    }

    eb_ptr = ((uint32_t)m->memory.chip_ram[4] << 24) |
             ((uint32_t)m->memory.chip_ram[5] << 16) |
             ((uint32_t)m->memory.chip_ram[6] << 8) |
             (uint32_t)m->memory.chip_ram[7];
    if (!is_ram_ptr(m, eb_ptr)) {
        return 0;
    }

    ready = EXEC_TASKREADY(eb_ptr);
    wait = EXEC_TASKWAIT(eb_ptr);
    lists[0] = ready;
    lists[1] = wait;

    for (li = 0; li < 2u; li++) {
        uint32_t list = lists[li];
        uint32_t node = LIST_HEAD(list);
        int count = 0;

        while (is_ram_ptr(m, node) && node != list + 4u && count < 64) {
            uint32_t next = NODE_SUCC(node);
            if (NODE_TYPE(node) == NT_PROCESS &&
                    ram32(m, PROCESS_WINDOWPTR(node)) != 0xFFFFFFFFu) {
                char name[32];
                read_name(m, NODE_NAME(node), name);
                ramw32(m, PROCESS_WINDOWPTR(node), 0xFFFFFFFFu);
                changed++;
                kprintf("[OSDBG-REQSUP] task @%08x \"%s\" pr_WindowPtr=-1\n",
                        (unsigned)node,
                        name);
            }
            node = next;
            count++;
        }
    }

    return changed;
}
