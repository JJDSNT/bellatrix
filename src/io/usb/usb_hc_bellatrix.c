/*
 * Legacy stub kept only as a reference during the early CherryUSB bring-up.
 *
 * Bellatrix no longer builds this file for USB host mode. The active host
 * controller implementation comes from the original CherryUSB DWC2 backend:
 *
 *   external/cherryusb/port/dwc2/usb_hc_dwc2.c
 *
 * Bellatrix-specific integration for that backend lives in:
 *
 *   src/io/usb/usb_glue_dwc2_bellatrix.c
 *
 * Keep this file clearly inert so it does not get mistaken for the active
 * host-controller path when reading the tree.
 */

#include "usbh_core.h"
#include "usbh_hub.h"

int usb_hc_init(struct usbh_bus *bus)
{
    (void)bus;
    return 0;
}

int usb_hc_deinit(struct usbh_bus *bus)
{
    (void)bus;
    return 0;
}

uint16_t usbh_get_frame_number(struct usbh_bus *bus)
{
    (void)bus;
    return 0;
}

int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf)
{
    uint8_t nports;
    uint8_t port;
    uint32_t status = 0;

    (void)bus;

    nports = CONFIG_USBHOST_MAX_RHPORTS;
    port = setup->wIndex;
    if (setup->bmRequestType & USB_REQUEST_RECIPIENT_DEVICE) {
        switch (setup->bRequest) {
            case HUB_REQUEST_CLEAR_FEATURE:
            case HUB_REQUEST_SET_FEATURE:
                switch (setup->wValue) {
                    case HUB_FEATURE_HUB_C_LOCALPOWER:
                    case HUB_FEATURE_HUB_C_OVERCURRENT:
                        break;
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_GET_STATUS:
                memset(buf, 0, 4);
                break;
            default:
                break;
        }
    } else if (setup->bmRequestType & USB_REQUEST_RECIPIENT_OTHER) {
        switch (setup->bRequest) {
            case HUB_REQUEST_CLEAR_FEATURE:
                if (!port || port > nports) {
                    return -USB_ERR_INVAL;
                }
                switch (setup->wValue) {
                    case HUB_PORT_FEATURE_ENABLE:
                    case HUB_PORT_FEATURE_SUSPEND:
                    case HUB_PORT_FEATURE_C_SUSPEND:
                    case HUB_PORT_FEATURE_POWER:
                    case HUB_PORT_FEATURE_C_CONNECTION:
                    case HUB_PORT_FEATURE_C_ENABLE:
                    case HUB_PORT_FEATURE_C_OVER_CURREN:
                    case HUB_PORT_FEATURE_C_RESET:
                        break;
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_SET_FEATURE:
                if (!port || port > nports) {
                    return -USB_ERR_INVAL;
                }
                switch (setup->wValue) {
                    case HUB_PORT_FEATURE_SUSPEND:
                    case HUB_PORT_FEATURE_POWER:
                    case HUB_PORT_FEATURE_RESET:
                        break;
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_GET_STATUS:
                if (!port || port > nports) {
                    return -USB_ERR_INVAL;
                }
                memcpy(buf, &status, 4);
                break;
            default:
                break;
        }
    }

    return 0;
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    (void)urb;
    return -USB_ERR_NOTSUPP;
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    (void)urb;
    return -USB_ERR_NOTSUPP;
}

void USBH_IRQHandler(uint8_t busid)
{
    (void)busid;
}
