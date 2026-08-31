/*
 * Say one line, two ways, from a shell program.
 *
 * A control for ISSUE-0045. Instrumentation added to glinfo produced nothing
 * at all, and silence from a marker cannot be told from a program that never
 * reached it -- so before reading that silence as evidence, establish that a
 * shell program on this machine can be heard at all, by both routes the
 * instrumentation uses:
 *
 *   bug()    the raw serial line, owing nothing to DOS
 *   Printf() this program's own output, which the caller redirects to DEBUG:
 *
 * If both arrive here and neither arrives from glinfo, glinfo does not reach
 * its main(). If neither arrives here, the instrument is the problem and every
 * reading taken with it is worthless.
 */

#include <aros/debug.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

int __nocommandline = 1;

int main(void)
{
    bug("[serialsay] via bug()\n");
    Printf("[serialsay] via Printf()\n");
    Flush(Output());
    return RETURN_OK;
}
