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

#define BT_SCAN_INQUIRY_1280MS_UNITS 4u      /* ≈5.1s classic inquiry  */
#define BT_SCAN_LE_PHASE_MS          5000u   /* LE scan slice           */

typedef enum {
    SCAN_OFF = 0,
    SCAN_WAIT_STACK,        /* started but HCI not WORKING yet */
    SCAN_CLASSIC_INQUIRY,
    SCAN_CLASSIC_NAMES,     /* remote name requests, one at a time */
    SCAN_LE,
} ScanPhase;

static btstack_packet_callback_registration_t s_scan_event_cb;
static btstack_timer_source_t s_le_phase_timer;

static BTScanResult s_results[BT_SCAN_MAX_RESULTS];
static unsigned     s_count;
static uint32_t     s_generation;
static ScanPhase    s_phase = SCAN_OFF;
static bool         s_registered;
static int          s_name_req_index = -1;

static void bt_scan_enter_classic(void);
static void bt_scan_enter_le(void);

static void touched(void) { s_generation++; }

static BTScanResult *find_or_add(const uint8_t *addr, uint8_t transport)
{
    unsigned i;
    BTScanResult *r;

    for (i = 0u; i < s_count; i++) {
        if (s_results[i].transport == transport &&
            memcmp(s_results[i].addr, addr, 6) == 0)
            return &s_results[i];
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
    for (i = 0u; i < n; i++) {
        char c = (char)name[i];
        r->name[i] = (c >= 0x20 && c <= 0x7E) ? c : '?';
    }
    r->name[n] = '\0';
    r->name_state = 2u;
    touched();
}

/* Kick the next pending classic remote-name request; advance to the LE
 * phase when none are left. */
static void bt_scan_next_name_request(void)
{
    unsigned i;

    for (i = 0u; i < s_count; i++) {
        BTScanResult *r = &s_results[i];
        if (r->transport == BT_SCAN_TRANSPORT_CLASSIC && r->name_state == 0u) {
            bd_addr_t addr;
            memcpy(addr, r->addr, 6);
            r->name_state = 1u;
            s_name_req_index = (int)i;
            gap_remote_name_request(addr, r->psrm,
                                    (uint16_t)(r->clock_offset | 0x8000u));
            return;
        }
    }
    s_name_req_index = -1;
    bt_scan_enter_le();
}

static void bt_scan_le_phase_timeout(btstack_timer_source_t *ts)
{
    (void)ts;
    if (s_phase != SCAN_LE)
        return;
    gap_stop_scan();
    bt_scan_enter_classic();
}

static void bt_scan_enter_classic(void)
{
    s_phase = SCAN_CLASSIC_INQUIRY;
    touched();
    gap_inquiry_start(BT_SCAN_INQUIRY_1280MS_UNITS);
}

static void bt_scan_enter_le(void)
{
    s_phase = SCAN_LE;
    touched();
    /* Active scan so devices answer with scan responses (names). */
    gap_set_scan_parameters(1u, 0x0030u, 0x0030u);
    gap_start_scan();
    btstack_run_loop_set_timer(&s_le_phase_timer, BT_SCAN_LE_PHASE_MS);
    btstack_run_loop_set_timer_handler(&s_le_phase_timer, bt_scan_le_phase_timeout);
    btstack_run_loop_add_timer(&s_le_phase_timer);
}

static void bt_scan_handle_adv_report(uint8_t *packet)
{
    bd_addr_t addr;
    BTScanResult *r;
    const uint8_t *data;
    uint8_t data_len;
    ad_context_t ctx;

    gap_event_advertising_report_get_address(packet, addr);
    r = find_or_add(addr, BT_SCAN_TRANSPORT_LE);
    if (!r)
        return;

    r->addr_type = gap_event_advertising_report_get_address_type(packet);
    r->rssi      = (int8_t)gap_event_advertising_report_get_rssi(packet);

    data     = gap_event_advertising_report_get_data(packet);
    data_len = gap_event_advertising_report_get_data_length(packet);

    for (ad_iterator_init(&ctx, data_len, data);
         ad_iterator_has_more(&ctx);
         ad_iterator_next(&ctx)) {
        uint8_t type = ad_iterator_get_data_type(&ctx);
        if (type == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME ||
            (type == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME &&
             r->name[0] == '\0')) {
            set_name(r, ad_iterator_get_data(&ctx),
                     ad_iterator_get_data_len(&ctx));
        }
    }
    touched();
}

static void bt_scan_handle_inquiry_result(uint8_t *packet)
{
    bd_addr_t addr;
    BTScanResult *r;

    gap_event_inquiry_result_get_bd_addr(packet, addr);
    r = find_or_add(addr, BT_SCAN_TRANSPORT_CLASSIC);
    if (!r)
        return;

    r->cod          = gap_event_inquiry_result_get_class_of_device(packet);
    r->psrm         = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    r->clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
    if (gap_event_inquiry_result_get_rssi_available(packet))
        r->rssi = (int8_t)gap_event_inquiry_result_get_rssi(packet);
    if (gap_event_inquiry_result_get_name_available(packet)) {
        set_name(r, gap_event_inquiry_result_get_name(packet),
                 gap_event_inquiry_result_get_name_len(packet));
    }
    touched();
}

static void bt_scan_packet_handler(uint8_t packet_type, uint16_t channel,
                                   uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || s_phase == SCAN_OFF)
        return;

    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (s_phase == SCAN_WAIT_STACK &&
            btstack_event_state_get_state(packet) == HCI_STATE_WORKING)
            bt_scan_enter_classic();
        break;

    case GAP_EVENT_INQUIRY_RESULT:
        bt_scan_handle_inquiry_result(packet);
        break;

    case GAP_EVENT_INQUIRY_COMPLETE:
        if (s_phase == SCAN_CLASSIC_INQUIRY) {
            s_phase = SCAN_CLASSIC_NAMES;
            touched();
            bt_scan_next_name_request();
        }
        break;

    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        if (s_phase == SCAN_CLASSIC_NAMES && s_name_req_index >= 0) {
            BTScanResult *r = &s_results[s_name_req_index];
            /* status byte at [2], bd_addr at [3], name at [9] */
            if (packet[2] == 0u) {
                const uint8_t *name = &packet[9];
                unsigned len = 0u;
                while (len < 240u && name[len] != 0u) len++;
                set_name(r, name, len);
            } else {
                r->name_state = 2u;   /* gave up */
            }
            bt_scan_next_name_request();
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
    s_name_req_index = -1;
    touched();

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
