#include "io/bluetooth/bt_host.h"
#include "debug/core_log.h"
#include "support.h"

#include "btstack.h"
#include "btstack_run_loop.h"
#include "btstack_memory.h"
#include "btstack_run_loop_embedded.h"
#include "hci_transport_h4.h"
#include "btstack_uart_block.h"
#include "hci.h"
#include "hci_dump.h"

// HAL declarations
void bt_hal_raspi3_poll_uart(void);
const btstack_uart_block_t * btstack_uart_block_embedded_instance(void);

// BCM Firmware stubs (empty for now to allow linking)
const uint8_t  brcm_patchram_buf[] = {};
const int      brcm_patch_ram_length = 0;
const char *   brcm_patch_version = "0.0";

void btstack_chipset_bcm_set_device_name(const char * device_name) {
    (void)device_name;
}

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                bd_addr_t local_addr;
                gap_local_bd_addr(local_addr);
                kprintf("[BT] Stack up and running! BD_ADDR: %s\n", bd_addr_to_str(local_addr));
            }
            break;
        default:
            break;
    }
}

bool bt_host_init(BTHost *bt) {
    if (!bt) return false;

    kprintf("[BT] Initializing BTStack (Raspberry Pi 3B)...\n");

    btstack_memory_init();
    
    // Initialize embedded run loop
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    // Use H4 transport with the embedded UART instance
    hci_init(hci_transport_h4_instance(btstack_uart_block_embedded_instance()), NULL);
    
    // Inform about BTstack state
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    bt->initialized = true;
    bt->enabled = true;

    kprintf("[BT] init OK\n");

    // Start stack
    hci_power_control(HCI_POWER_ON);

    return true;
}

void bt_host_step(BTHost *bt) {
    if (!bt || !bt->enabled) return;
    
    // Poll UART HAL (simulates interrupts)
    bt_hal_raspi3_poll_uart();

    // Execute run loop tasks
    btstack_run_loop_embedded_execute_once();
}

void bt_host_shutdown(BTHost *bt) {
    if (!bt || !bt->initialized) return;
    
    kprintf("[BT] shutdown\n");
    hci_power_control(HCI_POWER_OFF);
    bt->enabled = false;
}
