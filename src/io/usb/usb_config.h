#ifndef BELLATRIX_IO_USB_USB_CONFIG_H
#define BELLATRIX_IO_USB_USB_CONFIG_H

#include <stddef.h>

#include "support.h"

int bellatrix_usb_snprintf(char *str, size_t size, const char *format, ...);
long bellatrix_usb_strtol(const char *nptr, char **endptr, int base);

#define CONFIG_USB_PRINTF(...) kprintf(__VA_ARGS__)
#define CONFIG_USB_DBG_LEVEL USB_DBG_INFO

#define snprintf bellatrix_usb_snprintf
#define strtol bellatrix_usb_strtol

#define CONFIG_USB_DCACHE_ENABLE
#define CONFIG_USB_ALIGN_SIZE 32
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
