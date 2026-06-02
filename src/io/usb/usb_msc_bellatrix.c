#include "io/usb/usb_msc_bellatrix.h"
#include "support.h"

#if BELLATRIX_ENABLE_USBSTACK

#include "usb_config.h"
#include "usbh_msc.h"

static struct usbh_msc *g_usb_msc = NULL;

bool usb_msc_is_ready(void)
{
    return g_usb_msc != NULL;
}

static bool usb_msc_read_block(void *ctx, uint32_t lba, uint8_t *buf)
{
    struct usbh_msc *msc = (struct usbh_msc *)ctx;
    return usbh_msc_scsi_read10(msc, lba, (const uint8_t *)buf, 1u) == 0;
}

bool fat32_init_usb(Fat32State *fs)
{
    if (!g_usb_msc || !fs) return false;
    return fat32_init_with_reader(fs, usb_msc_read_block, g_usb_msc);
}

void usbh_msc_run(struct usbh_msc *msc_class)
{
    if (!msc_class) return;

    int ret = usbh_msc_scsi_init(msc_class);
    if (ret < 0) {
        kprintf("[USB-MSC] scsi_init failed: %d\n", ret);
        return;
    }

    kprintf("[USB-MSC] drive ready: %u blocks x %u B/block\n",
            (unsigned)msc_class->blocknum, (unsigned)msc_class->blocksize);

    g_usb_msc = msc_class;
}

void usbh_msc_stop(struct usbh_msc *msc_class)
{
    if (g_usb_msc == msc_class) {
        g_usb_msc = NULL;
    }
    kprintf("[USB-MSC] drive removed\n");
}

#else

bool usb_msc_is_ready(void) { return false; }
bool fat32_init_usb(Fat32State *fs) { (void)fs; return false; }

#endif
