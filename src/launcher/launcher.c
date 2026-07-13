// src/launcher/launcher.c
// Boot-time coordinator for the Bellatrix launcher.

#include "launcher/launcher.h"
#include "launcher/launcher_ui.h"
#include "launcher/launcher_input.h"
#include "launcher/media_selection.h"
#include "host/pal.h"
#if BELLATRIX_ENABLE_BTSTACK
#include "launcher/btscan.h"
#include "io/bluetooth/bt_diag.h"
#endif

#if BELLATRIX_ENABLE_BTSTACK
void bellatrix_launcher_bt_close_pairing(void);
int  bellatrix_launcher_bt_connect_pairs(void);
#endif

bool launcher_run(void)
{
    if (!framebuffer || fb_width == 0u || fb_height == 0u) {
        kprintf("[LAUNCHER] framebuffer not ready\n");
        return false;
    }

    ui_init_metrics();
    launcher_input_init();
    launcher_input_set_active(true);

    draw_message("Initialising...", COL_TITLE_BG);
    {
        uint64_t t_kick = PAL_Time_ReadCounter();
        while (launcher_ms_since(t_kick) < 300u)
            pump_usb();
    }

#if BELLATRIX_ENABLE_BTSTACK
    btscan_screen(false);
    if (!btscan_has_saved_mouse()) {
        bt_diag_log("[BT] no saved mouse; enter pairing recovery scan\n");
        btscan_screen(true);
    }
    bellatrix_launcher_bt_close_pairing();
    if (!bellatrix_launcher_bt_connect_pairs())
        bt_diag_log("[BT] passive HID reconnect armed; launcher continues\n");
    launcher_save_bt_report();
#endif

    return media_selection_run();
}
