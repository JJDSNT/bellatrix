#include "io/usb/usb_host.h"

#include "support.h"

#if BELLATRIX_ENABLE_USBSTACK
#include "usbh_core.h"
#include "usbh_hub.h"
#include "usb_dwc2_reg.h"

void usb_osal_timer_poll(void);

#define BELLATRIX_USB_BUS_ID 0u
#define BELLATRIX_USB_DWC2_REG_BASE 0xF2980000UL
#define BELLATRIX_USB_HPRT_ADDR (BELLATRIX_USB_DWC2_REG_BASE + USB_OTG_HOST_PORT_BASE)

#ifdef BELLATRIX_USB_LOG
#define BELLATRIX_USB_LOGF(...) kprintf(__VA_ARGS__)
#else
#define BELLATRIX_USB_LOGF(...) do { } while (0)
#endif

static void usbh_bellatrix_poll(uint8_t busid)
{
    USBH_IRQHandler(busid);
}

static void bellatrix_usb_pump_events(unsigned int passes)
{
    while (passes-- > 0u) {
        usb_osal_timer_poll();
        usbh_hub_poll(&g_usbhost_bus[BELLATRIX_USB_BUS_ID]);
        usbh_bellatrix_poll(BELLATRIX_USB_BUS_ID);
    }
    usb_osal_timer_poll();
    usbh_hub_poll(&g_usbhost_bus[BELLATRIX_USB_BUS_ID]);
}

static uint32_t bellatrix_usb_read_hprt(void)
{
    return rd32le((uintptr_t)BELLATRIX_USB_HPRT_ADDR);
}

static void bellatrix_usb_log_hprt(USBHost *host, const char *reason)
{
    uint32_t hprt;

#ifndef BELLATRIX_USB_LOG
    (void)reason;
#endif

    if (!host) {
        return;
    }

    hprt = bellatrix_usb_read_hprt();
    if (host->last_hprt_valid && host->last_hprt == hprt) {
        return;
    }

    host->last_hprt = hprt;
    host->last_hprt_valid = true;

    BELLATRIX_USB_LOGF("[USB] HPRT (%s): raw=0x%08x conn=%u ena=%u enchg=%u ovcchg=%u rst=%u pwr=%u spd=%u\n",
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
#ifdef BELLATRIX_USB_LOG
    const char *event_name;

    switch (event) {
    case USBH_EVENT_ERROR: event_name = "error"; break;
    case USBH_EVENT_SOF: event_name = "sof"; break;
    case USBH_EVENT_DEVICE_RESET: event_name = "device_reset"; break;
    case USBH_EVENT_DEVICE_CONNECTED: event_name = "device_connected"; break;
    case USBH_EVENT_DEVICE_DISCONNECTED: event_name = "device_disconnected"; break;
    case USBH_EVENT_DEVICE_CONFIGURED: event_name = "device_configured"; break;
    case USBH_EVENT_DEVICE_WAKEUP: event_name = "device_wakeup"; break;
    case USBH_EVENT_DEVICE_SUSPEND: event_name = "device_suspend"; break;
    case USBH_EVENT_DEVICE_RESUME: event_name = "device_resume"; break;
    case USBH_EVENT_INTERFACE_UNSUPPORTED: event_name = "interface_unsupported"; break;
    case USBH_EVENT_INTERFACE_START: event_name = "interface_start"; break;
    case USBH_EVENT_INTERFACE_STOP: event_name = "interface_stop"; break;
    case USBH_EVENT_INIT: event_name = "init"; break;
    case USBH_EVENT_DEINIT: event_name = "deinit"; break;
    default: event_name = "unknown"; break;
    }

    BELLATRIX_USB_LOGF("[USB] event bus=%u hub=%u port=%u intf=%u event=%u (%s)\n",
                       (unsigned)busid,
                       (unsigned)hub_index,
                       (unsigned)hub_port,
                       (unsigned)intf,
                       (unsigned)event,
                       event_name);
#else
    (void)busid;
    (void)hub_index;
    (void)hub_port;
    (void)intf;
    (void)event;
#endif
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
        bellatrix_usb_pump_events(4u);
        BELLATRIX_USB_LOGF("[USB] CherryUSB DWC2 host initialized at %p\n",
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
        bellatrix_usb_pump_events(2u);
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
        bellatrix_usb_pump_events(1u);
        usbh_deinitialize(BELLATRIX_USB_BUS_ID);
        bellatrix_usb_pump_events(1u);
    }

    BELLATRIX_USB_LOGF("[USB] shutdown\n");
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
