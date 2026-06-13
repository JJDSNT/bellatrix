#include "io/bluetooth/bt_scan.h"
#include "support.h"
#include <string.h>

#if BELLATRIX_ENABLE_BTSTACK

#include "btstack.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "ad_parser.h"
#include "bluetooth_data_types.h"
#include "gap.h"
#include "hci.h"
#include "io/bluetooth/bt_diag.h"

#define BT_SCAN_INQUIRY_1280MS_UNITS 4u      /* ≈5.1s classic inquiry  */
#define BT_SCAN_LE_PHASE_MS          5000u   /* LE scan slice           */
/* BCM43430A1 delivers inquiry results but sometimes never sends the
 * Inquiry Complete event — without a watchdog the phase machine stalls
 * in classic forever (seen on Pi 3B; chip still answers Inquiry Cancel). */
#define BT_SCAN_CLASSIC_WATCHDOG_MS  (BT_SCAN_INQUIRY_1280MS_UNITS * 1280u + 3000u)
#define BT_SCAN_CANCEL_GRACE_MS      2000u

typedef enum {
    SCAN_OFF = 0,
    SCAN_WAIT_STACK,        /* started but HCI not WORKING yet */
    SCAN_CLASSIC_INQUIRY,
    SCAN_CLASSIC_NAMES,     /* remote name requests, one at a time */
    SCAN_LE,
} ScanPhase;

static btstack_packet_callback_registration_t s_scan_event_cb;
static btstack_timer_source_t s_le_phase_timer;
static btstack_timer_source_t s_classic_watchdog;
static bool s_classic_cancel_sent;

static BTScanResult s_results[BT_SCAN_MAX_RESULTS];
static unsigned     s_count;
static uint32_t     s_generation;
static ScanPhase    s_phase = SCAN_OFF;
static bool         s_registered;

static void bt_scan_enter_classic(void);
static void bt_scan_enter_le(void);

static void touched(void) { s_generation++; }

static BTScanResult *find_or_add(const uint8_t *addr, uint8_t transport)
{
    unsigned i;
    BTScanResult *r;

    /* Match by address regardless of transport: a dual-mode device (public
     * address on both BR/EDR and LE, like the user's keyboard) must show up
     * as one entry, not two. */
    for (i = 0u; i < s_count; i++) {
        if (memcmp(s_results[i].addr, addr, 6) == 0) {
            s_results[i].transport |= transport;
            return &s_results[i];
        }
    }
    if (s_count >= BT_SCAN_MAX_RESULTS)
        return NULL;

    r = &s_results[s_count++];
    memset(r, 0, sizeof(*r));
    memcpy(r->addr, addr, 6);
    r->transport = transport;
    touched();
    return r;
}

static void set_name(BTScanResult *r, const uint8_t *name, unsigned len)
{
    unsigned i, n = len;

    if (n >= BT_SCAN_NAME_LEN)
        n = BT_SCAN_NAME_LEN - 1u;
    /* EIR names are NUL-padded to the field size; stop at the first NUL
     * instead of rendering the padding as '?' */
    for (i = 0u; i < n && name[i]; i++) {
        char c = (char)name[i];
        r->name[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
    }
    while (i > 0u && r->name[i - 1u] == ' ')
        i--;
    r->name[i] = '\0';
    r->name_state = 2u;
    touched();
}

/* Readable placeholder ("<keyboard>") until a real name arrives. */
static void set_label(BTScanResult *r, const char *label)
{
    unsigned i;

    if (r->name_state >= 2u || !label)
        return;
    for (i = 0u; label[i] && i < BT_SCAN_NAME_LEN - 1u; i++)
        r->name[i] = label[i];
    r->name[i] = '\0';
    r->name_state = 1u;
    touched();
}

static const char *label_from_appearance(uint16_t appearance)
{
    switch (appearance) {
    case 961: return "<keyboard>";
    case 962: return "<mouse>";
    case 963: return "<joystick>";
    case 964: return "<gamepad>";
    default:  return (appearance >> 6) == 15u ? "<HID device>" : NULL;
    }
}

static const char *label_from_cod(uint32_t cod)
{
    if (((cod >> 8) & 0x1Fu) != 0x05u)   /* major class: peripheral */
        return NULL;
    switch ((cod >> 2) & 0x0Fu) {        /* minor: pointing/gaming   */
    case 1: return "<joystick>";
    case 2: return "<gamepad>";
    default: break;
    }
    switch ((cod >> 6) & 0x03u) {        /* minor: keyboard/pointing */
    case 1: return "<keyboard>";
    case 2: return "<mouse>";
    case 3: return "<keyboard+mouse>";
    default: return "<HID device>";
    }
}

static void bt_scan_le_phase_timeout(btstack_timer_source_t *ts)
{
    (void)ts;
    if (s_phase != SCAN_LE)
        return;
    gap_stop_scan();
    bt_scan_enter_classic();
}

/* Two stages: first cancel the inquiry (btstack emits the missing
 * GAP_EVENT_INQUIRY_COMPLETE on the cancel's Command Complete); if even
 * that brings nothing, force-advance the phase machine. */
static void bt_scan_classic_watchdog_fired(btstack_timer_source_t *ts)
{
    (void)ts;
    if (s_phase != SCAN_CLASSIC_INQUIRY)
        return;

    if (!s_classic_cancel_sent) {
        s_classic_cancel_sent = true;
        bt_diag_log("[SCAN] inquiry complete missing after %u ms, cancelling\n",
                    (unsigned)BT_SCAN_CLASSIC_WATCHDOG_MS);
        gap_inquiry_stop();
        btstack_run_loop_set_timer(&s_classic_watchdog, BT_SCAN_CANCEL_GRACE_MS);
        btstack_run_loop_set_timer_handler(&s_classic_watchdog,
                                           bt_scan_classic_watchdog_fired);
        btstack_run_loop_add_timer(&s_classic_watchdog);
        return;
    }

    bt_diag_log("[SCAN] inquiry cancel brought no event either, forcing LE phase\n");
    bt_scan_enter_le();
}

static void bt_scan_diag_tap_arm(uint32_t n)
{
    void bt_hal_raspi3_diag_tap(uint32_t rx_bytes, uint32_t tx_bytes);
    bt_hal_raspi3_diag_tap(n, n);
}

static void bt_scan_enter_classic(void)
{
    int err;

    s_phase = SCAN_CLASSIC_INQUIRY;
    s_classic_cancel_sent = false;
    bt_scan_diag_tap_arm(24u);
    touched();
    err = gap_inquiry_start(BT_SCAN_INQUIRY_1280MS_UNITS);
    bt_diag_log("[SCAN] classic inquiry start: err=%d\n", err);
    btstack_run_loop_set_timer(&s_classic_watchdog, BT_SCAN_CLASSIC_WATCHDOG_MS);
    btstack_run_loop_set_timer_handler(&s_classic_watchdog,
                                       bt_scan_classic_watchdog_fired);
    btstack_run_loop_add_timer(&s_classic_watchdog);
}

static void bt_scan_enter_le(void)
{
    s_phase = SCAN_LE;
    touched();
    bt_scan_diag_tap_arm(24u);
    bt_diag_log("[SCAN] LE scan start\n");
    /* Active scan so devices answer with scan responses (names).
     * Window 0x10/interval 0x60 ≈ 17% duty: a 100% window floods the
     * 115200 line (~10KB/s of adverts) and any pump gap loses bytes. */
    gap_set_scan_parameters(1u, 0x0060u, 0x0010u);
    gap_start_scan();
    btstack_run_loop_set_timer(&s_le_phase_timer, BT_SCAN_LE_PHASE_MS);
    btstack_run_loop_set_timer_handler(&s_le_phase_timer, bt_scan_le_phase_timeout);
    btstack_run_loop_add_timer(&s_le_phase_timer);
}

#define BT_UUID16_HID_SERVICE 0x1812u

static bool ad_uuid16_list_has(const uint8_t *d, unsigned len, uint16_t uuid)
{
    unsigned i;
    for (i = 0u; i + 1u < len; i += 2u) {
        if ((uint16_t)(d[i] | (d[i + 1] << 8)) == uuid)
            return true;
    }
    return false;
}

static void bt_scan_handle_adv_report(uint8_t *packet)
{
    bd_addr_t addr;
    BTScanResult *r;
    const uint8_t *data;
    uint8_t data_len;
    uint8_t addr_type;
    ad_context_t ctx;
    const uint8_t *name = NULL, *short_name = NULL;
    unsigned name_len = 0u, short_name_len = 0u;
    uint16_t appearance = 0u;
    bool hid = false;

    gap_event_advertising_report_get_address(packet, addr);
    addr_type = gap_event_advertising_report_get_address_type(packet);
    data      = gap_event_advertising_report_get_data(packet);
    data_len  = gap_event_advertising_report_get_data_length(packet);

    for (ad_iterator_init(&ctx, data_len, data);
         ad_iterator_has_more(&ctx);
         ad_iterator_next(&ctx)) {
        uint8_t type = ad_iterator_get_data_type(&ctx);
        const uint8_t *d = ad_iterator_get_data(&ctx);
        unsigned dlen = ad_iterator_get_data_len(&ctx);

        switch (type) {
        case BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME:
            name = d; name_len = dlen;
            break;
        case BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME:
            short_name = d; short_name_len = dlen;
            break;
        case BLUETOOTH_DATA_TYPE_INCOMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
        case BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS:
            if (ad_uuid16_list_has(d, dlen, BT_UUID16_HID_SERVICE))
                hid = true;
            break;
        case BLUETOOTH_DATA_TYPE_SERVICE_DATA:
            if (dlen >= 2u &&
                (uint16_t)(d[0] | (d[1] << 8)) == BT_UUID16_HID_SERVICE)
                hid = true;
            break;
        case BLUETOOTH_DATA_TYPE_APPEARANCE:
            if (dlen >= 2u)
                appearance = (uint16_t)(d[0] | (d[1] << 8));
            break;
        default:
            break;
        }
    }

    /* Resolvable/non-resolvable private addresses rotate every few minutes,
     * so each ambient phone shows up as a parade of "new" devices.  Only
     * accept a NEW entry with a stable identity: public address, static
     * random (top two bits 11), or an advert that says who it is. */
    r = NULL;
    {
        unsigned i;
        for (i = 0u; i < s_count; i++) {
            if (memcmp(s_results[i].addr, addr, 6) == 0) {
                r = &s_results[i];
                r->transport |= BT_SCAN_TRANSPORT_LE;
                break;
            }
        }
    }
    if (!r) {
        bool stable = (addr_type == 0u) || ((addr[0] & 0xC0u) == 0xC0u);
        if (!stable && !name && !short_name && !hid && appearance == 0u)
            return;
        r = find_or_add(addr, BT_SCAN_TRANSPORT_LE);
        if (!r)
            return;
        bt_diag_log("[SCAN] LE adv %02x:%02x:%02x:%02x:%02x:%02x rssi=%d\n",
                    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                    (int)(int8_t)gap_event_advertising_report_get_rssi(packet));
    }

    r->addr_type = addr_type;
    r->rssi      = (int8_t)gap_event_advertising_report_get_rssi(packet);
    if (hid)
        r->hid = true;
    if (appearance) {
        r->appearance = appearance;
        set_label(r, label_from_appearance(appearance));
    }
    if (name)
        set_name(r, name, name_len);
    else if (short_name && r->name_state < 2u)
        set_name(r, short_name, short_name_len);
    touched();
}

static void bt_scan_handle_inquiry_result(uint8_t *packet)
{
    bd_addr_t addr;
    BTScanResult *r;

    gap_event_inquiry_result_get_bd_addr(packet, addr);
    bt_diag_log("[SCAN] inquiry result %02x:%02x:%02x:%02x:%02x:%02x\n",
                addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    r = find_or_add(addr, BT_SCAN_TRANSPORT_CLASSIC);
    if (!r)
        return;

    r->cod          = gap_event_inquiry_result_get_class_of_device(packet);
    r->psrm         = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    r->clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
    if (((r->cod >> 8) & 0x1Fu) == 0x05u)   /* peripheral major class */
        r->hid = true;
    set_label(r, label_from_cod(r->cod));
    if (gap_event_inquiry_result_get_rssi_available(packet))
        r->rssi = (int8_t)gap_event_inquiry_result_get_rssi(packet);
    if (gap_event_inquiry_result_get_name_available(packet)) {
        set_name(r, gap_event_inquiry_result_get_name(packet),
                 gap_event_inquiry_result_get_name_len(packet));
    }
    touched();
}

static unsigned s_evt_logged;

static void bt_scan_packet_handler(uint8_t packet_type, uint16_t channel,
                                   uint8_t *packet, uint16_t size)
{
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || s_phase == SCAN_OFF)
        return;

    /* raw view of the first events after scan start — tells a dead H5 link
     * (nothing at all) apart from a scan that runs but finds nobody */
    if (s_evt_logged < 16u) {
        s_evt_logged++;
        bt_diag_log("[SCAN] evt 0x%02x len=%u\n",
                    (unsigned)hci_event_packet_get_type(packet),
                    (unsigned)size);
    }

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING)
            break;
        if (s_phase == SCAN_WAIT_STACK) {
            bt_scan_enter_classic();
        } else {
            /* WORKING arriving mid-scan = the link watchdog power-cycled
             * the HCI after a desync; the controller forgot any inquiry/
             * scan in progress, so restart the phase machine (results
             * collected so far are kept). */
            bt_diag_log("[SCAN] stack recovered, restarting scan cycle\n");
            btstack_run_loop_remove_timer(&s_classic_watchdog);
            btstack_run_loop_remove_timer(&s_le_phase_timer);
            bt_scan_enter_classic();
        }
        break;

    case GAP_EVENT_INQUIRY_RESULT:
        bt_scan_handle_inquiry_result(packet);
        break;

    case GAP_EVENT_INQUIRY_COMPLETE:
        btstack_run_loop_remove_timer(&s_classic_watchdog);
        bt_diag_log("[SCAN] inquiry complete: status=%u found=%u\n",
                    (unsigned)gap_event_inquiry_complete_get_status(packet),
                    s_count);
        if (s_phase == SCAN_CLASSIC_INQUIRY) {
            /* Remote Name Request crashes the BCM43430A1 firmware mid
             * delivery (seen twice: 18 zero bytes; 248-of-255 byte stall).
             * Classic names are cosmetic here — CoD identifies the device
             * type and pairing gets names via SDP/GATT — so skip straight
             * to the LE phase. */
            bt_scan_enter_le();
        }
        break;

    case GAP_EVENT_ADVERTISING_REPORT:
        bt_scan_handle_adv_report(packet);
        break;

    default:
        break;
    }
}

void bt_scan_start(void)
{
    if (s_phase != SCAN_OFF)
        return;

    if (!s_registered) {
        s_scan_event_cb.callback = &bt_scan_packet_handler;
        hci_add_event_handler(&s_scan_event_cb);
        s_registered = true;
    }

    s_count = 0u;
    s_evt_logged = 0u;
    touched();

    bt_scan_diag_tap_arm(96u);
    bt_diag_log("[SCAN] start (hci state=%u)\n", (unsigned)hci_get_state());
    if (hci_get_state() == HCI_STATE_WORKING) {
        bt_scan_enter_classic();
    } else {
        s_phase = SCAN_WAIT_STACK;
    }
}

void bt_scan_stop(void)
{
    if (s_phase == SCAN_OFF)
        return;

    switch (s_phase) {
    case SCAN_CLASSIC_INQUIRY:
        btstack_run_loop_remove_timer(&s_classic_watchdog);
        gap_inquiry_stop();
        break;
    case SCAN_LE:
        btstack_run_loop_remove_timer(&s_le_phase_timer);
        gap_stop_scan();
        break;
    default:
        break;
    }
    s_phase = SCAN_OFF;
    touched();
}

unsigned bt_scan_count(void)
{
    return s_count;
}

const BTScanResult *bt_scan_get(unsigned index)
{
    if (index >= s_count)
        return NULL;
    return &s_results[index];
}

const char *bt_scan_status(void)
{
    switch (s_phase) {
    case SCAN_OFF:             return "stopped";
    case SCAN_WAIT_STACK:      return "waiting for controller...";
    case SCAN_CLASSIC_INQUIRY: return "scanning (classic)...";
    case SCAN_CLASSIC_NAMES:   return "resolving names...";
    case SCAN_LE:              return "scanning (LE)...";
    default:                   return "?";
    }
}

uint32_t bt_scan_generation(void)
{
    return s_generation;
}

#else /* !BELLATRIX_ENABLE_BTSTACK */

void bt_scan_start(void) {}
void bt_scan_stop(void) {}
unsigned bt_scan_count(void) { return 0u; }
const BTScanResult *bt_scan_get(unsigned index) { (void)index; return 0; }
const char *bt_scan_status(void) { return "bluetooth disabled"; }
uint32_t bt_scan_generation(void) { return 0u; }

#endif /* BELLATRIX_ENABLE_BTSTACK */
