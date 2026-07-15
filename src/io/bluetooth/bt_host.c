#include "io/bluetooth/bt_host.h"
#include "io/bluetooth/bt_diag.h"
#include "io/bluetooth/bt_scan.h"
#include "io/bluetooth/bt_pairs.h"
#include "io/bluetooth/bt_hid.h"
#include "debug/core_log.h"
#include "support.h"

#if BELLATRIX_ENABLE_BTSTACK
#include "hal_time_ms.h"
#include "btstack.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_uart_slip_wrapper.h"
#include "btstack_run_loop.h"
#include "btstack_memory.h"
#include "btstack_run_loop_embedded.h"
#include "btstack_chipset_bcm.h"
#include "btstack_chipset_bcm_download_firmware.h"
#include "hci_transport_h4.h"
#include "btstack_uart_block.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "hci_transport.h"
#include "l2cap.h"
#include "classic/hid_host.h"
#include "classic/sdp_server.h"
#include "io/bluetooth/bt_link_key_db_sd.h"
#include "mmu.h"
#include "host/raspi3/vc_mailbox.h"
#include "host/raspi3/physical_interrupts.h"
#include "io/bluetooth/bt_hal_raspi3.h"

// HAL declarations
void bt_hal_raspi3_poll_uart(void);
uint32_t bt_hal_raspi3_io_activity(void);
void bt_hal_raspi3_rx_pending(uint32_t *filled, uint32_t *wanted);
void bt_hal_raspi3_flush_rx(void);
void bt_hal_raspi3_trace_dump(void);
void bt_hal_raspi3_trace_reset(void);
const btstack_uart_block_t * btstack_uart_block_embedded_instance(void);
bool pl011_backend_route_bluetooth_pi3(void);
void pl011_backend_wait_idle(void);
uint32_t pl011_backend_setup_bt_lpo(uint32_t *old_ctl, uint32_t *old_div);
uint32_t pl011_backend_flag_register(void);
uint32_t pl011_backend_irq_mask(void);

void btstack_chipset_bcm_set_device_name(const char * device_name) {
    (void)device_name;
}

extern const int brcm_patch_ram_length;
extern const char brcm_patch_version[];

/* Main transport: H4 at 115200, hardware RTS/CTS at the PL011 level.
 * BCM4343x ships H4 in every proven config (Linux hciattach, Circle,
 * Ultibo); its H5 path wedged on the first radio command (HCI Inquiry
 * acked nothing, retransmitted forever).  115200 is plenty for HID. */
static const hci_transport_config_uart_t bt_transport_config = {
    HCI_TRANSPORT_CONFIG_UART,
    115200,
    115200,
    BTSTACK_UART_FLOWCONTROL_OFF,
    NULL,
    BTSTACK_UART_PARITY_OFF,
};

static const btstack_uart_config_t bt_phase1_uart_config = {
    115200,
    BTSTACK_UART_FLOWCONTROL_OFF,
    NULL,
    BTSTACK_UART_PARITY_OFF,
};

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_timer_source_t bt_pairing_window_timer;
static BTHost *s_bt_host = NULL;

/* HID host descriptor storage — used for report-protocol devices.
 * In boot protocol mode this is largely unused, but hid_host_init() requires it. */
#define BT_HID_DESCRIPTOR_STORAGE_SIZE 512u
static uint8_t s_hid_descriptor_storage[BT_HID_DESCRIPTOR_STORAGE_SIZE];

/* cid → device-type table for routing HID reports in hid_packet_handler */
#define BT_HID_CID_TABLE_MAX 4u
typedef struct {
    uint16_t cid;
    uint8_t dtype;
    uint8_t protocol_mode;
    bool descriptor_available;
} BTHIDCIDEntry;
static BTHIDCIDEntry s_hid_cid_table[BT_HID_CID_TABLE_MAX];
static unsigned      s_hid_cid_count;

static void hid_cid_register(uint16_t cid, uint8_t dtype)
{
    if (s_hid_cid_count >= BT_HID_CID_TABLE_MAX) return;
    s_hid_cid_table[s_hid_cid_count].cid   = cid;
    s_hid_cid_table[s_hid_cid_count].dtype = dtype;
    s_hid_cid_table[s_hid_cid_count].protocol_mode = HID_PROTOCOL_MODE_BOOT;
    s_hid_cid_table[s_hid_cid_count].descriptor_available = false;
    s_hid_cid_count++;
}
static BTHIDCIDEntry *hid_cid_entry(uint16_t cid)
{
    for (unsigned i = 0u; i < s_hid_cid_count; i++)
        if (s_hid_cid_table[i].cid == cid) return &s_hid_cid_table[i];
    return NULL;
}
static uint8_t hid_cid_type(uint16_t cid)
{
    BTHIDCIDEntry *entry = hid_cid_entry(cid);
    return entry ? entry->dtype : BT_PAIRS_TYPE_UNKNOWN;
}
static void hid_cid_unregister(uint16_t cid)
{
    for (unsigned i = 0u; i < s_hid_cid_count; i++) {
        if (s_hid_cid_table[i].cid != cid) continue;
        s_hid_cid_table[i] = s_hid_cid_table[--s_hid_cid_count];
        return;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void hid_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void bt_pairing_window_close(BTHost *bt);
static uint32_t bt_now_ms(void);
static void bt_phase2_start(int status);
static void bt_setup_hci_main(BTHost *bt);
static void bt_hardware_error_callback(uint8_t error_code);
static void bt_connection_step(BTHost *bt);

static const char *bt_connection_state_name(uint8_t state)
{
    switch (state) {
        case BT_CONNECTION_WAIT_STACK:  return "wait-stack";
        case BT_CONNECTION_PASSIVE:     return "passive";
        case BT_CONNECTION_CONNECTING:  return "connecting";
        case BT_CONNECTION_DISCOVERING: return "discovering";
        case BT_CONNECTION_ACTIVE:      return "active";
        case BT_CONNECTION_BACKOFF:     return "backoff";
        default:                        return "invalid";
    }
}

static void bt_connection_set(BTHost *bt, uint8_t state, uint32_t delay_ms)
{
    if (!bt)
        return;
    if (bt->connection_state != state)
        bt_diag_log("[BT-CM] %s -> %s\n",
                    bt_connection_state_name(bt->connection_state),
                    bt_connection_state_name(state));
    bt->connection_state = state;
    bt->connection_deadline_ms = delay_ms ? bt_now_ms() + delay_ms : 0u;
}

/* kprintf already lives permanently on the mini-UART by the time BT exists
 * (bellatrix_early_console_init(), called at the very start of boot) — PL011
 * is BT's alone, no runtime handoff needed here. See
 * AI_context/issue_logging_miniuart.md. */

#define BELLATRIX_BT_PAIRING_WINDOW_MS 120000u
/* Phase 1 alone pushes ~36KB of PatchRAM at 115200 baud (≈3.2s on the wire),
 * then H5 + HCI bring-up follow.  These are real seconds now that
 * hal_time_ms() is monotonic (the legacy-timer byte-swap fix in time.c). */
#define BELLATRIX_BT_BOOTSTRAP_WAIT_MS 20000u
#define BELLATRIX_BT_REG_EN_GPIO 128u
#define BELLATRIX_BT_REG_EN_ASSERT_MS 100u
#define BELLATRIX_BT_BOOT_ROM_SETTLE_MS 250u
#define BELLATRIX_BT_INIT_TIMEOUT_MS 15000u
#define BELLATRIX_BT_MAX_POWER_CYCLE_ATTEMPTS 2u
#define BELLATRIX_BT_PAIRING_DIRECT_RETRIES 6u
#define BELLATRIX_BT_PAIRING_RETRY_MS 5000u
#define VC_FIRMWARE_STATUS_REQUEST 0u
#define VC_FIRMWARE_STATUS_SUCCESS 0x80000000u
#define VC_FIRMWARE_PROPERTY_END 0u
#define VC_FIRMWARE_SET_GPIO_STATE 0x00038041u

typedef enum BTBootstrapState {
    BT_BOOTSTRAP_IDLE = 0,
    BT_BOOTSTRAP_RESET_ASSERTED,
    BT_BOOTSTRAP_WAIT_FOR_BOOT_ROM,
    BT_BOOTSTRAP_WAIT_FOR_PHASE1,
    BT_BOOTSTRAP_WAIT_FOR_WORKING,
    BT_BOOTSTRAP_WORKING,
    BT_BOOTSTRAP_FAILED
} BTBootstrapState;

typedef enum BTLinkRecoveryState {
    BT_LINK_RECOVERY_IDLE = 0,
    BT_LINK_RECOVERY_REQUESTED,
    BT_LINK_RECOVERY_FORCE_INITIALIZING,
    BT_LINK_RECOVERY_FORCE_OFF,
    BT_LINK_RECOVERY_PHYSICAL_RESET,
} BTLinkRecoveryState;

typedef enum BTLinkRecoveryReason {
    BT_LINK_RECOVERY_REASON_NONE = 0,
    BT_LINK_RECOVERY_REASON_HARDWARE_ERROR,
    BT_LINK_RECOVERY_REASON_TRANSPORT_ERROR,
    BT_LINK_RECOVERY_REASON_CONTROLLER_SILENCE,
    BT_LINK_RECOVERY_REASON_H4_DESYNC,
    BT_LINK_RECOVERY_REASON_CONTROLLER_MUTE,
} BTLinkRecoveryReason;

static uint32_t s_link_recoveries;

#define VC_MBOX_CH_PROP 8u

static uint32_t vc_property_buffer[8] __attribute__((aligned(16)));

static uint32_t bt_now_ms(void)
{
    return hal_time_ms();
}

static bool vc_set_gpio_state(uint32_t gpio, uint32_t state)
{
    vc_property_buffer[0] = LE32(sizeof(vc_property_buffer));
    vc_property_buffer[1] = LE32(VC_FIRMWARE_STATUS_REQUEST);
    vc_property_buffer[2] = LE32(VC_FIRMWARE_SET_GPIO_STATE);
    vc_property_buffer[3] = LE32(8);
    vc_property_buffer[4] = 0;
    vc_property_buffer[5] = LE32(gpio);
    vc_property_buffer[6] = LE32(state ? 1u : 0u);
    vc_property_buffer[7] = LE32(VC_FIRMWARE_PROPERTY_END);

    arm_flush_cache((uintptr_t)vc_property_buffer, sizeof(vc_property_buffer));
    vc_mbox_send(VC_MBOX_CH_PROP, (uint32_t)mmu_virt2phys((uintptr_t)vc_property_buffer));
    if (!vc_mbox_recv(VC_MBOX_CH_PROP, NULL)) {
        return false;
    }
    arm_dcache_invalidate((uintptr_t)vc_property_buffer, sizeof(vc_property_buffer));

    return LE32(vc_property_buffer[1]) == VC_FIRMWARE_STATUS_SUCCESS;
}

static void bt_begin_power_cycle(BTHost *bt, const char *reason)
{
    uint32_t now;
    bool gpio_ok;

    if (!bt) {
        return;
    }

    now = bt_now_ms();
    bt->power_cycle_attempts++;
    gpio_ok = vc_set_gpio_state(BELLATRIX_BT_REG_EN_GPIO, 0);

    if (gpio_ok) {
        bt->bootstrap_state = BT_BOOTSTRAP_RESET_ASSERTED;
        bt->bootstrap_deadline_ms = now + BELLATRIX_BT_REG_EN_ASSERT_MS;
        bt->init_deadline_ms = 0;
        bt_diag_log("[BT] controller reset asserted via BT_REG_EN (attempt %u, %s)\n",
                (unsigned)bt->power_cycle_attempts, reason);
        return;
    }

    bt->bootstrap_state = BT_BOOTSTRAP_WAIT_FOR_BOOT_ROM;
    bt->bootstrap_deadline_ms = now + BELLATRIX_BT_BOOT_ROM_SETTLE_MS;
    bt->init_deadline_ms = 0;
    bt_diag_log("[BT] BT_REG_EN mailbox failed, falling back to warm boot (attempt %u, %s)\n",
            (unsigned)bt->power_cycle_attempts, reason);
}

static void bt_schedule_power_on(BTHost *bt)
{
    if (!bt) {
        return;
    }

    if (!bt->hci_ready) {
        bt->bootstrap_state = BT_BOOTSTRAP_FAILED;
        bt->init_deadline_ms = 0;
        bt_diag_log("[BT] power on requested before HCI main setup completed\n");
        return;
    }

    if (hci_power_control(HCI_POWER_ON) != 0) {
        bt->bootstrap_state = BT_BOOTSTRAP_FAILED;
        bt->init_deadline_ms = 0;
        bt_diag_log("[BT] power on request failed\n");
        return;
    }

    bt->bootstrap_state = BT_BOOTSTRAP_WAIT_FOR_WORKING;
    bt->init_deadline_ms = bt_now_ms() + BELLATRIX_BT_INIT_TIMEOUT_MS;
    bt_diag_log("[BT] init OK, power on requested\n");
}

static void bt_bootstrap_step(BTHost *bt)
{
    uint32_t now;

    if (!bt) {
        return;
    }

    now = bt_now_ms();

    switch ((BTBootstrapState)bt->bootstrap_state) {
        case BT_BOOTSTRAP_RESET_ASSERTED:
            if ((int32_t)(now - bt->bootstrap_deadline_ms) < 0) {
                return;
            }

            bt_diag_log("[BT] releasing BT_REG_EN (now=%u)\n", (unsigned)now);
            if (!vc_set_gpio_state(BELLATRIX_BT_REG_EN_GPIO, 1)) {
                bt_diag_log("[BT] failed to release BT_REG_EN, proceeding with warm boot\n");
            } else {
                bt_diag_log("[BT] controller reset released\n");
            }

            bt->bootstrap_state = BT_BOOTSTRAP_WAIT_FOR_BOOT_ROM;
            bt->bootstrap_deadline_ms = now + BELLATRIX_BT_BOOT_ROM_SETTLE_MS;
            return;

        case BT_BOOTSTRAP_WAIT_FOR_BOOT_ROM:
            if ((int32_t)(now - bt->bootstrap_deadline_ms) < 0) {
                return;
            }

            if (!bt->phase1_complete) {
                const btstack_uart_t *uart_phase1;

                bt_diag_log("[BT] controller boot ROM settle complete, starting BCM phase 1 over H4\n");
                bt_hal_raspi3_trace_reset();
                pl011_backend_wait_idle();
                pl011_backend_route_bluetooth_pi3();
                bt->bootstrap_state = BT_BOOTSTRAP_WAIT_FOR_PHASE1;
                bt->init_deadline_ms = now + BELLATRIX_BT_INIT_TIMEOUT_MS;
                uart_phase1 = (const btstack_uart_t *)btstack_uart_block_embedded_instance();
                uart_phase1->init(&bt_phase1_uart_config);
                btstack_chipset_bcm_download_firmware(
                    btstack_uart_block_embedded_instance(),
                    (int)bt_transport_config.baudrate_main,
                    bt_phase2_start);
                return;
            }

            bt_diag_log("[BT] controller boot ROM settle complete, starting HCI bring-up\n");
            bt_schedule_power_on(bt);
            return;

        case BT_BOOTSTRAP_WAIT_FOR_PHASE1: {
            /* The PatchRAM upload (~141 records at 115200 baud) plus the
             * post-minidriver reboot pause can outlast a fixed deadline.
             * Slide the deadline while UART bytes keep moving: the timeout
             * then means "link dead", not "upload slow". */
            static uint32_t last_phase1_activity;
            uint32_t activity = bt_hal_raspi3_io_activity();
            if (activity != last_phase1_activity) {
                last_phase1_activity = activity;
                bt->init_deadline_ms = now + BELLATRIX_BT_INIT_TIMEOUT_MS;
            }
            if ((bt->init_deadline_ms != 0u) &&
                ((int32_t)(now - bt->init_deadline_ms) >= 0)) {
                bt->bootstrap_state = BT_BOOTSTRAP_FAILED;
                bt->init_deadline_ms = 0;
                bt_hal_raspi3_trace_dump();
                bt_diag_log("[BT] BCM phase 1 timed out before H5 startup\n");
            }
            return;
        }

        case BT_BOOTSTRAP_WAIT_FOR_WORKING:
            if ((bt->init_deadline_ms != 0u) &&
                ((int32_t)(now - bt->init_deadline_ms) >= 0)) {
                if (bt->power_cycle_attempts < BELLATRIX_BT_MAX_POWER_CYCLE_ATTEMPTS) {
                    bt_diag_log("[BT] still stuck in initializing after %u ms, retrying bring-up\n",
                            (unsigned)BELLATRIX_BT_INIT_TIMEOUT_MS);
                    hci_power_control(HCI_POWER_OFF);
                    bt_hal_raspi3_flush_rx();
                    bt_pairing_window_close(bt);
                    bt->phase1_complete = false;
                    bt_begin_power_cycle(bt, "retry after initializing timeout");
                } else {
                    bt->bootstrap_state = BT_BOOTSTRAP_FAILED;
                    bt->init_deadline_ms = 0;
                    bt_diag_log("[BT] initializing timeout after %u attempts; controller did not reach WORKING\n",
                            (unsigned)bt->power_cycle_attempts);
                }
            }
            return;

        default:
            return;
    }
}

static void bt_setup_hci_main(BTHost *bt)
{
    const btstack_chipset_t *chipset;
    const btstack_uart_t *uart_driver;
    const hci_transport_t *transport;
    const btstack_uart_block_t *uart_block;

    if (!bt || bt->hci_ready) {
        return;
    }

    uart_block = btstack_uart_block_embedded_instance();
    if (uart_block->close) {
        uart_block->close();
    }

    chipset = btstack_chipset_bcm_instance();
    chipset->init(&bt_transport_config);

    uart_driver = (const btstack_uart_t *) btstack_uart_block_embedded_instance();
    transport = hci_transport_h4_instance_for_uart(uart_driver);

    hci_init(transport, &bt_transport_config);
    hci_set_chipset(chipset);
    /* RSSI mode (not EIR): EIR results are up to 255 bytes per device and
     * the BCM43430A1 firmware wedges mid-delivery when two consecutive large
     * EIR events arrive (rxq=165/233 stall pattern seen with 3+ nearby
     * devices). RSSI mode uses fixed-size 14-byte per-device records, which
     * never overflow. Device names are lost, but CoD labels cover gamepads,
     * TVs, etc. Remote Name Request crashes the firmware anyway. */
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI);
    hci_set_hardware_error_callback(&bt_hardware_error_callback);
    hci_set_link_key_db(bt_link_key_db_sd_instance());

    l2cap_init();
    sdp_init();
    hid_host_init(s_hid_descriptor_storage, BT_HID_DESCRIPTOR_STORAGE_SIZE);
    hid_host_register_packet_handler(hid_packet_handler);

    bt_hid_init();
    s_hid_cid_count = 0u;

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    bt->hci_ready = true;
}

static void bt_phase2_start(int status)
{
    if (!s_bt_host) {
        return;
    }

    if (status != 0) {
        s_bt_host->bootstrap_state = BT_BOOTSTRAP_FAILED;
        s_bt_host->init_deadline_ms = 0;
        bt_hal_raspi3_trace_dump();
        bt_diag_log("[BT] BCM phase 1 failed with status=%d\n", status);
        return;
    }

    s_bt_host->phase1_complete = true;
    s_bt_host->init_deadline_ms = 0;
    bt_diag_log("[BT] BCM phase 1 complete, switching to H4 main transport\n");
    bt_setup_hci_main(s_bt_host);
    /* Phase 1 (btstack_uart_block) and our drain_fifo both read the same
     * PL011 FIFO concurrently during the bootstrap wait loop.  drain_fifo
     * wins some bytes that phase 1 never sees; those bytes accumulate in
     * our ring.  If left there they are fed to the H4 parser as the first
     * bytes of the real HCI init, producing a large garbage "packet" —
     * seen as rxq=251/252 stall right after phase 1 on every recovery.
     * Flushing here, before hci_power_control(ON), gives H4 a clean start. */
    bt_hal_raspi3_flush_rx();
    bt_schedule_power_on(s_bt_host);
}

static bool bt_bootstrap_is_terminal(const BTHost *bt)
{
    if (!bt) {
        return true;
    }

    switch ((BTBootstrapState)bt->bootstrap_state) {
        case BT_BOOTSTRAP_WORKING:
        case BT_BOOTSTRAP_FAILED:
            return true;
        default:
            return false;
    }
}

static void bt_pairing_window_close(BTHost *bt)
{
    if (!bt || !bt->pairing_window_open) {
        return;
    }

    gap_discoverable_control(0);
    bt->pairing_window_open = false;
    kprintf("[BT] pairing window closed\n");
}

static void bt_pairing_window_timeout(btstack_timer_source_t *ts)
{
    BTHost *bt = (BTHost *)btstack_run_loop_get_timer_context(ts);
    bt_pairing_window_close(bt);
}

static void bt_pairing_window_open(BTHost *bt)
{
    if (!bt || bt->pairing_window_open) {
        return;
    }

    gap_discoverable_control(1);
    bt->pairing_window_open = true;
    btstack_run_loop_set_timer(&bt_pairing_window_timer, bt->pairing_window_ms);
    btstack_run_loop_add_timer(&bt_pairing_window_timer);
    kprintf("[BT] pairing window open for %u ms\n", (unsigned)bt->pairing_window_ms);
}

void bt_host_close_pairing_window(BTHost *bt)
{
    bt_pairing_window_close(bt);
}

void bt_host_open_pairing_window(BTHost *bt)
{
    bt_pairing_window_open(bt);
}

void bt_host_set_outgoing_reconnect_suspended(BTHost *bt, bool suspended)
{
    if (!bt)
        return;
    bt->outgoing_reconnect_suspended = suspended;
    bt_diag_log("[BT-CM] outgoing reconnect %s\n",
                suspended ? "suspended" : "resumed");
}

void bt_host_prepare_explicit_pairing(BTHost *bt, const uint8_t addr[6])
{
    bd_addr_t pairing_addr;
    if (!bt || !addr)
        return;
    memcpy(pairing_addr, addr, sizeof(pairing_addr));
    gap_drop_link_key_for_bd_addr(pairing_addr);
    bt_pairing_window_open(bt);
    bt_diag_log("[BT] explicit pairing: old key removed for %s\n",
                bd_addr_to_str(pairing_addr));
}

bool bt_host_mouse_connected(void)
{
    return s_bt_host && s_bt_host->mouse_connected;
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    bd_addr_t event_addr;

    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_POWERON_FAILED:
            bt_diag_log("[BT] power on failed\n");
            break;
        case BTSTACK_EVENT_STATE:
            switch (btstack_event_state_get_state(packet)) {
                case HCI_STATE_WORKING: {
                    bd_addr_t local_addr;
                    gap_local_bd_addr(local_addr);
                    if (s_bt_host) {
                        s_bt_host->bootstrap_state = BT_BOOTSTRAP_WORKING;
                        s_bt_host->init_deadline_ms = 0;
                    }
                    bt_diag_log("[BT] stack up and running, BD_ADDR=%s\n", bd_addr_to_str(local_addr));
                    gap_connectable_control(1);
                    if (s_bt_host)
                        s_bt_host->recovery_direct_attempts = 0u;
                    bt_connection_set(s_bt_host, BT_CONNECTION_PASSIVE,
                                      2000u);
                    break;
                }
                case HCI_STATE_INITIALIZING:
                    if (s_bt_host && s_bt_host->init_deadline_ms == 0u) {
                        s_bt_host->bootstrap_state = BT_BOOTSTRAP_WAIT_FOR_WORKING;
                        s_bt_host->init_deadline_ms = bt_now_ms() + BELLATRIX_BT_INIT_TIMEOUT_MS;
                    }
                    bt_diag_log("[BT] state=initializing\n");
                    break;
                case HCI_STATE_OFF:
                    if (s_bt_host && s_bt_host->bootstrap_state == BT_BOOTSTRAP_WORKING) {
                        s_bt_host->bootstrap_state = BT_BOOTSTRAP_IDLE;
                    }
                    bt_pairing_window_close(s_bt_host);
                    bt_diag_log("[BT] state=off\n");
                    break;
                case HCI_STATE_HALTING:
                    bt_pairing_window_close(s_bt_host);
                    bt_diag_log("[BT] state=halting\n");
                    break;
                default:
                    break;
            }
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            if ((s_bt_host && s_bt_host->pairing_window_open) ||
                bt_pairs_is_known(event_addr)) {
                kprintf("[BT] pin code request from %s -> using 0000%s\n",
                        bd_addr_to_str(event_addr),
                        bt_pairs_is_known(event_addr) ? " (saved pair)" : "");
                hci_send_cmd(&hci_pin_code_request_reply, &event_addr, 4, "0000");
            } else {
                kprintf("[BT] pin code request from %s denied "
                        "(unknown device, pairing window closed)\n",
                        bd_addr_to_str(event_addr));
                hci_send_cmd(&hci_pin_code_request_negative_reply, &event_addr);
            }
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            if ((s_bt_host && s_bt_host->pairing_window_open) ||
                bt_pairs_is_known(event_addr)) {
                kprintf("[BT] user confirmation request from %s -> accepted%s\n",
                        bd_addr_to_str(event_addr),
                        bt_pairs_is_known(event_addr) ? " (saved pair)" : "");
                hci_send_cmd(&hci_user_confirmation_request_reply, &event_addr);
            } else {
                kprintf("[BT] user confirmation request from %s denied "
                        "(unknown device, pairing window closed)\n",
                        bd_addr_to_str(event_addr));
                hci_send_cmd(&hci_user_confirmation_request_negative_reply, &event_addr);
            }
            break;
        default:
            break;
    }
}

void bt_host_connect_pairs(BTHost *bt)
{
    const BTPair *selected = NULL;
    unsigned selected_index = 0u;

    if (!bt || !bt_host_is_working(bt) || bt->mouse_connected ||
        bt->hid_connect_pending)
        return;
    unsigned n = bt_pairs_count();
    if (n == 0u) {
        bt_diag_log("[BT] connect_pairs: no pairs saved\n");
        return;
    }

    /* BTstack's embedded SDP client has one query context. Select one saved
     * device, with mouse priority, rather than starting concurrent queries. */
    for (unsigned i = 0u; i < n; i++) {
        const BTPair *p = bt_pairs_get(i);
        if (!p)
            continue;
        if (!selected || p->device_type == BT_PAIRS_TYPE_MOUSE) {
            selected = p;
            selected_index = i;
        }
        if (p->device_type == BT_PAIRS_TYPE_MOUSE)
            break;
    }
    if (!selected)
        return;

    bd_addr_t addr;
    memcpy(addr, selected->addr, sizeof(addr));
    uint16_t cid = 0u;
    int ret = hid_host_connect(addr, HID_PROTOCOL_MODE_BOOT, &cid);
    bt->mouse_connected = false;
    bt->hid_connect_pending = (ret == ERROR_CODE_SUCCESS);
    bt->pending_hid_cid = bt->hid_connect_pending ? cid : 0u;
    bt->last_hid_status = (uint8_t)ret;
    if (ret == ERROR_CODE_SUCCESS)
        bt_connection_set(bt, BT_CONNECTION_CONNECTING, 15000u);
    else
        bt_connection_set(bt, BT_CONNECTION_BACKOFF, 5000u);
    bt_diag_log("[BT] connect priority pair[%u] %s type=%u "
                "-> cid=0x%04x ret=%d\n",
                selected_index, bd_addr_to_str(addr),
                (unsigned)selected->device_type, (unsigned)cid, ret);
}

static void hid_packet_handler(uint8_t packet_type, uint16_t channel,
                                uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) return;

    bd_addr_t addr;
    uint16_t hid_cid;
    uint8_t  status;

    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION:
            hid_cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            status = hid_subevent_incoming_connection_get_status(packet);
            bt_diag_log("[BT] HID incoming: cid=0x%04x status=0x%02x\n",
                        (unsigned)hid_cid, (unsigned)status);
            if (status == ERROR_CODE_SUCCESS) {
                status = hid_host_accept_connection(hid_cid,
                                                    HID_PROTOCOL_MODE_BOOT);
                if (status == ERROR_CODE_SUCCESS) {
                    if (s_bt_host) {
                        s_bt_host->hid_connect_pending = true;
                        s_bt_host->pending_hid_cid = hid_cid;
                        bt_connection_set(s_bt_host,
                                          BT_CONNECTION_CONNECTING, 15000u);
                    }
                    bt_diag_log("[BT] HID incoming accepted: cid=0x%04x "
                                "boot protocol\n", (unsigned)hid_cid);
                } else {
                    bt_diag_log("[BT] HID incoming accept failed: "
                                "cid=0x%04x status=0x%02x\n",
                                (unsigned)hid_cid, (unsigned)status);
                }
            }
            break;

        case HID_SUBEVENT_CONNECTION_OPENED:
            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            status  = hid_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                if (s_bt_host) {
                    s_bt_host->hid_connect_pending = false;
                    s_bt_host->pending_hid_cid = 0u;
                    s_bt_host->last_hid_status = status;
                }
                bt_diag_log("[BT] HID connect failed: cid=0x%04x "
                            "status=0x%02x\n",
                            (unsigned)hid_cid, (unsigned)status);

                if (s_bt_host && bt_pairs_count() != 0u &&
                    s_bt_host->recovery_direct_attempts <
                        BELLATRIX_BT_PAIRING_DIRECT_RETRIES) {
                    const BTPair *mouse = NULL;
                    for (unsigned i = 0u; i < bt_pairs_count(); i++) {
                        const BTPair *candidate = bt_pairs_get(i);
                        if (candidate &&
                            candidate->device_type == BT_PAIRS_TYPE_MOUSE) {
                            mouse = candidate;
                            break;
                        }
                    }
                    if (mouse) {
                        bool key_rejected =
                            status == ERROR_CODE_AUTHENTICATION_FAILURE ||
                            status == ERROR_CODE_PIN_OR_KEY_MISSING;
                        if (key_rejected) {
                            bd_addr_t known_addr;
                            memcpy(known_addr, mouse->addr,
                                   sizeof(known_addr));
                            gap_drop_link_key_for_bd_addr(known_addr);
                            bt_pairing_window_open(s_bt_host);
                            bt_diag_log("[BT-CM] rejected key 0x%02x dropped "
                                        "for %s; re-pair armed\n",
                                        (unsigned)status,
                                        bd_addr_to_str(known_addr));
                        }
                        s_bt_host->recovery_direct_attempts++;
                        bt_connection_set(s_bt_host, BT_CONNECTION_PASSIVE,
                                          BELLATRIX_BT_PAIRING_RETRY_MS);
                        break;
                    }
                }
                bt_connection_set(s_bt_host, BT_CONNECTION_DISCOVERING, 0u);
                break;
            }
            hid_subevent_connection_opened_get_bd_addr(packet, addr);
            {
                uint8_t dtype = BT_PAIRS_TYPE_UNKNOWN;
                for (unsigned i = 0u; i < bt_pairs_count(); i++) {
                    const BTPair *p = bt_pairs_get(i);
                    if (p && memcmp(p->addr, addr, 6u) == 0) {
                        dtype = p->device_type;
                        break;
                    }
                }
                hid_cid_register(hid_cid, dtype);
                if (s_bt_host) {
                    if (dtype == BT_PAIRS_TYPE_MOUSE)
                        s_bt_host->mouse_connected = true;
                    s_bt_host->hid_connect_pending = false;
                    s_bt_host->pending_hid_cid = 0u;
                    s_bt_host->recovery_direct_attempts = 0u;
                    s_bt_host->last_hid_status = ERROR_CODE_SUCCESS;
                    if (s_bt_host->discovery_started) {
                        bt_scan_stop();
                        s_bt_host->discovery_started = false;
                    }
                    bt_pairing_window_close(s_bt_host);
                }
                bt_connection_set(s_bt_host, BT_CONNECTION_ACTIVE, 0u);
                bt_diag_log("[BT] HID connected: %s type=%u cid=0x%04x\n",
                            bd_addr_to_str(addr), (unsigned)dtype, (unsigned)hid_cid);
                if (dtype == BT_PAIRS_TYPE_MOUSE && s_bt_host) {
                    s_bt_host->mouse_input_ready = false;
                    s_bt_host->hid_activation_cid = hid_cid;
                    s_bt_host->hid_activation_stage = 1u;
                }
            }
            break;

        case HID_SUBEVENT_REPORT: {
            hid_cid = hid_subevent_report_get_hid_cid(packet);
            const uint8_t *data = hid_subevent_report_get_report(packet);
            uint16_t len = (uint16_t)hid_subevent_report_get_report_len(packet);
            uint8_t rtype = hid_cid_type(hid_cid);
            if (s_bt_host) {
                s_bt_host->hid_reports_received++;
                if (rtype == BT_PAIRS_TYPE_MOUSE &&
                    !s_bt_host->mouse_input_ready) {
                    s_bt_host->mouse_input_ready = true;
                    bt_diag_log("[BT] mouse input ready cid=0x%04x\n",
                                (unsigned)hid_cid);
                }
            }
            /* BTstack preserves the HIDP DATA/Input prefix. The actual boot
             * or report-protocol payload begins after 0xA1. */
            if (len != 0u && data[0] == 0xA1u) {
                data++;
                len--;
            }
            switch (rtype) {
                case BT_PAIRS_TYPE_KEYBOARD:
                    bt_hid_handle_keyboard_report(hid_cid, data, len);
                    break;
                case BT_PAIRS_TYPE_MOUSE:
                    bt_hid_handle_mouse_report(hid_cid, data, len);
                    break;
                case BT_PAIRS_TYPE_JOYSTICK:
                    bt_hid_handle_joystick_report(hid_cid, data, len);
                    break;
                default:
                    /* Unknown type: guess from report length */
                    if (len >= 8u)
                        bt_hid_handle_keyboard_report(hid_cid, data, len);
                    else
                        bt_hid_handle_mouse_report(hid_cid, data, len);
                    break;
            }
            break;
        }

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
            hid_cid = hid_subevent_descriptor_available_get_hid_cid(packet);
            status = hid_subevent_descriptor_available_get_status(packet);
            BTHIDCIDEntry *entry = hid_cid_entry(hid_cid);
            if (entry)
                entry->descriptor_available = status == ERROR_CODE_SUCCESS;
            bt_diag_log("[BT-HID] descriptor cid=0x%04x status=0x%02x "
                        "len=%u\n", (unsigned)hid_cid, (unsigned)status,
                        (unsigned)hid_descriptor_storage_get_descriptor_len(
                            hid_cid));
            break;
        }

        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE: {
            hid_cid = hid_subevent_set_protocol_response_get_hid_cid(packet);
            status = hid_subevent_set_protocol_response_get_handshake_status(
                packet);
            uint8_t mode =
                hid_subevent_set_protocol_response_get_protocol_mode(packet);
            BTHIDCIDEntry *entry = hid_cid_entry(hid_cid);
            if (entry && status == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL)
                entry->protocol_mode = mode;
            bt_diag_log("[BT-HID] protocol cid=0x%04x status=0x%02x "
                        "mode=%u\n", (unsigned)hid_cid, (unsigned)status,
                        (unsigned)mode);
            break;
        }

        case HID_SUBEVENT_GET_PROTOCOL_RESPONSE:
            hid_cid = hid_subevent_get_protocol_response_get_hid_cid(packet);
            status = hid_subevent_get_protocol_response_get_handshake_status(
                packet);
            bt_diag_log("[BT-HID] get protocol cid=0x%04x status=0x%02x "
                        "mode=%u\n", (unsigned)hid_cid, (unsigned)status,
                        (unsigned)hid_subevent_get_protocol_response_get_protocol_mode(
                            packet));
            break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
            hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            bt_diag_log("[BT] HID disconnected: cid=0x%04x\n", (unsigned)hid_cid);
            bt_hid_release_all(hid_cid);
            hid_cid_unregister(hid_cid);
            if (s_bt_host) {
                s_bt_host->mouse_connected = false;
                s_bt_host->mouse_input_ready = false;
                s_bt_host->hid_connect_pending = false;
                s_bt_host->pending_hid_cid = 0u;
                if (s_bt_host->hid_activation_cid == hid_cid) {
                    s_bt_host->hid_activation_cid = 0u;
                    s_bt_host->hid_activation_stage = 0u;
                }
            }
            if (s_bt_host &&
                s_bt_host->connection_state != BT_CONNECTION_DISCOVERING &&
                s_bt_host->connection_state != BT_CONNECTION_BACKOFF)
                bt_connection_set(s_bt_host, BT_CONNECTION_PASSIVE, 2000u);
            break;

        default:
            break;
    }
}

uint8_t bt_host_last_hid_status(void)
{
    return s_bt_host ? s_bt_host->last_hid_status : 0u;
}

bool bt_host_recovery_discovery_active(const BTHost *bt)
{
    return bt && bt->connection_state == BT_CONNECTION_DISCOVERING &&
           bt->discovery_started;
}

void bt_host_claim_recovery_discovery(BTHost *bt)
{
    if (!bt_host_recovery_discovery_active(bt))
        return;
    bt->discovery_started = false;
    bt_connection_set(bt, BT_CONNECTION_PASSIVE, 0u);
    bt_diag_log("[BT-CM] F11 claimed recovery discovery session\n");
}

bool bt_host_init(BTHost *bt) {
    const btstack_chipset_t *chipset;

    if (!bt) return false;

    kprintf("[BT] Initializing BTStack (Raspberry Pi 3B, H4 over PL011)...\n");
    kprintf("[BT] PatchRAM: version=%s size=%u bytes\n",
            brcm_patch_version, (unsigned)brcm_patch_ram_length);

    /* Clock sanity probe: three consecutive reads must be monotonic and
     * ~µs apart.  Catches the legacy-timer byte-swap class of bug at a
     * glance (see time.c). */
    {
        extern uint64_t raspi3_read_legacy_system_timer(void);
        uint64_t t0 = raspi3_read_legacy_system_timer();
        uint64_t t1 = raspi3_read_legacy_system_timer();
        uint64_t t2 = raspi3_read_legacy_system_timer();
        bt_diag_log("[BT] clock probe: t0=%u t1=%u t2=%u (us, mono=%s)\n",
                    (unsigned)t0, (unsigned)t1, (unsigned)t2,
                    (t1 >= t0 && t2 >= t1) ? "yes" : "NO");
    }

    /* 32.768 kHz LPO for the BT radio low-power domain (GPCLK2 → GPIO 43).
     * Must be stable before BT_REG_EN releases the chip from reset. */
    {
        uint32_t old_ctl, old_div, new_ctl;
        new_ctl = pl011_backend_setup_bt_lpo(&old_ctl, &old_div);
        bt_diag_log("[BT] LPO GPCLK2: old ctl=%08x div=%08x -> ctl=%08x\n",
                    (unsigned)old_ctl, (unsigned)old_div, (unsigned)new_ctl);
    }

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());
    gap_set_local_name("Bellatrix");
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);

    chipset = btstack_chipset_bcm_instance();
    chipset->init(&bt_transport_config);

    s_bt_host = bt;
    bt->baudrate = bt_transport_config.baudrate_main;
    bt->initialized = true;
    bt->enabled = true;
    bt->pairing_window_open = false;
    bt->phase1_complete = false;
    bt->hci_ready = false;
    bt->connection_deadline_ms = 0u;
    bt->pending_hid_cid = 0u;
    bt->connection_state = BT_CONNECTION_WAIT_STACK;
    bt->recovery_direct_attempts = 0u;
    bt->hid_activation_stage = 0u;
    bt->hid_activation_cid = 0u;
    bt->discovery_started = false;
    bt->mouse_connected = false;
    bt->mouse_input_ready = false;
    bt->hid_connect_pending = false;
    bt->outgoing_reconnect_suspended = false;
    bt->link_recovery_state = BT_LINK_RECOVERY_IDLE;
    bt->link_recovery_reason = BT_LINK_RECOVERY_REASON_NONE;
    bt->link_recovery_error_code = 0u;
    bt->last_hid_status = 0u;
    bt->hid_reports_received = 0u;
    s_link_recoveries = 0u;
    bt->pairing_window_ms = BELLATRIX_BT_PAIRING_WINDOW_MS;
    bt->bootstrap_state = BT_BOOTSTRAP_IDLE;
    bt->power_cycle_attempts = 0;
    bt->bootstrap_deadline_ms = 0;
    bt->init_deadline_ms = 0;

    btstack_run_loop_set_timer_handler(&bt_pairing_window_timer, bt_pairing_window_timeout);
    btstack_run_loop_set_timer_context(&bt_pairing_window_timer, bt);

    bt_begin_power_cycle(bt, "initial bring-up");

    return true;
}

bool bt_host_wait_for_bootstrap(BTHost *bt, uint32_t timeout_ms)
{
    uint32_t start_ms;
    uint32_t deadline_ms;

    if (!bt || !bt->enabled) {
        return false;
    }

    start_ms = bt_now_ms();
    deadline_ms = start_ms + (timeout_ms ? timeout_ms : BELLATRIX_BT_BOOTSTRAP_WAIT_MS);

    bt_diag_log("[BT] waiting for bootstrap window (%u ms)\n",
            (unsigned)(timeout_ms ? timeout_ms : BELLATRIX_BT_BOOTSTRAP_WAIT_MS));

    /* 1s heartbeat with state-machine and normal-IRQ telemetry. It is kept
     * both in the RAM report and on the independent miniUART log. */
    uint32_t last_beat_ms = start_ms;
    uint32_t last_activity = bt_hal_raspi3_io_activity();

    while (!bt_bootstrap_is_terminal(bt)) {
        bt_host_step(bt);
        uint32_t now_ms = bt_now_ms();
        /* keep waiting while the UART link is visibly alive (PatchRAM
         * upload in progress) — only time out on real silence */
        uint32_t activity = bt_hal_raspi3_io_activity();
        if (activity != last_activity) {
            last_activity = activity;
            deadline_ms = now_ms + (timeout_ms ? timeout_ms : BELLATRIX_BT_BOOTSTRAP_WAIT_MS);
        }
        if (now_ms - last_beat_ms >= 1000u) {
            last_beat_ms = now_ms;
            bt_diag_log("[BT] wait: state=%u now=%u bdl=%u idl=%u p1=%u "
                        "hci=%u irq=%u unknown_irq=%u irq_bytes=%u ring=%u "
                        "high=%u budget=%u ovf=%u uart_err=%u\n",
                        (unsigned)bt->bootstrap_state, (unsigned)now_ms,
                        (unsigned)bt->bootstrap_deadline_ms,
                        (unsigned)bt->init_deadline_ms,
                        (unsigned)bt->phase1_complete,
                        (unsigned)bt->hci_ready,
                        (unsigned)bellatrix_physical_bt_irq_count(),
                        (unsigned)bellatrix_physical_unknown_irq_count(),
                        (unsigned)bt_hal_raspi3_irq_rx_bytes(),
                        (unsigned)bt_hal_raspi3_rx_ring_used(),
                        (unsigned)bt_hal_raspi3_rx_high_water(),
                        (unsigned)bt_hal_raspi3_irq_rx_budget_hits(),
                        (unsigned)bt_hal_raspi3_rx_overflow(),
                        (unsigned)bt_hal_raspi3_uart_rx_errors());
        }
        if ((int32_t)(now_ms - deadline_ms) >= 0) {
            bt_diag_log("[BT] bootstrap wait timed out after %u ms\n",
                    (unsigned)(timeout_ms ? timeout_ms : BELLATRIX_BT_BOOTSTRAP_WAIT_MS));
            return false;
        }
    }

    return bt->bootstrap_state == BT_BOOTSTRAP_WORKING;
}

bool bt_host_is_working(const BTHost *bt)
{
    return bt && bt->bootstrap_state == (uint8_t)BT_BOOTSTRAP_WORKING;
}

/* An H4 block stuck mid-fill for seconds is unambiguous (HCI events
 * complete in microseconds at 115200): either an RX overrun desynced
 * the parser, or the BCM43430A1 firmware wedged outright and stopped
 * transmitting — observed during LE advert floods, same family as the
 * H5 and Remote Name Request firmware bugs.  A soft HCI restart is not
 * enough for the wedge (the chip ignores HCI Reset), so recovery is the
 * full bring-up: BT_REG_EN power cycle + PatchRAM re-upload (~5 s).
 * The existing bootstrap state machine does all of it. */
#define BT_LINK_STALL_MS 3000u
#define BT_LINK_MAX_RECOVERIES 8u

static const char *bt_link_recovery_reason_name(const BTHost *bt)
{
    if (!bt)
        return "unknown recovery";
    switch ((BTLinkRecoveryReason)bt->link_recovery_reason) {
        case BT_LINK_RECOVERY_REASON_HARDWARE_ERROR:
            return "controller hardware error";
        case BT_LINK_RECOVERY_REASON_TRANSPORT_ERROR:
            return "transport overrun/error";
        case BT_LINK_RECOVERY_REASON_CONTROLLER_SILENCE:
            return "controller/device silence mid-H4";
        case BT_LINK_RECOVERY_REASON_H4_DESYNC:
            return "H4 protocol desync";
        case BT_LINK_RECOVERY_REASON_CONTROLLER_MUTE:
            return "controller mute (no HCI response)";
        default:
            return "unspecified link failure";
    }
}

static void bt_link_recovery_request(BTHost *bt, BTLinkRecoveryReason reason,
                                     uint8_t error_code)
{
    if (!bt || bt->link_recovery_state != BT_LINK_RECOVERY_IDLE)
        return;
    if (s_link_recoveries >= BT_LINK_MAX_RECOVERIES) {
        bt_diag_log("[BT] %s, but recovery budget exhausted (%u)\n",
                    bt_link_recovery_reason_name(bt),
                    (unsigned)s_link_recoveries);
        return;
    }

    bt->link_recovery_reason = (uint8_t)reason;
    bt->link_recovery_error_code = error_code;
    bt->link_recovery_state = BT_LINK_RECOVERY_REQUESTED;
    s_link_recoveries++;
    if (reason == BT_LINK_RECOVERY_REASON_HARDWARE_ERROR)
        bt_diag_log("[BT] recovery requested: %s 0x%02x (%u/%u)\n",
                    bt_link_recovery_reason_name(bt), (unsigned)error_code,
                    (unsigned)s_link_recoveries,
                    (unsigned)BT_LINK_MAX_RECOVERIES);
    else
        bt_diag_log("[BT] recovery requested: %s (%u/%u)\n",
                    bt_link_recovery_reason_name(bt),
                    (unsigned)s_link_recoveries,
                    (unsigned)BT_LINK_MAX_RECOVERIES);
}

static void bt_link_recovery_step(BTHost *bt)
{
    if (!bt)
        return;

    switch ((BTLinkRecoveryState)bt->link_recovery_state) {
        case BT_LINK_RECOVERY_REQUESTED:
            bt_scan_notify_recovery();
            bt_pairing_window_close(bt);
            bt->mouse_connected = false;
            bt->mouse_input_ready = false;
            bt->hid_connect_pending = false;
            bt->pending_hid_cid = 0u;
            bt->hid_activation_stage = 0u;
            bt->hid_activation_cid = 0u;
            bt_connection_set(bt, BT_CONNECTION_WAIT_STACK, 0u);
            bt_diag_log("[BT-RECOVERY] phase 1/4: request HCI off\n");
            hci_power_control(HCI_POWER_OFF);
            bt->link_recovery_state = BT_LINK_RECOVERY_FORCE_INITIALIZING;
            return;

        case BT_LINK_RECOVERY_FORCE_INITIALIZING:
            bt_diag_log("[BT-RECOVERY] phase 2/4: release halted HCI state\n");
            hci_power_control(HCI_POWER_ON);
            bt->link_recovery_state = BT_LINK_RECOVERY_FORCE_OFF;
            return;

        case BT_LINK_RECOVERY_FORCE_OFF:
            bt_diag_log("[BT-RECOVERY] phase 3/4: close HCI transport\n");
            hci_power_control(HCI_POWER_OFF);
            bt->link_recovery_state = BT_LINK_RECOVERY_PHYSICAL_RESET;
            return;

        case BT_LINK_RECOVERY_PHYSICAL_RESET:
            bt_diag_log("[BT] recovery: reset controller and reload firmware\n");
            bt_hal_raspi3_flush_rx();
            bt->phase1_complete = false;
            bt->power_cycle_attempts = 0u;
            btstack_chipset_bcm_instance()->init(&bt_transport_config);
            bt->link_recovery_state = BT_LINK_RECOVERY_IDLE;
            bt->link_recovery_reason = BT_LINK_RECOVERY_REASON_NONE;
            bt->link_recovery_error_code = 0u;
            bt_begin_power_cycle(bt, "deferred link recovery");
            return;

        default:
            return;
    }
}

/* The chip can also confess on its own: HCI Hardware Error (evt 0x10).
 * btstack's default reaction is a bare off+on, which would skip the
 * PatchRAM upload — route it to the full recovery instead. */
static void bt_hardware_error_callback(uint8_t error_code)
{
    if (s_bt_host)
        bt_link_recovery_request(s_bt_host,
                                 BT_LINK_RECOVERY_REASON_HARDWARE_ERROR,
                                 error_code);
}

static void bt_link_watchdog(BTHost *bt)
{
    static uint32_t stall_filled;
    static uint32_t stall_since_ms;
    static uint32_t stall_overflow;
    static uint32_t stall_uart_errors;
    static uint32_t stall_activity;
    uint32_t filled, wanted, now_ms;

    if (bt->bootstrap_state != BT_BOOTSTRAP_WORKING || !bt->hci_ready)
        return;

    bt_hal_raspi3_rx_pending(&filled, &wanted);
    if (filled == 0u || filled >= wanted) {        /* idle or completing */
        stall_since_ms = 0u;
        return;
    }

    now_ms = bt_now_ms();
    if (stall_since_ms == 0u || filled != stall_filled) {
        stall_filled = filled;
        stall_since_ms = now_ms;
        stall_overflow = bt_hal_raspi3_rx_overflow();
        stall_uart_errors = bt_hal_raspi3_uart_rx_errors();
        stall_activity = bt_hal_raspi3_io_activity();
        return;
    }
    if (now_ms - stall_since_ms < BT_LINK_STALL_MS)
        return;

    stall_since_ms = 0u;
    uint32_t overflow_delta = bt_hal_raspi3_rx_overflow() - stall_overflow;
    uint32_t error_delta =
        bt_hal_raspi3_uart_rx_errors() - stall_uart_errors;
    uint32_t activity_delta = bt_hal_raspi3_io_activity() - stall_activity;
    BTLinkRecoveryReason reason;
    if (overflow_delta || error_delta)
        reason = BT_LINK_RECOVERY_REASON_TRANSPORT_ERROR;
    else if (activity_delta == 0u)
        reason = BT_LINK_RECOVERY_REASON_CONTROLLER_SILENCE;
    else
        reason = BT_LINK_RECOVERY_REASON_H4_DESYNC;
    bt_diag_log("[BT] link stalled class=%s rxq=%u/%u overflow+%u "
                "uart_error+%u activity+%u\n",
                reason == BT_LINK_RECOVERY_REASON_TRANSPORT_ERROR
                    ? "transport" : reason == BT_LINK_RECOVERY_REASON_CONTROLLER_SILENCE
                        ? "silence" : "H4 desync",
                (unsigned)filled, (unsigned)wanted,
                (unsigned)overflow_delta, (unsigned)error_delta,
                (unsigned)activity_delta);
    bt_link_recovery_request(bt, reason, 0u);
}

/* A complete controller mute leaves no partial H4 packet for the watchdog
 * above. HCI traffic transmitted with no RX response for three seconds is a
 * controller failure, unless PL011 still has bytes waiting in its FIFO (which
 * instead proves a host-side IRQ/drain failure). */
#define BT_CONTROLLER_MUTE_MS 3000u

static void bt_controller_liveness(BTHost *bt)
{
    static uint32_t last_rx_total;
    static uint32_t tx_at_last_rx;
    static uint32_t mute_since_ms;

    if (bt->bootstrap_state != BT_BOOTSTRAP_WORKING || !bt->hci_ready) {
        last_rx_total = bt_hal_raspi3_io_rx();
        tx_at_last_rx = bt_hal_raspi3_io_tx();
        mute_since_ms = 0u;
        return;
    }

    uint32_t rx = bt_hal_raspi3_io_rx();
    uint32_t tx = bt_hal_raspi3_io_tx();
    if (rx != last_rx_total) {
        last_rx_total = rx;
        tx_at_last_rx = tx;
        mute_since_ms = 0u;
        return;
    }
    if (tx == tx_at_last_rx) {
        mute_since_ms = 0u;
        return;
    }

    uint32_t now = bt_now_ms();
    if (mute_since_ms == 0u) {
        mute_since_ms = now;
        return;
    }
    if (now - mute_since_ms < BT_CONTROLLER_MUTE_MS)
        return;
    mute_since_ms = 0u;

    uint32_t fr = pl011_backend_flag_register();
    bt_diag_log("[BT] controller mute: tx+%u rx=%u FR=%08x rxfe=%u "
                "imsc=%08x irq_armed=%u irq_entries=%u\n",
                (unsigned)(tx - tx_at_last_rx), (unsigned)rx, (unsigned)fr,
                (unsigned)((fr >> 4) & 1u),
                (unsigned)pl011_backend_irq_mask(),
                bellatrix_physical_bt_irq_is_armed() ? 1u : 0u,
                (unsigned)bellatrix_physical_bt_irq_count());
    if ((fr & (1u << 4)) == 0u) {
        bt_diag_log("[BT] RX FIFO non-empty during mute: host IRQ/drain "
                    "fault; controller recovery suppressed\n");
        return;
    }
    bt_link_recovery_request(bt, BT_LINK_RECOVERY_REASON_CONTROLLER_MUTE, 0u);
}

static void bt_connection_step(BTHost *bt)
{
    if (!bt || bt->bootstrap_state != BT_BOOTSTRAP_WORKING)
        return;

    uint32_t now = bt_now_ms();

    switch (bt->connection_state) {
        case BT_CONNECTION_WAIT_STACK:
            return;

        case BT_CONNECTION_ACTIVE:
            /* Advance at most one HID control request per normal Core-3 tick,
             * never from the connection event callback. */
            if (bt->hid_activation_stage == 1u) {
                uint8_t status =
                    hid_host_send_exit_suspend(bt->hid_activation_cid);
                bt_diag_log("[BT-HID] exit suspend cid=0x%04x "
                            "status=0x%02x\n",
                            (unsigned)bt->hid_activation_cid,
                            (unsigned)status);
                bt->hid_activation_stage = 2u;
                return;
            }
            if (bt->hid_activation_stage == 2u) {
                uint8_t status =
                    hid_host_send_get_protocol(bt->hid_activation_cid);
                bt_diag_log("[BT-HID] get protocol request cid=0x%04x "
                            "status=0x%02x\n",
                            (unsigned)bt->hid_activation_cid,
                            (unsigned)status);
                bt->hid_activation_stage = 0u;
            }
            return;

        case BT_CONNECTION_PASSIVE:
            if (bt->mouse_connected) {
                bt_connection_set(bt, BT_CONNECTION_ACTIVE, 0u);
                return;
            }
            if (bt->outgoing_reconnect_suspended ||
                bt_pairs_count() == 0u ||
                bt->connection_deadline_ms == 0u ||
                (int32_t)(now - bt->connection_deadline_ms) < 0)
                return;
            bt_host_connect_pairs(bt);
            return;

        case BT_CONNECTION_CONNECTING:
            if (bt->connection_deadline_ms != 0u &&
                (int32_t)(now - bt->connection_deadline_ms) >= 0) {
                uint16_t cid = bt->pending_hid_cid;
                bt->pending_hid_cid = 0u;
                bt->hid_connect_pending = false;
                if (cid != 0u)
                    hid_host_disconnect(cid);
                bt_diag_log("[BT-CM] HID connection timed out: "
                            "cid=0x%04x\n", (unsigned)cid);
                bt_connection_set(bt, BT_CONNECTION_DISCOVERING, 0u);
            }
            return;

        case BT_CONNECTION_DISCOVERING:
            if (!bt->discovery_started) {
                bt->discovery_started = true;
                bt_pairing_window_open(bt);
                bt_scan_start();
                bt_connection_set(bt, BT_CONNECTION_DISCOVERING, 30000u);
                bt_diag_log("[BT-CM] recovery discovery started\n");
            }
            for (unsigned i = 0u; i < bt_scan_count(); i++) {
                const BTScanResult *r = bt_scan_get(i);
                if (!r || !r->hid || !bt_pairs_is_known(r->addr) ||
                    bt_pairs_classify(r) != BT_PAIRS_TYPE_MOUSE)
                    continue;
                bt_diag_log("[BT-CM] saved mouse rediscovered: %s\n",
                            r->name[0] ? r->name : "(no name)");
                bt_scan_stop();
                bt->discovery_started = false;
                bt_pairing_window_close(bt);
                bt_connection_set(bt, BT_CONNECTION_PASSIVE, 1u);
                return;
            }
            if (bt->connection_deadline_ms != 0u &&
                (int32_t)(now - bt->connection_deadline_ms) >= 0) {
                bt_scan_stop();
                bt->discovery_started = false;
                bt_pairing_window_close(bt);
                bt_connection_set(bt, BT_CONNECTION_BACKOFF, 60000u);
                bt_diag_log("[BT-CM] recovery discovery expired; passive "
                            "reconnect remains armed\n");
            }
            return;

        case BT_CONNECTION_BACKOFF:
            if (bt->connection_deadline_ms != 0u &&
                (int32_t)(now - bt->connection_deadline_ms) >= 0)
                bt_connection_set(bt, BT_CONNECTION_PASSIVE, 2000u);
            return;

        default:
            bt_connection_set(bt, BT_CONNECTION_WAIT_STACK, 0u);
            return;
    }
}

void bt_host_step(BTHost *bt) {
    static uint32_t transport_diag_at_ms;
    static uint32_t last_reactor_rx_bytes;
    static uint32_t last_reactor_rx_completions;
    static uint32_t last_reactor_rx_budget_hits;
    if (!bt || !bt->enabled) return;

    if (bt->link_recovery_state != BT_LINK_RECOVERY_IDLE)
        bt_link_recovery_step(bt);
    else
        bt_bootstrap_step(bt);

    // Drain/rearm the normal-IRQ-owned UART transport from Core 3.
    bt_hal_raspi3_poll_uart();

    // Execute run loop tasks
    btstack_run_loop_embedded_execute_once();

    if (bt->link_recovery_state == BT_LINK_RECOVERY_IDLE) {
        bt_link_watchdog(bt);
        bt_controller_liveness(bt);
        if (bt->link_recovery_state == BT_LINK_RECOVERY_IDLE)
            bt_connection_step(bt);
    }

    if (transport_diag_at_ms == 0u ||
        (int32_t)(bt_now_ms() - transport_diag_at_ms) >= 0) {
        uint32_t rx_bytes = bt_hal_raspi3_reactor_rx_bytes();
        uint32_t rx_completions = bt_hal_raspi3_reactor_rx_completions();
        uint32_t rx_budget_hits = bt_hal_raspi3_reactor_rx_budget_hits();
        transport_diag_at_ms = bt_now_ms() + 10000u;
        bt_diag_log("[BT-IRQ] entries=%u bytes=%u budget_hit=%u ring=%u "
                    "high=%u overflow=%u uart_err=%u(fe=%u pe=%u be=%u "
                    "oe=%u)\n",
                    (unsigned)bellatrix_physical_bt_irq_count(),
                    (unsigned)bt_hal_raspi3_irq_rx_bytes(),
                    (unsigned)bt_hal_raspi3_irq_rx_budget_hits(),
                    (unsigned)bt_hal_raspi3_rx_ring_used(),
                    (unsigned)bt_hal_raspi3_rx_high_water(),
                    (unsigned)bt_hal_raspi3_rx_overflow(),
                    (unsigned)bt_hal_raspi3_uart_rx_errors(),
                    (unsigned)bt_hal_raspi3_uart_rx_framing_errors(),
                    (unsigned)bt_hal_raspi3_uart_rx_parity_errors(),
                    (unsigned)bt_hal_raspi3_uart_rx_break_errors(),
                    (unsigned)bt_hal_raspi3_uart_rx_overrun_errors());
        bt_diag_log("[BT-REACTOR-RX] bytes=%u completions=%u budget_hit=%u\n",
                    (unsigned)(rx_bytes - last_reactor_rx_bytes),
                    (unsigned)(rx_completions - last_reactor_rx_completions),
                    (unsigned)(rx_budget_hits - last_reactor_rx_budget_hits));
        last_reactor_rx_bytes = rx_bytes;
        last_reactor_rx_completions = rx_completions;
        last_reactor_rx_budget_hits = rx_budget_hits;
    }
}

void bt_host_shutdown(BTHost *bt) {
    if (!bt || !bt->initialized) return;
    
    kprintf("[BT] shutdown\n");
    btstack_run_loop_remove_timer(&bt_pairing_window_timer);
    bt_pairing_window_close(bt);
    hci_power_control(HCI_POWER_OFF);
    bt->enabled = false;
    bt->bootstrap_state = BT_BOOTSTRAP_IDLE;
    bt->bootstrap_deadline_ms = 0;
    bt->init_deadline_ms = 0;
    bt->hci_ready = false;
    bt->mouse_connected = false;
    bt->mouse_input_ready = false;
    bt->hid_connect_pending = false;
    bt->link_recovery_state = BT_LINK_RECOVERY_IDLE;
    s_bt_host = NULL;
}
#else
bool bt_host_init(BTHost *bt) {
    if (!bt) {
        return false;
    }

    bt->enabled = false;
    bt->initialized = false;
    bt->pairing_window_open = false;
    bt->phase1_complete = false;
    bt->hci_ready = false;
    bt->connection_deadline_ms = 0u;
    bt->pending_hid_cid = 0u;
    bt->connection_state = BT_CONNECTION_WAIT_STACK;
    bt->recovery_direct_attempts = 0u;
    bt->hid_activation_stage = 0u;
    bt->hid_activation_cid = 0u;
    bt->discovery_started = false;
    bt->mouse_connected = false;
    bt->mouse_input_ready = false;
    bt->hid_connect_pending = false;
    bt->outgoing_reconnect_suspended = false;
    bt->link_recovery_state = BT_LINK_RECOVERY_IDLE;
    bt->link_recovery_reason = BT_LINK_RECOVERY_REASON_NONE;
    bt->link_recovery_error_code = 0u;
    bt->last_hid_status = 0u;
    bt->hid_reports_received = 0u;
    bt->baudrate = 0;
    bt->pairing_window_ms = 0;
    bt->bootstrap_state = 0;
    bt->power_cycle_attempts = 0;
    bt->bootstrap_deadline_ms = 0;
    bt->init_deadline_ms = 0;
    return true;
}

bool bt_host_wait_for_bootstrap(BTHost *bt, uint32_t timeout_ms) {
    (void)bt;
    (void)timeout_ms;
    return false;
}

bool bt_host_is_working(const BTHost *bt) {
    (void)bt;
    return false;
}

void bt_host_connect_pairs(BTHost *bt) {
    (void)bt;
}

void bt_host_close_pairing_window(BTHost *bt) { (void)bt; }
void bt_host_open_pairing_window(BTHost *bt) { (void)bt; }
void bt_host_set_outgoing_reconnect_suspended(BTHost *bt, bool suspended)
{
    (void)bt;
    (void)suspended;
}
void bt_host_prepare_explicit_pairing(BTHost *bt, const uint8_t addr[6])
{
    (void)bt;
    (void)addr;
}
bool bt_host_mouse_connected(void) { return false; }
uint8_t bt_host_last_hid_status(void) { return 0u; }
bool bt_host_recovery_discovery_active(const BTHost *bt)
{
    (void)bt;
    return false;
}
void bt_host_claim_recovery_discovery(BTHost *bt) { (void)bt; }

void bt_host_step(BTHost *bt) {
    (void)bt;
}

void bt_host_shutdown(BTHost *bt) {
    (void)bt;
}
#endif
