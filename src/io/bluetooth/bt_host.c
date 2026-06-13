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

// HAL declarations
void bt_hal_raspi3_poll_uart(void);
uint32_t bt_hal_raspi3_io_activity(void);
void bt_hal_raspi3_rx_pending(uint32_t *filled, uint32_t *wanted);
void bt_hal_raspi3_flush_rx(void);
void bt_hal_raspi3_trace_dump(void);
void bt_hal_raspi3_trace_reset(void);
const btstack_uart_block_t * btstack_uart_block_embedded_instance(void);
bool pl011_backend_route_header_console(void);
bool pl011_backend_route_bluetooth_pi3(void);
void pl011_backend_wait_idle(void);
uint32_t pl011_backend_setup_bt_lpo(uint32_t *old_ctl, uint32_t *old_div);

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
static bool s_bt_console_handed_off = false;

/* HID host descriptor storage — used for report-protocol devices.
 * In boot protocol mode this is largely unused, but hid_host_init() requires it. */
#define BT_HID_DESCRIPTOR_STORAGE_SIZE 512u
static uint8_t s_hid_descriptor_storage[BT_HID_DESCRIPTOR_STORAGE_SIZE];

/* cid → device-type table for routing HID reports in hid_packet_handler */
#define BT_HID_CID_TABLE_MAX 4u
typedef struct { uint16_t cid; uint8_t dtype; } BTHIDCIDEntry;
static BTHIDCIDEntry s_hid_cid_table[BT_HID_CID_TABLE_MAX];
static unsigned      s_hid_cid_count;

static void hid_cid_register(uint16_t cid, uint8_t dtype)
{
    if (s_hid_cid_count >= BT_HID_CID_TABLE_MAX) return;
    s_hid_cid_table[s_hid_cid_count].cid   = cid;
    s_hid_cid_table[s_hid_cid_count].dtype = dtype;
    s_hid_cid_count++;
}
static uint8_t hid_cid_type(uint16_t cid)
{
    for (unsigned i = 0u; i < s_hid_cid_count; i++)
        if (s_hid_cid_table[i].cid == cid) return s_hid_cid_table[i].dtype;
    return BT_PAIRS_TYPE_UNKNOWN;
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
static void bt_phase2_start(int status);
static void bt_setup_hci_main(BTHost *bt);
static void bt_hardware_error_callback(uint8_t error_code);

/* While BT owns the PL011, kprintf is redirected to the mini-UART on the
 * same header pins (GPIO 14/15 ALT5, 115200 8N1) — boot logs keep flowing
 * to the user's serial adapter.  Requires enable_uart=1 in config.txt so
 * the firmware pins core_freq (the mini-UART baud divisor tracks it). */
#include "io/serial/miniuart_backend.h"
void kprintf_set_putc_override(void (*fn)(char chr));

static MiniUartBackend s_console_miniuart;

/* kprintf must never block while BT owns the PL011: a busy-wait on the
 * 8-deep mini-UART FIFO costs ~87µs per byte at 115200, and a single log
 * line opens a multi-ms blind window in which HCI RX bytes are lost (the
 * chip ignores our RTS).  So the putc override only deposits into this
 * ring; bt_console_drain() — called from the BT pump — moves bytes to the
 * FIFO when there is space. */
#define BT_CONSOLE_RING_SIZE 32768u
static volatile uint8_t s_console_ring[BT_CONSOLE_RING_SIZE];
static volatile uint32_t s_console_ring_head; /* producer (kprintf) */
static volatile uint32_t s_console_ring_tail; /* consumer (drain)   */
static volatile uint32_t s_console_ring_dropped;

static void bt_console_ring_push(uint8_t byte)
{
    uint32_t head = s_console_ring_head;
    uint32_t next = (head + 1u) % BT_CONSOLE_RING_SIZE;
    if (next == s_console_ring_tail) {
        s_console_ring_dropped++;
        return;
    }
    s_console_ring[head] = byte;
    s_console_ring_head = next;
}

static void bt_console_ring_putc(char chr)
{
    if (chr == '\n') {
        bt_console_ring_push((uint8_t)'\r');
    }
    bt_console_ring_push((uint8_t)chr);
}

static void bt_console_drain(void)
{
    if (!miniuart_backend_is_open(&s_console_miniuart)) {
        return;
    }
    /* LSR bit 5 = TX FIFO has space */
    while (s_console_ring_tail != s_console_ring_head
           && (miniuart_backend_read_lsr() & 0x20u)) {
        miniuart_backend_write_byte(&s_console_miniuart,
                                    s_console_ring[s_console_ring_tail]);
        s_console_ring_tail = (s_console_ring_tail + 1u) % BT_CONSOLE_RING_SIZE;
    }
}

static void bt_console_flush(uint32_t budget_bytes)
{
    /* Blocking flush — only safe outside BT traffic (release path). */
    while (s_console_ring_tail != s_console_ring_head && budget_bytes--) {
        int spin = 1000000;
        while (!(miniuart_backend_read_lsr() & 0x20u) && --spin > 0) { }
        miniuart_backend_write_byte(&s_console_miniuart,
                                    s_console_ring[s_console_ring_tail]);
        s_console_ring_tail = (s_console_ring_tail + 1u) % BT_CONSOLE_RING_SIZE;
    }
}

static uint32_t vc_get_core_clock_hz(void);

static void bt_console_release(void)
{
    if (!s_bt_console_handed_off) {
        return;
    }

    kprintf_set_putc_override(NULL);
    bt_console_flush(BT_CONSOLE_RING_SIZE);
    miniuart_backend_close(&s_console_miniuart);
    pl011_backend_route_header_console();
    kprintf_set_enabled(1);
    s_bt_console_handed_off = false;
    if (s_console_ring_dropped) {
        kprintf("[BT] console ring dropped %u bytes during BT operation\n",
                (unsigned)s_console_ring_dropped);
        s_console_ring_dropped = 0;
    }
    bt_hal_raspi3_trace_dump();
}

static void bt_console_handoff(void)
{
    if (s_bt_console_handed_off) {
        return;
    }

    bt_hal_raspi3_trace_reset();
    uint32_t core_hz = vc_get_core_clock_hz();
    if (core_hz == 0) {
        core_hz = 250000000u;
    }
    kprintf("[BT] handing PL011 to BT; console continues on mini-UART "
            "(same pins, core clk %u Hz)\n", (unsigned)core_hz);
    pl011_backend_wait_idle();
    pl011_backend_route_bluetooth_pi3();
    s_console_ring_head = 0;
    s_console_ring_tail = 0;
    s_console_ring_dropped = 0;
    if (miniuart_backend_open_clk(&s_console_miniuart, 115200u, core_hz)) {
        kprintf_set_putc_override(bt_console_ring_putc);
        kprintf("[BT] console now on mini-UART (GPIO 14/15 ALT5, 115200)\n");
        bt_console_flush(BT_CONSOLE_RING_SIZE);
    } else {
        kprintf_set_enabled(0);
    }
    s_bt_console_handed_off = true;
}

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
#define VC_MBOX_CH_PROP 8u
#define VC_MBOX_TX_FULL 0x80000000u
#define VC_MBOX_RX_EMPTY 0x40000000u
#define VC_MBOX_CHANMASK 0xFu
#define VC_FIRMWARE_STATUS_REQUEST 0u
#define VC_FIRMWARE_STATUS_SUCCESS 0x80000000u
#define VC_FIRMWARE_PROPERTY_END 0u
#define VC_FIRMWARE_SET_GPIO_STATE 0x00038041u
#define VC_FIRMWARE_GET_CLOCK_RATE 0x00030002u
#define VC_CLOCK_ID_CORE 4u
#define ARM_PERI_VIRT_BASE 0xF2000000u
#define VC_MBOX_READ_ADDR   (ARM_PERI_VIRT_BASE + 0xB880u)
#define VC_MBOX_STATUS_ADDR (ARM_PERI_VIRT_BASE + 0xB898u)
#define VC_MBOX_WRITE_ADDR  (ARM_PERI_VIRT_BASE + 0xB8A0u)

typedef enum BTBootstrapState {
    BT_BOOTSTRAP_IDLE = 0,
    BT_BOOTSTRAP_RESET_ASSERTED,
    BT_BOOTSTRAP_WAIT_FOR_BOOT_ROM,
    BT_BOOTSTRAP_WAIT_FOR_PHASE1,
    BT_BOOTSTRAP_WAIT_FOR_WORKING,
    BT_BOOTSTRAP_WORKING,
    BT_BOOTSTRAP_FAILED
} BTBootstrapState;

static uint32_t vc_property_buffer[8] __attribute__((aligned(16)));

static uint32_t bt_now_ms(void)
{
    return hal_time_ms();
}

/* Bounded mailbox receive — a missing firmware response must degrade to the
 * warm-boot fallback, not hang the whole boot with the console dark. */
#define VC_MBOX_RECV_SPINS 4000000u

static bool vc_mbox_recv(uint32_t channel, uint32_t *out)
{
    uint32_t response;
    uint32_t spins = VC_MBOX_RECV_SPINS;

    do {
        while (rd32le(VC_MBOX_STATUS_ADDR) & VC_MBOX_RX_EMPTY) {
            dsb();
            if (--spins == 0u) {
                bt_diag_log("[BT] vc mailbox recv timeout (ch=%u)\n",
                            (unsigned)channel);
                return false;
            }
        }

        dmb();
        response = rd32le(VC_MBOX_READ_ADDR);
        dmb();
    } while ((response & VC_MBOX_CHANMASK) != channel);

    if (out) {
        *out = response & ~VC_MBOX_CHANMASK;
    }
    return true;
}

static void vc_mbox_send(uint32_t channel, uint32_t data)
{
    uint32_t value = (data & ~VC_MBOX_CHANMASK) | (channel & VC_MBOX_CHANMASK);

    while (rd32le(VC_MBOX_STATUS_ADDR) & VC_MBOX_TX_FULL) {
        dsb();
    }

    dmb();
    wr32le(VC_MBOX_WRITE_ADDR, value);
}

/* Real VPU/core clock — the mini-UART baud divisor tracks it, so never
 * assume 250 MHz (garbled console when the firmware runs the core at 400). */
static uint32_t vc_get_core_clock_hz(void)
{
    vc_property_buffer[0] = LE32(sizeof(vc_property_buffer));
    vc_property_buffer[1] = LE32(VC_FIRMWARE_STATUS_REQUEST);
    vc_property_buffer[2] = LE32(VC_FIRMWARE_GET_CLOCK_RATE);
    vc_property_buffer[3] = LE32(8);
    vc_property_buffer[4] = 0;
    vc_property_buffer[5] = LE32(VC_CLOCK_ID_CORE);
    vc_property_buffer[6] = 0;
    vc_property_buffer[7] = LE32(VC_FIRMWARE_PROPERTY_END);

    arm_flush_cache((uintptr_t)vc_property_buffer, sizeof(vc_property_buffer));
    vc_mbox_send(VC_MBOX_CH_PROP, (uint32_t)mmu_virt2phys((uintptr_t)vc_property_buffer));
    if (!vc_mbox_recv(VC_MBOX_CH_PROP, NULL)) {
        return 0;
    }
    arm_dcache_invalidate((uintptr_t)vc_property_buffer, sizeof(vc_property_buffer));

    if (LE32(vc_property_buffer[1]) != VC_FIRMWARE_STATUS_SUCCESS) {
        return 0;
    }
    return LE32(vc_property_buffer[6]);
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
                bt_console_handoff();
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
                bt_console_release();
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
        bt_console_release();
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
                    bt_pairing_window_open(s_bt_host);
                    /* bt_pairs is populated only after launcher_run() reads BTPAIRS.TXT.
                     * Recovery-triggered WORKING events fire during emulation when bt_pairs
                     * is already in memory — reconnect then.  Initial connect is done by
                     * bt_host_connect_pairs() called from bellatrix.c after launcher_run(). */
                    if (bt_pairs_count() > 0u) {
                        for (unsigned i = 0u; i < bt_pairs_count(); i++) {
                            const BTPair *p = bt_pairs_get(i);
                            if (!p) continue;
                            bd_addr_t addr;
                            memcpy(addr, p->addr, 6u);
                            uint16_t cid = 0u;
                            int ret = hid_host_connect(addr, HID_PROTOCOL_MODE_BOOT, &cid);
                            bt_diag_log("[BT] hid_host_connect (recovery) pair[%u] %s type=%u -> cid=0x%04x ret=%d\n",
                                        i, bd_addr_to_str(addr),
                                        (unsigned)p->device_type, (unsigned)cid, ret);
                        }
                    }
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
            if (s_bt_host && s_bt_host->pairing_window_open) {
                kprintf("[BT] pin code request from %s -> using 0000\n", bd_addr_to_str(event_addr));
                hci_send_cmd(&hci_pin_code_request_reply, &event_addr, 4, "0000");
            } else {
                kprintf("[BT] pin code request from %s denied (pairing window closed)\n", bd_addr_to_str(event_addr));
                hci_send_cmd(&hci_pin_code_request_negative_reply, &event_addr);
            }
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, event_addr);
            if (s_bt_host && s_bt_host->pairing_window_open) {
                kprintf("[BT] user confirmation request from %s -> accepted\n", bd_addr_to_str(event_addr));
                hci_send_cmd(&hci_user_confirmation_request_reply, &event_addr);
            } else {
                kprintf("[BT] user confirmation request from %s denied (pairing window closed)\n", bd_addr_to_str(event_addr));
                hci_send_cmd(&hci_user_confirmation_request_negative_reply, &event_addr);
            }
            break;
        default:
            break;
    }
}

void bt_host_connect_pairs(BTHost *bt)
{
    if (!bt || !bt_host_is_working(bt)) return;
    unsigned n = bt_pairs_count();
    if (n == 0u) {
        bt_diag_log("[BT] connect_pairs: no pairs saved\n");
        return;
    }
    for (unsigned i = 0u; i < n; i++) {
        const BTPair *p = bt_pairs_get(i);
        if (!p) continue;
        bd_addr_t addr;
        memcpy(addr, p->addr, 6u);
        uint16_t cid = 0u;
        int ret = hid_host_connect(addr, HID_PROTOCOL_MODE_BOOT, &cid);
        bt_diag_log("[BT] connect_pairs[%u] %s type=%u -> cid=0x%04x ret=%d\n",
                    i, bd_addr_to_str(addr),
                    (unsigned)p->device_type, (unsigned)cid, ret);
    }
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
        case HID_SUBEVENT_CONNECTION_OPENED:
            hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            status  = hid_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                bt_diag_log("[BT] HID connect failed: cid=0x%04x status=0x%02x\n",
                            (unsigned)hid_cid, (unsigned)status);
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
                bt_diag_log("[BT] HID connected: %s type=%u cid=0x%04x\n",
                            bd_addr_to_str(addr), (unsigned)dtype, (unsigned)hid_cid);
            }
            break;

        case HID_SUBEVENT_REPORT: {
            hid_cid = hid_subevent_report_get_hid_cid(packet);
            const uint8_t *data = hid_subevent_report_get_report(packet);
            uint16_t len = (uint16_t)hid_subevent_report_get_report_len(packet);
            uint8_t rtype = hid_cid_type(hid_cid);
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

        case HID_SUBEVENT_CONNECTION_CLOSED:
            hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            bt_diag_log("[BT] HID disconnected: cid=0x%04x\n", (unsigned)hid_cid);
            bt_hid_release_all(hid_cid);
            hid_cid_unregister(hid_cid);
            break;

        default:
            break;
    }
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

    /* 1s heartbeat with raw state-machine values — survives in the bt_diag
     * RAM ring even when the console is handed to the controller, and is
     * the primary clue when the bootstrap silently stalls. */
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
            bt_diag_log("[BT] wait: state=%u now=%u bdl=%u idl=%u p1=%u hci=%u\n",
                        (unsigned)bt->bootstrap_state, (unsigned)now_ms,
                        (unsigned)bt->bootstrap_deadline_ms,
                        (unsigned)bt->init_deadline_ms,
                        (unsigned)bt->phase1_complete,
                        (unsigned)bt->hci_ready);
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

static uint32_t s_link_recoveries;

static void bt_link_recover(BTHost *bt, const char *reason)
{
    if (s_link_recoveries >= BT_LINK_MAX_RECOVERIES) {
        bt_diag_log("[BT] %s, but recovery budget exhausted (%u)\n",
                    reason, (unsigned)s_link_recoveries);
        return;
    }
    s_link_recoveries++;
    bt_diag_log("[BT] %s - full power cycle %u/%u\n", reason,
                (unsigned)s_link_recoveries, (unsigned)BT_LINK_MAX_RECOVERIES);

    /* Park the scan phase machine before touching the stack: its watchdog and
     * LE timers would otherwise fire gap_inquiry_stop()/gap_stop_scan() during
     * HCI re-initialisation, corrupting the init sequence and preventing the
     * stack from ever reaching WORKING after the power cycle. */
    bt_scan_notify_recovery();

    /* Force the stack to OFF *synchronously*.  A single POWER_OFF from
     * WORKING enters the graceful HALTING path, which needs answers from
     * the (dead) chip and closes the transport asynchronously — last
     * round that close landed mid PatchRAM upload and killed phase 1.
     * Public-API workaround using btstack's own state machine:
     *   OFF: WORKING → HALTING        (async, don't wait for it)
     *   ON : HALTING → INITIALIZING   (immediate; resets num_cmd_packets,
     *                                  freeing the stuck command slot)
     *   OFF: INITIALIZING → OFF       (hci_power_control_off(): closes
     *                                  the transport right here, sync)
     * Same off/on pattern btstack itself uses on HCI_EVENT_HARDWARE_ERROR. */
    hci_power_control(HCI_POWER_OFF);
    hci_power_control(HCI_POWER_ON);
    hci_power_control(HCI_POWER_OFF);

    bt_hal_raspi3_flush_rx();
    /* Force phase 1 to run again and rewind the chipset init script so
     * the PatchRAM upload starts from record zero. */
    bt->phase1_complete = false;
    bt->power_cycle_attempts = 0;
    btstack_chipset_bcm_instance()->init(&bt_transport_config);
    bt_begin_power_cycle(bt, "link recovery");
}

/* The chip can also confess on its own: HCI Hardware Error (evt 0x10).
 * btstack's default reaction is a bare off+on, which would skip the
 * PatchRAM upload — route it to the full recovery instead. */
static void bt_hardware_error_callback(uint8_t error_code)
{
    char reason[48];
    unsigned i = 0u;
    static const char prefix[] = "hardware error 0x";
    static const char hexd[] = "0123456789abcdef";

    for (; prefix[i]; i++) reason[i] = prefix[i];
    reason[i++] = hexd[(error_code >> 4) & 0xF];
    reason[i++] = hexd[error_code & 0xF];
    reason[i] = '\0';

    if (s_bt_host)
        bt_link_recover(s_bt_host, reason);
}

static void bt_link_watchdog(BTHost *bt)
{
    static uint32_t stall_filled;
    static uint32_t stall_since_ms;
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
        return;
    }
    if (now_ms - stall_since_ms < BT_LINK_STALL_MS)
        return;

    stall_since_ms = 0u;
    bt_diag_log("[BT] link stalled (rxq=%u/%u for %u ms)\n",
                (unsigned)filled, (unsigned)wanted, (unsigned)BT_LINK_STALL_MS);
    bt_link_recover(bt, "link stalled");
}

void bt_host_step(BTHost *bt) {
    if (!bt || !bt->enabled) return;

    bt_bootstrap_step(bt);

    // Poll UART HAL (simulates interrupts)
    bt_hal_raspi3_poll_uart();

    // Execute run loop tasks
    btstack_run_loop_embedded_execute_once();

    // Detect and recover a desynced/dead H4 link
    bt_link_watchdog(bt);

    // Move buffered console bytes to the mini-UART (never blocks)
    bt_console_drain();
}

void bt_host_shutdown(BTHost *bt) {
    if (!bt || !bt->initialized) return;
    
    kprintf("[BT] shutdown\n");
    btstack_run_loop_remove_timer(&bt_pairing_window_timer);
    bt_pairing_window_close(bt);
    hci_power_control(HCI_POWER_OFF);
    bt_console_release();
    bt->enabled = false;
    bt->bootstrap_state = BT_BOOTSTRAP_IDLE;
    bt->bootstrap_deadline_ms = 0;
    bt->init_deadline_ms = 0;
    bt->hci_ready = false;
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

void bt_host_step(BTHost *bt) {
    (void)bt;
}

void bt_host_shutdown(BTHost *bt) {
    (void)bt;
}
#endif
