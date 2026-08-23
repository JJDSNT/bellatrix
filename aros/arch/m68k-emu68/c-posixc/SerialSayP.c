/*
 * SerialSay again, linked the way a contrib program is.
 *
 * A control for ISSUE-0045. Markers added to glinfo -- through bug() and
 * through Printf() -- have never printed a single line, in any configuration,
 * including runs where the program demonstrably gets far enough to make the
 * display driver program a mode. C:SerialSay, using the same two calls on the
 * same machine, is heard.
 *
 * The only structural difference found between them is on our side: the
 * mmakefile for arch/m68k-emu68/c sets -noposixc, for BootProgress, and every
 * program in that directory inherits it. The contrib demos link the full C
 * runtime. So this one lives in its own directory with its own flags and links
 * posixc, and answers whether that linkage is what silences kprintf.
 *
 * If it is, no contrib program on this port can be instrumented, which blocks
 * every remaining question about the GL chain -- and is a bigger problem than
 * the GL chain.
 */

#include <aros/debug.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    bug("[serialsayp] via bug()\n");
    Printf("[serialsayp] via Printf()\n");
    Flush(Output());
    printf("[serialsayp] via printf()\n");
    fflush(stdout);
    return RETURN_OK;
}
