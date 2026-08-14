#ifndef AROS_EMU68_BOOTUI_H
#define AROS_EMU68_BOOTUI_H

#include <exec/nodes.h>
#include <exec/types.h>

#define BOOTUI_RESOURCE_NAME "bootui.resource"

#define BOOTUI_STAGE_DOS_READY 0x45303231UL
#define BOOTUI_STAGE_STARTUP   0x45303232UL
#define BOOTUI_STAGE_DESKTOP   0x45303233UL

struct BootUIResource
{
    struct Node node;
    void (*set_stage)(ULONG stage);
};

#endif
