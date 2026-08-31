/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: DEVS:Monitors loader for the VideoCore graphics driver.
*/

#include <aros/debug.h>

#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <oop/oop.h>

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/oop.h>

/* Must match CLID_Hidd_Gfx_VideoCore4 in the driver's vcgfx_hidd.h. Named
 * here rather than included: that header pulls in the driver's private
 * static data layout, and this program only needs the class name. */
#define VCGFX_CLASSID   "hidd.gfx.bcmvc4"
#define VCGFX_LIBNAME   "vcgfx.hidd"

int __nocommandline = 1;

/*
 * AROSMonDrvs (run from rom/dos/boot.c) executes everything it finds in
 * DEVS:Monitors, and this is what it finds for the VideoCore. Its whole job
 * is to bring the driver into memory at a point where the filesystem exists.
 *
 * Registration itself happens inside the driver's own InitLib, which calls
 * AddDisplayDriver() as it comes up -- so opening the library is the whole
 * of the work, and this program's success is measured by the class being
 * there afterwards rather than by anything it does itself.
 *
 * That differs from rom/hidds/gfx/headless, whose loader registers the driver
 * so it can pass the user's depth configuration from its icon's tooltypes.
 * There is one display on this machine and no such configuration to pass; if
 * that changes, the driver's self-registration has to be put behind a
 * FindResident() guard first, the way headless does it, or the loader will
 * always be too late to supply tags.
 *
 * The library is deliberately left open. Closing the last reference would
 * expunge the driver that has just registered itself.
 */
int main(void)
{
    struct Library *VCGfxBase;

    if (FindName(&SysBase->LibList, VCGFX_LIBNAME))
    {
        D(bug("[VCGfx:Disk] %s is already resident\n", VCGFX_LIBNAME));
        return RETURN_OK;
    }

    VCGfxBase = OpenLibrary(VCGFX_LIBNAME, 0);
    if (!VCGfxBase)
        VCGfxBase = OpenLibrary("DEVS:Drivers/" VCGFX_LIBNAME, 0);

    if (!VCGfxBase)
    {
        D(bug("[VCGfx:Disk] %s would not open\n", VCGFX_LIBNAME));
        return RETURN_FAIL;
    }

    D(bug("[VCGfx:Disk] driver loaded @ 0x%p\n", VCGfxBase));

    /*
     * No VideoCore, no display driver: the driver's InitLib fails its mailbox
     * probe and never registers. Say so and leave -- the boot driver stays,
     * which is what DDRV_BootMode is for, and is the whole fallback.
     */
    if (!OOP_FindClass((STRPTR)VCGFX_CLASSID))
    {
        D(bug("[VCGfx:Disk] driver did not register a display class\n"));
        CloseLibrary(VCGfxBase);
        return RETURN_WARN;
    }

    return RETURN_OK;
}
