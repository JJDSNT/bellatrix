#ifndef BELLATRIX_IO_USB_USB_MSC_BELLATRIX_H
#define BELLATRIX_IO_USB_USB_MSC_BELLATRIX_H

#include <stdbool.h>
#include "storage/fat/fat32.h"

// Returns true once a USB mass-storage device is enumerated and SCSI-ready.
bool usb_msc_is_ready(void);

// Mount a FAT32 filesystem on the attached USB drive.
// Must only be called after usb_msc_is_ready() returns true.
bool fat32_init_usb(Fat32State *fs);

#endif
