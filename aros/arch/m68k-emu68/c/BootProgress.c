/* Publish late boot milestones to the Emu68 early-boot framebuffer UI. */

#include <aros/bootui.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <string.h>

int __nocommandline = 1;

int main(void)
{
    IPTR args[1] = { 0 };
    struct RDArgs *rda;
    struct BootUIResource *resource;
    ULONG stage = 0;

    rda = ReadArgs("STAGE/A", args, NULL);
    if (!rda)
        return RETURN_FAIL;

    if (!strcmp((const char *)args[0], "DOS"))
        stage = BOOTUI_STAGE_DOS_READY;
    else if (!strcmp((const char *)args[0], "STARTUP"))
        stage = BOOTUI_STAGE_STARTUP;
    else if (!strcmp((const char *)args[0], "WANDERER"))
        stage = BOOTUI_STAGE_DESKTOP;
    /* Ends the display hold: the splash has been covering a desktop that was
     * being assembled behind it, and this says the desktop is worth looking
     * at. Nothing sends it during a normal boot yet -- the hold expires on its
     * own -- so this is here for whatever ends up in a position to. */
    else if (!strcmp((const char *)args[0], "ICONS"))
        stage = BOOTUI_STAGE_ICONS;

    resource = (struct BootUIResource *)OpenResource(BOOTUI_RESOURCE_NAME);
    if (stage && resource && resource->set_stage)
        resource->set_stage(stage);

    FreeArgs(rda);
    return stage ? RETURN_OK : RETURN_WARN;
}
