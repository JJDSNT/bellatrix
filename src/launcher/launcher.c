// src/launcher/launcher.c
// Boot-time coordinator for the Bellatrix launcher.

#include "launcher/launcher.h"
#include "launcher/launcher_ui.h"
#include "launcher/launcher_input.h"
#include "launcher/media_selection.h"
#include "io/hid/hid_router.h"
#include "host/pal.h"
#include <stdatomic.h>
#if BELLATRIX_ENABLE_BTSTACK
#include "launcher/btscan.h"
#include "io/bluetooth/bt_diag.h"
#endif

#if BELLATRIX_ENABLE_BTSTACK
void bellatrix_launcher_bt_close_pairing(void);
int  bellatrix_launcher_bt_connect_pairs(void);
#endif

typedef enum LauncherRuntimeModal {
    LAUNCHER_MODAL_NONE = 0,
    LAUNCHER_MODAL_BTSCAN,
    LAUNCHER_MODAL_MEDIA,
} LauncherRuntimeModal;

static _Atomic uint32_t s_runtime_modal;
static bool s_runtime_stepping;

bool launcher_runtime_modal_active(void)
{
    return atomic_load_explicit(&s_runtime_modal, memory_order_acquire) !=
           LAUNCHER_MODAL_NONE;
}

static void launcher_runtime_finish(void)
{
    launcher_input_set_active(false);
    atomic_store_explicit(&s_runtime_modal, LAUNCHER_MODAL_NONE,
                          memory_order_release);
    kprintf("[LAUNCHER] runtime modal closed\n");
}

static void launcher_runtime_step_impl(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    btscan_runtime_background_step();
#endif
    uint32_t modal = atomic_load_explicit(&s_runtime_modal,
                                          memory_order_relaxed);
    if (modal == LAUNCHER_MODAL_BTSCAN) {
#if BELLATRIX_ENABLE_BTSTACK
        if (!btscan_runtime_step())
            launcher_runtime_finish();
#else
        launcher_runtime_finish();
#endif
        return;
    }

    if (modal == LAUNCHER_MODAL_MEDIA) {
        if (!media_selection_runtime_step())
            launcher_runtime_finish();
        return;
    }

    HIDHostAction action = hid_router_take_host_action();
    if (action == HID_HOST_ACTION_NONE)
        return;

    while (launcher_input_pop() != 0u) {}
    launcher_input_set_active(true);

    if (action == HID_HOST_ACTION_BTSCAN) {
#if BELLATRIX_ENABLE_BTSTACK
        if (btscan_runtime_open()) {
            atomic_store_explicit(&s_runtime_modal, LAUNCHER_MODAL_BTSCAN,
                                  memory_order_release);
            kprintf("[LAUNCHER] F11 opened Bluetooth scan\n");
            return;
        }
#endif
        kprintf("[LAUNCHER] F11 unavailable\n");
    } else if (action == HID_HOST_ACTION_MEDIA) {
        if (media_selection_runtime_open()) {
            atomic_store_explicit(&s_runtime_modal, LAUNCHER_MODAL_MEDIA,
                                  memory_order_release);
            kprintf("[LAUNCHER] F12 opened media selection\n");
            return;
        }
        kprintf("[LAUNCHER] F12 unavailable\n");
    }

    launcher_input_set_active(false);
}

void launcher_runtime_step(void)
{
    /* Boot-time synchronous helpers can nest the host reactor. Never recurse
     * into the modal state machine from that nested service point. */
    if (s_runtime_stepping)
        return;
    s_runtime_stepping = true;
    launcher_runtime_step_impl();
    s_runtime_stepping = false;
}

bool launcher_run(void)
{
    if (!framebuffer || fb_width == 0u || fb_height == 0u) {
        kprintf("[LAUNCHER] framebuffer not ready\n");
        return false;
    }

    ui_init_metrics();
    launcher_input_init();
    atomic_store_explicit(&s_runtime_modal, LAUNCHER_MODAL_NONE,
                          memory_order_release);
    s_runtime_stepping = false;
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
