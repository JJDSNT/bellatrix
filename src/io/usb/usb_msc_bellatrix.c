#include "io/usb/usb_msc_bellatrix.h"
#include "io/usb/usb_host.h"
#include "support.h"

#if BELLATRIX_ENABLE_USBSTACK && BELLATRIX_ENABLE_USB_MSC

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
    if (!usb_host_stack_enter("msc-read10")) {
        kprintf("[USB-MSC] read10 lba=%u rejected: USB host unavailable\n",
                (unsigned)lba);
        return false;
    }
    int ret = usbh_msc_scsi_read10(msc, lba, (const uint8_t *)buf, 1u);
    usb_host_stack_leave();
    if (ret != 0) {
        kprintf("[USB-MSC] read10 lba=%u failed: %d\n", (unsigned)lba, ret);
        return false;
    }
    return true;
}

bool fat32_init_usb(Fat32State *fs)
{
    if (!g_usb_msc || !fs) return false;
    return fat32_init_with_reader(fs, usb_msc_read_block, g_usb_msc);
}

uint32_t usb_glue_vc_get_throttled(void);

void usbh_msc_run(struct usbh_msc *msc_class)
{
    if (!msc_class) return;

    int ret = usbh_msc_scsi_init(msc_class);
    if (ret < 0) {
        kprintf("[USB-MSC] scsi_init failed: %d\n", ret);
        return;
    }

    kprintf("[USB-MSC] drive ready: %u blocks x %u B/block (throttled=0x%08x)\n",
            (unsigned)msc_class->blocknum, (unsigned)msc_class->blocksize,
            (unsigned)usb_glue_vc_get_throttled());

    g_usb_msc = msc_class;
}

void usbh_msc_stop(struct usbh_msc *msc_class)
{
    if (g_usb_msc == msc_class) {
        g_usb_msc = NULL;
    }
    kprintf("[USB-MSC] drive removed (throttled=0x%08x)\n",
            (unsigned)usb_glue_vc_get_throttled());
}

#else

bool usb_msc_is_ready(void) { return false; }
bool fat32_init_usb(Fat32State *fs) { (void)fs; return false; }

#endif
