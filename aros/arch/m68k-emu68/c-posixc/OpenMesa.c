/*
 * Open one library and say how long it took.
 *
 * ISSUE-0045 stops inside OpenLibrary("mesa3dgl20-0.library", 0), reached from
 * gl.library's LibOpen, itself inside OpenLibrary("gl.library"), itself inside
 * a program's autoinit. Three levels of nesting were assumed to matter because
 * the same library appeared to open in 19 seconds from the shell.
 *
 * That measurement was `Version mesa3dgl20-0.library`, and Version does not
 * have to open a library to answer -- it can read the version string out of
 * the file. Nineteen seconds is about what reading 10 MB costs. So the
 * comparison the whole nesting theory rests on may never have been an
 * OpenLibrary at all.
 *
 * This does nothing else: no GL, no nesting, one open, with the clock either
 * side and bug() on the raw serial so it survives a machine that stops
 * answering.
 */

#include <aros/debug.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/timer.h>
#include <devices/timer.h>
#include <dos/var.h>
#include <string.h>

int main(int argc, char *argv[])
{
    /* With no argument, open the Mesa library gl.library itself would: the
     * one ENV:SYS/GL.default names, which the build writes for the Mesa
     * version it built (mesa3dgl20-0, mesa3dgl26-0, ...). */
    static char fromenv[64];
    const char *name = (argc > 1) ? argv[1] : NULL;
    struct Library *lib;

    if (!name)
    {
        LONG len = GetVar((CONST_STRPTR)"SYS/GL.default", (STRPTR)fromenv,
                          sizeof(fromenv) - 8, GVF_GLOBAL_ONLY);
        if (len <= 0)
        {
            bug("[openmesa] no ENV:SYS/GL.default and no library named\n");
            return RETURN_FAIL;
        }
        strcat(fromenv, ".library");
        name = fromenv;
    }

    bug("[openmesa] opening '%s'\n", name);
    lib = OpenLibrary((CONST_STRPTR)name, 0);
    bug("[openmesa] OpenLibrary returned 0x%p\n", lib);

    if (lib)
    {
        bug("[openmesa] '%s' version %d.%d\n", name,
            lib->lib_Version, lib->lib_Revision);
        CloseLibrary(lib);
        bug("[openmesa] closed\n");
    }

    return RETURN_OK;
}
