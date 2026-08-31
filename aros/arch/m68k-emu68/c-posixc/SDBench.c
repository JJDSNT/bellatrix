/*
 * Sequential read, with the buffer size as a variable.
 *
 * sdcard.md sec.5 and sec.7: boot time is one number that mixes every layer,
 * and a single I/O size hides the class of fault where a path fails to benefit
 * from larger requests. This reads one file end to end with a chosen buffer
 * and reports what it cost.
 *
 *     SDBench FILE/A [BUF/K/N]
 *
 * Timing is DateStamp's tick field: 1/50 s, which is coarse but the reads
 * being measured take seconds. Everything is reported, not just the rate, so
 * a run that was too short to mean anything says so.
 */

#include <dos/dos.h>
#include <dos/datetime.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>

#define TICKS_PER_SEC 50

static ULONG now_ticks(void)
{
    struct DateStamp ds;

    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 60 * TICKS_PER_SEC + (ULONG)ds.ds_Tick;
}

int main(int argc, char *argv[])
{
    const char *name = (argc > 1) ? argv[1] : NULL;
    ULONG bufkb = (argc > 2) ? (ULONG)atoi(argv[2]) : 64;
    ULONG bufsize;
    APTR  buf;
    BPTR  fh;
    ULONG total = 0, t0, t1, ticks;
    LONG  n;

    if (!name)
    {
        printf("SDBench FILE [BUFKB]\n");
        return RETURN_FAIL;
    }

    if (bufkb == 0)
        bufkb = 1;
    bufsize = bufkb * 1024;

    buf = AllocMem(bufsize, MEMF_ANY);
    if (!buf)
    {
        printf("SDBench: no memory for a %lu KB buffer\n", bufkb);
        return RETURN_FAIL;
    }

    fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
    if (!fh)
    {
        printf("SDBench: cannot open %s\n", name);
        FreeMem(buf, bufsize);
        return RETURN_FAIL;
    }

    t0 = now_ticks();
    while ((n = Read(fh, buf, bufsize)) > 0)
        total += (ULONG)n;
    t1 = now_ticks();

    Close(fh);
    FreeMem(buf, bufsize);

    ticks = (t1 >= t0) ? (t1 - t0) : 0;

    printf("SDBench %s buf=%luKB bytes=%lu ticks=%lu (%lu.%02lus)",
           name, bufkb, total, ticks,
           ticks / TICKS_PER_SEC, (ticks % TICKS_PER_SEC) * 2);

    if (ticks >= TICKS_PER_SEC / 2)
        printf(" = %lu KB/s\n", (total / 1024) * TICKS_PER_SEC / ticks);
    else
        printf(" -- too short to rate\n");

    return RETURN_OK;
}
