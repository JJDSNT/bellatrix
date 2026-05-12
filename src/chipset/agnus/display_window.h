#ifndef BELLATRIX_AGNUS_DISPLAY_WINDOW_H
#define BELLATRIX_AGNUS_DISPLAY_WINDOW_H

#include "agnus.h"

typedef struct AgnusDisplayWindow
{
    int vstart;
    int vstop;
    int hstart;
    int hstop;
    int vheight;
    int hwidth;
} AgnusDisplayWindow;

static inline AgnusDisplayWindow agnus_get_display_window(const AgnusState *agnus)
{
    AgnusDisplayWindow w;

    w.vstart = (int)((agnus->diwstrt >> 8) & 0xFFu);
    w.vstop = (int)((agnus->diwstop >> 8) & 0xFFu);

    /*
     * Temporary OCS/ECS heuristic:
     * Workbench 1.3 commonly gives DIWSTRT=057e / DIWSTOP=40be.
     * Raw decode gives 5..64, which is visibly wrong.
     */
    if (w.vstop <= w.vstart)
        w.vstop += 256;

    w.hstart = (int)(agnus->diwstrt & 0xFFu);
    w.hstop = (int)(agnus->diwstop & 0xFFu);

    if (w.hstop <= w.hstart)
        w.hstop += 256;

    w.vheight = w.vstop - w.vstart;
    w.hwidth = w.hstop - w.hstart;

    return w;
}

#endif