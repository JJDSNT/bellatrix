/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: DEVS:Monitors loader for the classic Amiga chipset display driver.
*/

#include <aros/debug.h>

#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <oop/oop.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/oop.h>

/* Must match CLID_Hidd_Gfx_AmigaVideo in the driver's include/amigavideo.h.
 * Named here rather than included, for the reason vcgfx's loader gives: that
 * header belongs to the driver, and this program needs only the class name. */
#define AMIGAVIDEO_CLASSID  "hidd.gfx.amigavideo"
#define AMIGAVIDEO_LIBNAME  "amigavideo.hidd"

int __nocommandline = 1;

/*
 * The driver is built with __OOP_NOLIBBASE__, and USER_CPPFLAGS reaches this
 * program too, so proto/oop.h expects the caller to supply the base rather
 * than referring to a global one. Opening it here is the honest way round:
 * this program genuinely does use oop.library, for one call.
 */
static struct Library *OOPBase;

/*
 * The same arrangement as the VideoCore loader beside this one: AROSMonDrvs
 * runs everything in DEVS:Monitors, and this brings the driver into memory at
 * a point where the filesystem exists. The driver registers itself from its
 * own InitLib -- upstream's startup.c calls AddDisplayDriver() there -- so
 * opening the library is the whole of the work.
 *
 * One difference worth knowing, and it is upstream's choice rather than ours:
 * this driver registers with DDRV_KeepBootMode, so it does *not* retire the
 * driver already displaying. It asks for monitor ID 0 and adds itself beside
 * whatever is there. On this machine that means the chipset and the VideoCore
 * are both display drivers, which is the arrangement a real Amiga with a
 * graphics card has, and is what makes a display source switch possible at
 * all.
 *
 * Unlike a real Amiga, the chipset here renders into memory that nothing
 * scans out yet. Bellatrix publishes each finished frame at $01000000 for a
 * consumer to present (src/amiga/frame.h, C:DeniseView); until something does
 * that, opening a screen on this driver draws a picture nobody sees.
 *
 * The library is deliberately left open. Closing the last reference would
 * expunge the driver that has just registered itself.
 */
int main(void)
{
    struct Library *AmigaVideoBase;

    if (FindName(&SysBase->LibList, AMIGAVIDEO_LIBNAME))
    {
        D(bug("[AmigaVideo:Disk] %s is already resident\n", AMIGAVIDEO_LIBNAME));
        return RETURN_OK;
    }

    AmigaVideoBase = OpenLibrary(AMIGAVIDEO_LIBNAME, 0);
    if (!AmigaVideoBase)
        AmigaVideoBase = OpenLibrary("DEVS:Drivers/" AMIGAVIDEO_LIBNAME, 0);

    if (!AmigaVideoBase)
    {
        D(bug("[AmigaVideo:Disk] %s would not open\n", AMIGAVIDEO_LIBNAME));
        return RETURN_FAIL;
    }

    D(bug("[AmigaVideo:Disk] driver loaded @ 0x%p\n", AmigaVideoBase));

    OOPBase = OpenLibrary("oop.library", 0);
    if (OOPBase)
    {
        BOOL registered = OOP_FindClass((STRPTR)AMIGAVIDEO_CLASSID) != NULL;

        CloseLibrary(OOPBase);
        OOPBase = NULL;

        if (!registered)
        {
            D(bug("[AmigaVideo:Disk] driver did not register a display class\n"));
            CloseLibrary(AmigaVideoBase);
            return RETURN_WARN;
        }
    }

    return RETURN_OK;
}
