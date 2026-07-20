// src/launcher/launcher.c
// Boot-time coordinator for the Bellatrix launcher.

#include "launcher/launcher.h"
#include "launcher/launcher_ui.h"
#include "launcher/launcher_input.h"
#include "launcher/media_selection.h"
#include "io/hid/hid_router.h"
#include "host/pal.h"
#include "runtime/runtime.h"
#include <stdatomic.h>
#if BELLATRIX_ENABLE_USBSTACK
#include "io/usb/usb_msc_bellatrix.h"
#endif
#if BELLATRIX_ENABLE_BTSTACK
#include "launcher/btscan.h"
#include "io/bluetooth/bt_diag.h"
#include "io/bluetooth/bt_session.h"
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
        atomic_store_explicit(&s_runtime_modal, LAUNCHER_MODAL_BTSCAN,
                              memory_order_release);
        if (btscan_runtime_open()) {
            kprintf("[LAUNCHER] F11 opened Bluetooth scan\n");
            return;
        }
        atomic_store_explicit(&s_runtime_modal, LAUNCHER_MODAL_NONE,
                              memory_order_release);
#endif
        kprintf("[LAUNCHER] F11 unavailable\n");
    } else if (action == HID_HOST_ACTION_MEDIA) {
        atomic_store_explicit(&s_runtime_modal, LAUNCHER_MODAL_MEDIA,
                              memory_order_release);
        if (media_selection_runtime_open()) {
            kprintf("[LAUNCHER] F12 opened media selection\n");
            return;
        }
        atomic_store_explicit(&s_runtime_modal, LAUNCHER_MODAL_NONE,
                              memory_order_release);
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

    /* Give an attached drive the same enumeration budget the media selector
     * historically used, but do it before touching the framebuffer.  With no
     * drive present the normal boot therefore contains no launcher screen. */
    uint64_t t_enum = PAL_Time_ReadCounter();
#if BELLATRIX_ENABLE_USBSTACK
    while (!usb_msc_is_ready() && launcher_ms_since(t_enum) < 5000u)
        bellatrix_runtime_io_pump();
#else
    while (launcher_ms_since(t_enum) < 300u)
        bellatrix_runtime_io_pump();
#endif

#if BELLATRIX_ENABLE_BTSTACK
    btscan_screen(false);
    if (!btscan_has_saved_mouse()) {
        bt_diag_log("[BT] no saved mouse; enter pairing recovery scan\n");
        btscan_screen(true);
    }
    bt_session_close_pairing();
    if (!bt_session_connect_pairs())
        bt_diag_log("[BT] passive HID reconnect armed; launcher continues\n");
    launcher_save_bt_report();
#endif

#if BELLATRIX_ENABLE_USBSTACK
    if (!usb_msc_is_ready() && !media_selection_qemu_media_present()) {
        kprintf("[LAUNCHER] no USB media; skipping boot-time launcher\n");
        launcher_input_set_active(false);
        return false;
    }
#else
    if (!media_selection_qemu_media_present()) {
        launcher_input_set_active(false);
        return false;
    }
#endif

    return media_selection_run();
}
