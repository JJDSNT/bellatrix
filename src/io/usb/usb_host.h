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

    /* ISSUE-0045 phase 0: fail-closed ownership guard around every public
     * entry that can drive CherryUSB/DWC2. The value is core_id + 1 while an
     * executor is inside the stack, zero otherwise. These use __atomic
     * builtins in usb_host.c because this header is shared with C99 code. */
    volatile uint32_t owner_active;
    volatile uint32_t last_owner_core;
    volatile uint32_t ownership_violations;
    volatile uint32_t calls_by_core[4];
    volatile uint32_t owner_depth[4];
} USBHost;

bool usb_host_init(USBHost *host);
void usb_host_step(USBHost *host);
void usb_host_shutdown(USBHost *host);

/* Temporary ISSUE-0045 safety boundary for direct class operations (notably
 * MSC-backed FAT/HDF/ISO reads). Same-core nesting is valid because CherryUSB
 * invokes class callbacks synchronously; cross-core entry fails closed. */
bool usb_host_stack_enter(const char *operation);
void usb_host_stack_leave(void);

#endif
