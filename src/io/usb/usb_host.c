#include "io/usb/usb_host.h"

#include "support.h"

#if BELLATRIX_ENABLE_USBSTACK
#include "usbh_core.h"
#include "usbh_hub.h"
#include "usb_dwc2_reg.h"

#define BELLATRIX_USB_BUS_ID 0u
#define BELLATRIX_USB_DWC2_REG_BASE 0xF2980000UL
#define BELLATRIX_USB_HPRT_ADDR (BELLATRIX_USB_DWC2_REG_BASE + USB_OTG_HOST_PORT_BASE)

static void usbh_bellatrix_poll(uint8_t busid)
{
    USBH_IRQHandler(busid);
}

static const char *bellatrix_usb_event_name(uint8_t event)
{
    switch (event) {
    case USBH_EVENT_ERROR: return "error";
    case USBH_EVENT_SOF: return "sof";
    case USBH_EVENT_DEVICE_RESET: return "device_reset";
    case USBH_EVENT_DEVICE_CONNECTED: return "device_connected";
    case USBH_EVENT_DEVICE_DISCONNECTED: return "device_disconnected";
    case USBH_EVENT_DEVICE_CONFIGURED: return "device_configured";
    case USBH_EVENT_DEVICE_WAKEUP: return "device_wakeup";
    case USBH_EVENT_DEVICE_SUSPEND: return "device_suspend";
    case USBH_EVENT_DEVICE_RESUME: return "device_resume";
    case USBH_EVENT_INTERFACE_UNSUPPORTED: return "interface_unsupported";
    case USBH_EVENT_INTERFACE_START: return "interface_start";
    case USBH_EVENT_INTERFACE_STOP: return "interface_stop";
    case USBH_EVENT_INIT: return "init";
    case USBH_EVENT_DEINIT: return "deinit";
    default: return "unknown";
    }
}

static uint32_t bellatrix_usb_read_hprt(void)
{
    return rd32le((uintptr_t)BELLATRIX_USB_HPRT_ADDR);
}

static void bellatrix_usb_log_hprt(USBHost *host, const char *reason)
{
    uint32_t hprt;

    if (!host) {
        return;
    }

    hprt = bellatrix_usb_read_hprt();
    if (host->last_hprt_valid && host->last_hprt == hprt) {
        return;
    }

    host->last_hprt = hprt;
    host->last_hprt_valid = true;

    kprintf("[USB] HPRT (%s): raw=0x%08x conn=%u ena=%u enchg=%u ovcchg=%u rst=%u pwr=%u spd=%u\n",
            reason,
            (unsigned)hprt,
            (unsigned)((hprt & USB_OTG_HPRT_PCSTS) != 0u),
            (unsigned)((hprt & USB_OTG_HPRT_PENA) != 0u),
            (unsigned)((hprt & USB_OTG_HPRT_PENCHNG) != 0u),
            (unsigned)((hprt & USB_OTG_HPRT_POCCHNG) != 0u),
            (unsigned)((hprt & USB_OTG_HPRT_PRST) != 0u),
            (unsigned)((hprt & USB_OTG_HPRT_PPWR) != 0u),
            (unsigned)((hprt & USB_OTG_HPRT_PSPD) >> USB_OTG_HPRT_PSPD_Pos));
}

static void bellatrix_usb_event(uint8_t busid, uint8_t hub_index, uint8_t hub_port, uint8_t intf, uint8_t event)
{
    kprintf("[USB] event bus=%u hub=%u port=%u intf=%u event=%u (%s)\n",
            (unsigned)busid,
            (unsigned)hub_index,
            (unsigned)hub_port,
            (unsigned)intf,
            (unsigned)event,
            bellatrix_usb_event_name(event));
}

bool usb_host_init(USBHost *host)
{
    if (!host) {
        return false;
    }

    host->enabled = true;
    host->initialized = true;
    host->stack_linked = true;
    host->controller_ready = false;
    host->poll_count = 0;
    host->last_hprt = 0;
    host->last_hprt_valid = false;

    if (usbh_initialize(BELLATRIX_USB_BUS_ID, BELLATRIX_USB_DWC2_REG_BASE, bellatrix_usb_event) == 0) {
        host->controller_ready = true;
        usbh_hub_poll(&g_usbhost_bus[BELLATRIX_USB_BUS_ID]);
        usbh_bellatrix_poll(BELLATRIX_USB_BUS_ID);
        kprintf("[USB] CherryUSB DWC2 host initialized at %p\n",
                (void *)BELLATRIX_USB_DWC2_REG_BASE);
        bellatrix_usb_log_hprt(host, "post-init");
    } else {
        kprintf("[USB] CherryUSB DWC2 host initialization failed\n");
    }

    return true;
}

void usb_host_step(USBHost *host)
{
    if (!host || !host->enabled || !host->initialized) {
        return;
    }

    host->poll_count++;

    if (host->controller_ready) {
        usbh_hub_poll(&g_usbhost_bus[BELLATRIX_USB_BUS_ID]);
        usbh_bellatrix_poll(BELLATRIX_USB_BUS_ID);
        USBH_IRQHandler(BELLATRIX_USB_BUS_ID);
        usbh_hub_poll(&g_usbhost_bus[BELLATRIX_USB_BUS_ID]);
        if (host->poll_count <= 16u || (host->poll_count % 4096u) == 0u) {
            bellatrix_usb_log_hprt(host, "poll");
        }
    }
}

void usb_host_shutdown(USBHost *host)
{
    if (!host || !host->initialized) {
        return;
    }

    if (host->controller_ready) {
        usbh_hub_poll(&g_usbhost_bus[BELLATRIX_USB_BUS_ID]);
        usbh_deinitialize(BELLATRIX_USB_BUS_ID);
        usbh_hub_poll(&g_usbhost_bus[BELLATRIX_USB_BUS_ID]);
        usbh_bellatrix_poll(BELLATRIX_USB_BUS_ID);
    }

    kprintf("[USB] shutdown\n");
    host->enabled = false;
    host->initialized = false;
    host->stack_linked = false;
    host->controller_ready = false;
    host->last_hprt = 0;
    host->last_hprt_valid = false;
}

#else

bool usb_host_init(USBHost *host)
{
    if (!host) {
        return false;
    }

    host->enabled = false;
    host->initialized = false;
    host->stack_linked = false;
    host->controller_ready = false;
    host->poll_count = 0;
    host->last_hprt = 0;
    host->last_hprt_valid = false;
    return true;
}

void usb_host_step(USBHost *host)
{
    (void)host;
}

void usb_host_shutdown(USBHost *host)
{
    (void)host;
}

#endif
