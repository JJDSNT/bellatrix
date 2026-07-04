#include "usbh_core.h"
#include "usb_dwc2_reg.h"
#include "usb_dwc2_param.h"

#include <string.h>

#include "support.h"
#include "host/raspi3/console_log.h"
#include "mmu.h"

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

/* VideoCore mailbox property interface — same pattern as bt_host.c */
#define USB_GLUE_ARM_PERI_VIRT  0xF2000000u
#define USB_GLUE_MBOX_READ      (USB_GLUE_ARM_PERI_VIRT + 0xB880u)
#define USB_GLUE_MBOX_STATUS    (USB_GLUE_ARM_PERI_VIRT + 0xB898u)
#define USB_GLUE_MBOX_WRITE     (USB_GLUE_ARM_PERI_VIRT + 0xB8A0u)
#define USB_GLUE_MBOX_TX_FULL   0x80000000u
#define USB_GLUE_MBOX_RX_EMPTY  0x40000000u
#define USB_GLUE_MBOX_CHANMASK  0xFu
#define USB_GLUE_MBOX_CH_PROP   8u
#define USB_GLUE_VC_REQ         0u
#define USB_GLUE_VC_END         0u

/* SET_POWER_STATE: device_id=3 (USB), state=3 (on=1 | wait=1) */
#define VC_TAG_GET_POWER_STATE  0x00020001u
#define VC_TAG_SET_POWER_STATE  0x00028001u
#define VC_DEVICE_USB           3u
#define VC_POWER_ON_WAIT        3u

/* Emu68's mailbox property buffer — mapped uncached by the boot MMU setup.
 * Our own cached static buffer made responses unreliable (we read back our
 * request, e.g. the bogus "state=0x3 device-missing").  Reuse Emu68's. */
extern uint32_t *FBReq;

static void usb_glue_mbox_send(uint32_t data)
{
    uint32_t val = (data & ~USB_GLUE_MBOX_CHANMASK) | USB_GLUE_MBOX_CH_PROP;
    while (rd32le(USB_GLUE_MBOX_STATUS) & USB_GLUE_MBOX_TX_FULL) { dsb(); }
    dmb();
    wr32le(USB_GLUE_MBOX_WRITE, val);
}

static uint32_t usb_glue_mbox_recv(void)
{
    uint32_t resp;
    do {
        while (rd32le(USB_GLUE_MBOX_STATUS) & USB_GLUE_MBOX_RX_EMPTY) { dsb(); }
        dmb();
        resp = rd32le(USB_GLUE_MBOX_READ);
        dmb();
    } while ((resp & USB_GLUE_MBOX_CHANMASK) != USB_GLUE_MBOX_CH_PROP);
    return resp & ~USB_GLUE_MBOX_CHANMASK;
}

/* Every working BCM283x USB driver (CSUD, USPI, Circle, Linux) powers the
 * USB block on through the VideoCore before touching the DWC2.  Without it
 * the PHY sense circuits still detect a connected device, but the transmit
 * serialiser never drives the bus: DMA fetches the packet and the MAC hands
 * it to a dead PHY — no XFRC, no TXERR, HCINT stays 0. */
static uint32_t usb_glue_vc_power_call(uint32_t tag, uint32_t state, uint32_t *resp_code)
{
    FBReq[0] = LE32(8u * 4u);           /* total size */
    FBReq[1] = LE32(USB_GLUE_VC_REQ);   /* request */
    FBReq[2] = LE32(tag);
    FBReq[3] = LE32(8u);                /* value buf size */
    FBReq[4] = LE32(0u);                /* request indicator */
    FBReq[5] = LE32(VC_DEVICE_USB);
    FBReq[6] = LE32(state);
    FBReq[7] = LE32(USB_GLUE_VC_END);

    arm_flush_cache((uintptr_t)FBReq, 8u * 4u);
    usb_glue_mbox_send((uint32_t)mmu_virt2phys((uintptr_t)FBReq));
    usb_glue_mbox_recv();

    if (resp_code) {
        *resp_code = LE32(FBReq[1]);
    }
    return LE32(FBReq[6]);
}

/* GET_THROTTLED (0x30046): bit0=undervoltage now, bit1=arm freq capped,
 * bit2=currently throttled, bits16-18 = same conditions since last read
 * (reading clears the sticky bits in firmware). */
uint32_t usb_glue_vc_get_throttled(void)
{
    FBReq[0] = LE32(8u * 4u);
    FBReq[1] = LE32(USB_GLUE_VC_REQ);
    FBReq[2] = LE32(0x00030046u);
    FBReq[3] = LE32(8u);
    FBReq[4] = LE32(0u);
    FBReq[5] = LE32(0u);
    FBReq[6] = LE32(0u);
    FBReq[7] = LE32(USB_GLUE_VC_END);

    arm_flush_cache((uintptr_t)FBReq, 8u * 4u);
    usb_glue_mbox_send((uint32_t)mmu_virt2phys((uintptr_t)FBReq));
    usb_glue_mbox_recv();

    return LE32(FBReq[5]);
}

static void usb_glue_vc_power_on_usb(void)
{
    uint32_t code_get, code_set, code_chk;
    uint32_t before, set, after;

    before = usb_glue_vc_power_call(VC_TAG_GET_POWER_STATE, 0u, &code_get);
    set    = usb_glue_vc_power_call(VC_TAG_SET_POWER_STATE, VC_POWER_ON_WAIT, &code_set);
    after  = usb_glue_vc_power_call(VC_TAG_GET_POWER_STATE, 0u, &code_chk);

    kprintf("[USB] VC USB power: get=0x%08x(rc=0x%08x) set=0x%08x(rc=0x%08x) chk=0x%08x(rc=0x%08x)\n",
            (unsigned int)before, (unsigned int)code_get,
            (unsigned int)set,    (unsigned int)code_set,
            (unsigned int)after,  (unsigned int)code_chk);
}

void usb_hc_low_level_init(struct usbh_bus *bus)
{
    if (!bus) {
        return;
    }

    kprintf("[USB] DWC2 build: FSLSS=1 at host-init (latched before port enable)\n");
    usb_glue_vc_power_on_usb();
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
