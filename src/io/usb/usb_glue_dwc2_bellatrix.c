#include "usbh_core.h"
#include "usb_dwc2_reg.h"
#include "usb_dwc2_param.h"

#include <string.h>

#include "support.h"

/*
 * BCM2837 / Raspberry Pi 3B
 *
 * The on-board USB host controller is the Broadcom DWC2 instance mapped in the
 * peripheral window at 0x3f980000. Bellatrix uses Emu68's high-half alias for
 * peripheral MMIO, so the controller is visible at 0xf2980000.
 *
 * Bellatrix runs against two materially different DWC2 implementations:
 *
 * - the BCM2837 hardware on Raspberry Pi 3B, which advertises an HS/UTMI path
 * - the simpler QEMU model, which exposes only the FS configuration
 *
 * Pick the PHY mode at runtime from GHWCFG2 so both environments keep working.
 * Keep VBUS sensing bypassed so the root hub can still drive power and report
 * connection status during bring-up.
 */

#define BELLATRIX_USB_DWC2_TOTAL_FIFO_SIZE_WORDS    512u
#define BELLATRIX_USB_DWC2_HOST_RX_FIFO_SIZE_WORDS  256u
#define BELLATRIX_USB_DWC2_HOST_NP_FIFO_SIZE_WORDS  128u
#define BELLATRIX_USB_DWC2_HOST_PE_FIFO_SIZE_WORDS  128u

static const struct dwc2_user_params bellatrix_dwc2_params = {
    .phy_type = DWC2_PHY_TYPE_PARAM_FS,
    .phy_utmi_width = 8,
    .device_dma_enable = false,
    .device_dma_desc_enable = false,
    .device_rx_fifo_size = 0,
    .device_tx_fifo_size = { 0 },
    .host_dma_desc_enable = false,
    .host_rx_fifo_size = BELLATRIX_USB_DWC2_HOST_RX_FIFO_SIZE_WORDS,
    .host_nperio_tx_fifo_size = BELLATRIX_USB_DWC2_HOST_NP_FIFO_SIZE_WORDS,
    .host_perio_tx_fifo_size = BELLATRIX_USB_DWC2_HOST_PE_FIFO_SIZE_WORDS,
    .device_gccfg = 0,
    .host_gccfg = (USB_OTG_GCCFG_PWRDWN | USB_OTG_GCCFG_NOVBUSSENS),
    .b_session_valid_override = false,
    .total_fifo_size = BELLATRIX_USB_DWC2_TOTAL_FIFO_SIZE_WORDS,
};

void dwc2_get_user_params(uint32_t reg_base, struct dwc2_user_params *params)
{
    uint32_t ghwcfg2;
    uint32_t hs_phy_type;

    (void)reg_base;
    if (!params) {
        return;
    }

    memcpy(params, &bellatrix_dwc2_params, sizeof(*params));

    ghwcfg2 = dwc2_readl(reg_base + GHWCFG2_OFFSET);
    hs_phy_type = (ghwcfg2 & GHWCFG2_HS_PHY_TYPE_MASK) >> GHWCFG2_HS_PHY_TYPE_SHIFT;

    if (hs_phy_type == GHWCFG2_HS_PHY_TYPE_UTMI ||
        hs_phy_type == GHWCFG2_HS_PHY_TYPE_UTMI_ULPI) {
        params->phy_type = DWC2_PHY_TYPE_PARAM_UTMI;
    }
}

void usb_hc_low_level_init(struct usbh_bus *bus)
{
    if (!bus) {
        return;
    }

    kprintf("[USB] DWC2 low-level init: bus=%u reg_base=%p\n",
            (unsigned)bus->hcd.hcd_id,
            (void *)bus->hcd.reg_base);
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
    if (!bus) {
        return;
    }

    kprintf("[USB] DWC2 low-level deinit: bus=%u\n",
            (unsigned)bus->hcd.hcd_id);
}

void usb_dcache_clean(uintptr_t addr, size_t size)
{
    arm_flush_cache(addr, (uint32_t)size);
}

void usb_dcache_invalidate(uintptr_t addr, size_t size)
{
    arm_dcache_invalidate(addr, (uint32_t)size);
}

void usb_dcache_flush(uintptr_t addr, size_t size)
{
    arm_flush_cache(addr, (uint32_t)size);
}
