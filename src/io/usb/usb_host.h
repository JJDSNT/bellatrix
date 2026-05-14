#ifndef BELLATRIX_IO_USB_USB_HOST_H
#define BELLATRIX_IO_USB_USB_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct USBHost {
    bool enabled;
    bool initialized;
    bool stack_linked;
    bool controller_ready;
    uint32_t poll_count;
    uint32_t last_hprt;
    bool last_hprt_valid;
} USBHost;

bool usb_host_init(USBHost *host);
void usb_host_step(USBHost *host);
void usb_host_shutdown(USBHost *host);

#endif
