#ifndef AROS_EMU68_SCANOUT_H
#define AROS_EMU68_SCANOUT_H

#include <exec/nodes.h>
#include <exec/types.h>

/*
 * Who owns the scanout, and where it is.
 *
 * A display driver that programs a mode moves the surface the machine is
 * scanning out -- new address, new pitch, and on this port 16bpp becomes
 * 32bpp. Anything else that was drawing there has to find out, and the two
 * cannot be introduced to each other: a driver that knows a boot splash
 * exists has to be revisited whenever the splash changes, and a splash that
 * knows about drivers has to be revisited whenever a driver is added.
 *
 * So the driver states where the scanout is, and whoever cares reads it. That
 * is the display-ownership facility rather than a message between two
 * particular components, and it is a resource because a driver installed from
 * DEVS:Drivers can reach a resource and cannot reach a kernel symbol -- which
 * is not a detail, it is what makes the direct call impossible rather than
 * merely wrong.
 *
 * Absent resource, or absent publisher, means nobody has changed the surface
 * since the bootloader handed it over.
 */

#define SCANOUT_RESOURCE_NAME "scanout.resource"

struct ScanoutResource
{
    struct Node node;

    /* Called by whoever has just programmed a mode. */
    void (*publish)(APTR framebuffer, ULONG pitch, ULONG width,
                    ULONG height, ULONG depth);

    /* Fills in the surface last published; returns FALSE if there is none. */
    BOOL (*current)(APTR *framebuffer, ULONG *pitch, ULONG *width,
                    ULONG *height, ULONG *depth);
};

#endif /* AROS_EMU68_SCANOUT_H */
