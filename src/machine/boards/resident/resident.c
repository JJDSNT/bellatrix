/*
 * src/machine/boards/resident/resident.c
 *
 * Bellatrix CD Automount — Zorro II Autoconfig board with DiagArea entry point.
 *
 * Why this exists
 * ---------------
 * AROS CDFS never mounts an ISO present at boot due to a structural race
 * in ata.device: AddBootNode (which starts CDFS_Handler) fires *before*
 * the DaemonReq is added to the daemon list.  CDFS_Handler calls
 * BCache_Create → TD_CHANGENUM while au_ChangeNum is still 0, so
 * bp_ChangeIdx=0.  Two seconds later the daemon increments au_ChangeNum
 * to 1, and BCache_Present returns RETURN_WARN forever (bcache.c:363 has
 * the wrong assignment direction; CDFS_DevicePresent ignores RETURN_WARN
 * when cd_Volume==NULL).
 *
 * Fix
 * ---
 * A second AddBootNode is issued *after* the daemon has fired.  We use a
 * Zorro II Autoconfig board with a DiagArea.  AROS expansion library reads
 * er_InitDiagVec=0x0010, copies da_Size (0x8000) bytes from board_base+0x0010
 * to AllocMem, and calls copy_base + da_DiagPoint.  That creates a background
 * task (priority -50) that:
 *   1. Spins on OpenLibrary("dos.library") until DOS is ready.
 *   2. Delay(DAEMON_WAIT_TICKS) — waits ~3 s; daemon fires at ~2 s.
 *   3. Calls MountDrive("ata.device", unit 0, cdBoot=TRUE).
 *
 * ROM memory layout (flat binary produced by objcopy -O binary)
 * -------------------------------------------------------------
 *   0x0000: 16 bytes 0xFF padding  ← romhdr global asm (first in .text)
 *   0x0010: 14-byte struct DiagArea + 2-byte pad = 16 bytes total
 *            da_DiagPoint = 16 → entry at copy_base+16 = right after DiagArea
 *   0x0020: diag_point()  ← first C function (immediately follows asm bytes)
 *   ...   : mount_task(), string helpers, __mulsi3, mounter.c code
 *
 * Layout guarantee
 * ----------------
 * GCC emits file-scope __asm__ blocks in source order, before the first C
 * function.  The two asm blocks at the TOP of this file therefore land at
 * offset 0x0000 and 0x0010 in the single .text HUNK.  diag_point() is the
 * first C function defined, so it follows at 0x0020.
 *
 * All code is compiled with -mpcrel; function calls use JSR (label,PC) —
 * PC-relative, valid on m68000, correct after DiagArea CopyMem.
 *
 * Build
 * -----
 *   make -C src/machine/boards/resident
 *   (requires m68k-amigaos-gcc from the bebbo toolchain via Docker wrapper)
 */

/* =========================================================================
 * ROM header and DiagArea — emitted FIRST in .text (before any C function).
 * File-scope __asm__ is output in source order; these two blocks land at
 * binary offsets 0x0000 and 0x0010 respectively.
 * ====================================================================== */

/* 0x0000: 16 bytes of 0xFF (romhdr pad before DiagArea) */
__asm__(
    ".text\n"
    ".byte 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF\n"
    ".byte 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF\n"
);

/* 0x0010: struct DiagArea (14 bytes) + 2-byte align pad = 16 bytes total.
 *
 *   da_Config    = 0x90: bit7=word-wide (DAC_WORDWIDE), bits[5:4]=01=configtime
 *   da_Flags     = 0x00
 *   da_Size      = 0x8000: AROS copies 32 KB starting from board_base+er_InitDiagVec
 *   da_DiagPoint = 16: offset from DiagArea start to entry; sizeof(DiagArea)=14
 *                  in this toolchain but the section is 16 bytes aligned, so the
 *                  code starts 16 bytes after the DiagArea in the binary.
 *   rest = 0
 */
__asm__(
    ".byte 0x90, 0x00\n"    /* da_Config, da_Flags */
    ".word 0x8000\n"        /* da_Size */
    ".word 16\n"            /* da_DiagPoint: code starts 16 bytes from DiagArea */
    ".word 0, 0, 0, 0\n"   /* da_BootPoint, da_Name, da_Reserved01, da_Reserved02 */
    ".word 0\n"             /* 2-byte pad: sizeof(DiagArea)=14 → align to 16 */
);

/* 0x0020: entry point trampoline — exactly where da_DiagPoint=16 points.
 * AROS DiagArea calling convention passes SysBase in A6; we push it onto
 * the stack so diag_point() receives it as a normal C argument (avoids
 * relying on *(ExecBase **)4 which is not yet written at diag-init time).
 */
__asm__(
    ".globl _rom_entry\n"
    "_rom_entry:\n"
    "    move.l  %a6,-(%sp)\n"    /* push SysBase (A6 = AROS DiagArea convention) */
    "    jsr     (_diag_point,pc)\n" /* PC-relative call to C diag_point */
    "    addq.l  #4,%sp\n"        /* pop SysBase argument */
    "    rts\n"                   /* return d0 (1=keep copy) to AROS */
);

/* =========================================================================
 * Headers
 * ====================================================================== */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <libraries/expansion.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stddef.h>

#include "../../../external/mounter/mounter.h"

/* =========================================================================
 * Configuration
 * ====================================================================== */

#define RESIDENT_NAME      "bellatrix.cdmount"
#define RESIDENT_PRI       (-50)
#define DAEMON_WAIT_TICKS   150    /* 3 s at 50 Hz — daemon fires at ~2 s */

/* =========================================================================
 * Debug helpers — direct SERDAT write (0xDFF030), no exec LVO dependency.
 * AROS's own debug output goes through the same hardware register.
 * ====================================================================== */

static void dbg_putc(unsigned char c)
{
    /* AROS krnPutC pattern: poll SERDATR (0xDFF018) bit 13 (TBE), then
     * write SERDAT_STP8|byte to SERDAT (0xDFF030).
     * Use inline asm throughout — C volatile pointers generate PC-relative
     * load sequences under -mpcrel that crash on m68000. */
    unsigned short sr;
    __asm__ volatile (
        "0: move.w 0x00DFF018,%0 \n\t"  /* read SERDATR */
        "   btst   #13,%0        \n\t"  /* test TBE (bit 13) */
        "   beq.s  0b            \n\t"  /* loop while TBE=0 */
        "   move.w %1,0x00DFF030"       /* write SERDAT_STP8 | char */
        : "=d"(sr)
        : "d"((unsigned short)(0x0100u | (unsigned short)c))
        : "cc"
    );
}

static void dbg_str(const char *s)
{
    while (*s)
        dbg_putc((unsigned char)*s++);
}

static void dbg_hex(ULONG val)
{
    int i;
    dbg_putc('0'); dbg_putc('x');
    for (i = 28; i >= 0; i -= 4) {
        int n = (val >> i) & 0xF;
        dbg_putc(n < 10 ? ('0' + n) : ('a' + n - 10));
    }
}

#define DBG(s)         dbg_str("[CDMOUNT] " s "\n")
#define DBG_HEX(s, v)  do { dbg_str("[CDMOUNT] " s); \
                             dbg_hex((ULONG)(v));      \
                             dbg_putc('\n'); } while(0)

/* =========================================================================
 * Forward declarations
 * ====================================================================== */

static void mount_task(void);

/* =========================================================================
 * diag_point — FIRST C function in this file; lands at ROM offset 0x0020.
 * Called by AROS expansion library after CopyMem'ing the DiagArea block.
 * Returns nonzero to tell AROS to keep the copy permanently.
 *
 * Uses AllocMem + AddTask (inline exec vectors, local SysBase) instead of
 * clib/alib_protos.h CreateTask to avoid needing a global SysBase.
 * ====================================================================== */

int __attribute__((used)) diag_point(struct ExecBase *SysBase)
{
    const ULONG stack_size = 8192;

    DBG("diag_point entered");
    DBG_HEX("SysBase=", (ULONG)SysBase);

    UBYTE *mem = (UBYTE *)AllocMem(sizeof(struct Task) + stack_size,
                                   MEMF_CLEAR | MEMF_PUBLIC);
    if (!mem) {
        DBG("AllocMem FAILED");
        return 0;
    }
    DBG_HEX("task mem=", (ULONG)mem);

    struct Task *task  = (struct Task *)mem;
    UBYTE       *stack = mem + sizeof(struct Task);

    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri  = RESIDENT_PRI;
    task->tc_Node.ln_Name = (STRPTR)RESIDENT_NAME;
    task->tc_SPLower      = stack;
    task->tc_SPUpper      = stack + stack_size;
    task->tc_SPReg        = task->tc_SPUpper;

    DBG_HEX("AddTask pc=", (ULONG)mount_task);
    AddTask(task, (APTR)mount_task, NULL);
    DBG("diag_point done, returning 1");
    return 1;
}

/* =========================================================================
 * mount_task — background task, priority -50
 * ====================================================================== */

static void mount_task(void)
{
    struct ExecBase    *SysBase = *(struct ExecBase **)4;
    struct DosLibrary  *DOSBase;
    ULONG               unit = 0;

    DBG("mount_task started");

    /* Spin until dos.library is available (we run at -50, everything more
     * important finishes first; this loop is rarely hit more than once).  */
    while (!(DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 0)))
        ;

    DBG("dos.library opened");
    DBG("waiting DAEMON_WAIT_TICKS...");
    Delay(DAEMON_WAIT_TICKS);
    DBG("delay done, calling MountDrive");

    struct MountStruct ms;
    ms.deviceName  = "ata.device";
    ms.unitNum     = &unit;
    ms.creatorName = RESIDENT_NAME;
    ms.configDev   = NULL;
    ms.SysBase     = SysBase;
    ms.luns        = FALSE;
    ms.slowSpinup  = FALSE;
    ms.cdBoot      = TRUE;
    ms.ignoreLast  = FALSE;
    ms.hostId      = 255;

    MountDrive(&ms);

    DBG("MountDrive returned");
    CloseLibrary((struct Library *)DOSBase);
    RemTask(NULL);
}

/* =========================================================================
 * String helpers — satisfy mounter.c without linking libc (-nostdlib).
 * Defined here so they appear in the same .text HUNK as the rest of the ROM.
 * ====================================================================== */

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* =========================================================================
 * __mulsi3 — 32-bit signed multiply for m68000.
 *
 * The bebbo libc.a provides this, but -nostdlib means it isn't linked.
 * m68000 has only 16×16→32 MULU; mounter.c triggers two calls for sector
 * LBA * sector-size calculations.  Implementation mirrors libnix exactly.
 *
 * Calling convention: args pushed on stack before JSR (pc-rel); result d0.
 * ====================================================================== */

__asm__(
    ".text\n"
    ".globl ___mulsi3\n"
    "___mulsi3:\n"
    "    movem.l 4(%sp),%d0-%d1\n"  /* d0=a, d1=b */
    "    move.l %d3,-(%sp)\n"
    "    move.l %d2,-(%sp)\n"
    "    move.w %d1,%d2\n"           /* d2 = b_lo */
    "    mulu.w %d0,%d2\n"           /* d2 = a_lo * b_lo */
    "    move.l %d1,%d3\n"
    "    swap %d3\n"                  /* d3_lo = b_hi */
    "    mulu.w %d0,%d3\n"           /* d3 = a_lo * b_hi */
    "    swap %d3\n"                  /* shift <<16 */
    "    clr.w %d3\n"
    "    add.l %d3,%d2\n"
    "    swap %d0\n"                  /* d0_lo = a_hi */
    "    mulu.w %d1,%d0\n"           /* d0 = a_hi * b_lo */
    "    swap %d0\n"                  /* shift <<16 */
    "    clr.w %d0\n"
    "    add.l %d2,%d0\n"            /* d0 = result */
    "    move.l (%sp)+,%d2\n"
    "    move.l (%sp)+,%d3\n"
    "    rts\n"
);

/* =========================================================================
 * mounter.c — included last so its code follows the ROM entry points.
 * String declarations from string.h (included by mounter.c) match the
 * definitions above; the linker resolves calls to our implementations.
 * ====================================================================== */

#include "../../../external/mounter/mounter.c"
