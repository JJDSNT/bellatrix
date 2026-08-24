/*
 * Read blocks straight from the device, with no DOS and no filesystem.
 *
 * sdcard.md sec.10: a slow file operation does not mean the driver is slow, and
 * good device throughput does not mean the driver behaves under a filesystem's
 * request pattern. Everything measured here so far went through DOS and FAT.
 * This does not, so the two can finally be compared.
 *
 *     SDRaw [KBPERREAD] [TOTALMB]
 *
 * Reads TOTALMB from the start of unit 0 in KBPERREAD chunks and reports the
 * rate. Timing is DateStamp's tick field, 1/50 s.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/trackdisk.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

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
    ULONG chunkkb = (argc > 1) ? (ULONG)atoi(argv[1]) : 64;
    ULONG totalmb = (argc > 2) ? (ULONG)atoi(argv[2]) : 8;
    struct MsgPort *port;
    struct IOStdReq *io;
    APTR   buf;
    ULONG  chunk, total, off, t0, t1, ticks;

    if (chunkkb == 0) chunkkb = 1;
    if (totalmb == 0) totalmb = 1;
    chunk = chunkkb * 1024;
    total = totalmb * 1024 * 1024;

    port = CreateMsgPort();
    if (!port) { printf("SDRaw: no port\n"); return RETURN_FAIL; }

    io = (struct IOStdReq *)CreateIORequest(port, sizeof(struct IOStdReq));
    if (!io) { DeleteMsgPort(port); printf("SDRaw: no request\n"); return RETURN_FAIL; }

    if (OpenDevice((CONST_STRPTR)"sdcard.device", 0, (struct IORequest *)io, 0) != 0)
    {
        printf("SDRaw: cannot open sdcard.device unit 0\n");
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return RETURN_FAIL;
    }

    /* MEMF_PUBLIC and 32-byte aligned, so the driver's direct-DMA path is the
     * one being measured rather than its bounce path. */
    buf = AllocMem(chunk + 32, MEMF_PUBLIC | MEMF_CLEAR);
    if (!buf)
    {
        printf("SDRaw: no buffer\n");
        CloseDevice((struct IORequest *)io);
        DeleteIORequest((struct IORequest *)io);
        DeleteMsgPort(port);
        return RETURN_FAIL;
    }

    t0 = now_ticks();
    for (off = 0; off < total; off += chunk)
    {
        io->io_Command = CMD_READ;
        io->io_Data    = (APTR)(((IPTR)buf + 31) & ~(IPTR)31);
        io->io_Length  = chunk;
        io->io_Offset  = off;

        if (DoIO((struct IORequest *)io) != 0)
        {
            printf("SDRaw: read failed at offset %lu\n", off);
            break;
        }
    }
    t1 = now_ticks();

    ticks = (t1 >= t0) ? (t1 - t0) : 0;

    printf("SDRaw chunk=%luKB total=%luMB ticks=%lu (%lu.%02lus)",
           chunkkb, totalmb, ticks,
           ticks / TICKS_PER_SEC, (ticks % TICKS_PER_SEC) * 2);
    if (ticks >= TICKS_PER_SEC / 2)
        printf(" = %lu KB/s\n", (off / 1024) * TICKS_PER_SEC / ticks);
    else
        printf(" -- too short to rate\n");

    FreeMem(buf, chunk + 32);
    CloseDevice((struct IORequest *)io);
    DeleteIORequest((struct IORequest *)io);
    DeleteMsgPort(port);
    return RETURN_OK;
}
