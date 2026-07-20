/*
 * Bluetooth control surface for consumers that do not own the stack.
 * See bt_session.h for why this layer exists.
 *
 * Moved here from src/cpu/emu68/bellatrix.c (2026-07-20), where it had
 * accumulated as bellatrix_launcher_bt_* inside the Emu68 CPU adapter.
 */

#include "io/bluetooth/bt_session.h"

#include "runtime/runtime.h"

#if BELLATRIX_ENABLE_BTSTACK
#include "io/bluetooth/bt_host.h"
#endif

void bt_session_poll(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_step(&g_runtime.bluetooth);
#endif
}

int bt_session_ready(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    return bt_host_is_working(&g_runtime.bluetooth) ? 1 : 0;
#else
    return 0;
#endif
}

int bt_session_connect_pairs(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_connect_pairs(&g_runtime.bluetooth);
    return bt_host_mouse_connected() ? 1 : 0;
#else
    return 0;
#endif
}

void bt_session_connect_now(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_connect_pairs(&g_runtime.bluetooth);
#endif
}

int bt_session_mouse_connected(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    return bt_host_mouse_connected() ? 1 : 0;
#else
    return 0;
#endif
}

void bt_session_suspend_reconnect(int suspended)
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_set_outgoing_reconnect_suspended(&g_runtime.bluetooth,
                                             suspended != 0);
#else
    (void)suspended;
#endif
}

int bt_session_recovery_discovery_active(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    return bt_host_recovery_discovery_active(&g_runtime.bluetooth) ? 1 : 0;
#else
    return 0;
#endif
}

void bt_session_claim_recovery_discovery(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_claim_recovery_discovery(&g_runtime.bluetooth);
#endif
}

void bt_session_prepare_pairing(const uint8_t addr[6])
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_prepare_explicit_pairing(&g_runtime.bluetooth, addr);
#else
    (void)addr;
#endif
}

void bt_session_close_pairing(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_close_pairing_window(&g_runtime.bluetooth);
#endif
}

void bt_session_open_pairing(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_open_pairing_window(&g_runtime.bluetooth);
#endif
}
