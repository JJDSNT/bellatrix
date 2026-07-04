#ifndef BELLATRIX_IO_USB_USB_CONFIG_H
#define BELLATRIX_IO_USB_USB_CONFIG_H

#include <stddef.h>

#include "support.h"

int bellatrix_usb_snprintf(char *str, size_t size, const char *format, ...);
long bellatrix_usb_strtol(const char *nptr, char **endptr, int base);

#define CONFIG_USB_PRINTF(...) kprintf(__VA_ARGS__)
#define CONFIG_USB_DBG_LEVEL USB_DBG_ERROR

#define snprintf bellatrix_usb_snprintf
#define strtol bellatrix_usb_strtol

#define CONFIG_USB_DCACHE_ENABLE
/* ISSUE-0036: must match the Cortex-A53 D-cache line size (64 bytes), not
 * 32. USB_NOCACHE_RAM_SECTION is a no-op below (these buffers are regular
 * cached .bss, not an actually-uncached region) -- CherryUSB compensates
 * with explicit usb_dcache_clean()/usb_dcache_invalidate() calls around
 * every transfer instead. Those calls operate at cache-line granularity;
 * with only 32-byte alignment, a buffer's first/last 32 bytes can share a
 * 64-byte cache line with a completely unrelated adjacent global.
 * usb_dcache_invalidate() (used on every IN transfer, e.g. the very first
 * GET_DESCRIPTOR read in usbh_enumerate()) discards a cache line without
 * writing it back -- if that line also holds not-yet-flushed data from an
 * unrelated variable, that data is silently dropped. Confirmed on hardware:
 * mini-UART console output corrupts starting exactly at the first USB IN
 * control transfer. 64-byte alignment guarantees each buffer starts on its
 * own cache line, closing that false-sharing window. */
#define CONFIG_USB_ALIGN_SIZE 64
#define USB_NOCACHE_RAM_SECTION

#define CONFIG_USBHOST_MAX_BUS               1
#define CONFIG_USBHOST_MAX_RHPORTS           1
#define CONFIG_USBHOST_MAX_EXTHUBS           1
#define CONFIG_USBHOST_MAX_EHPORTS           8
#define CONFIG_USBHOST_MAX_HID_CLASS         2
#define CONFIG_USBHOST_MAX_MSC_CLASS         1
#define CONFIG_USBHOST_MSC_TIMEOUT           5000
#define CONFIG_USBHOST_MAX_INTERFACES        8
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS  2
#define CONFIG_USBHOST_MAX_ENDPOINTS         4
#define CONFIG_USBHOST_DEV_NAMELEN           16

#define CONFIG_USBHOST_PSC_PRIO              10
#define CONFIG_USBHOST_PSC_STACKSIZE         2048
#define CONFIG_USBHOST_REQUEST_BUFFER_LEN    512
#define CONFIG_USBHOST_CONTROL_TRANSFER_TIMEOUT 500

#define CONFIG_USBHOST_BELLATRIX_POLLING

#endif
