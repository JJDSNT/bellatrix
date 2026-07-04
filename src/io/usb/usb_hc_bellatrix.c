/*
 * Copyright (c) 2022, sakumisu
 * Bellatrix adaptations: AArch64 BE register access, setup-packet LE bounce
 * buffer, BCM2837 DMA bus alias, SOF-driven channel kick.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * This is the active DWC2 host controller backend for Bellatrix on BCM2837.
 * It replaces CherryUSB's generic usb_hc_dwc2.c in the build so that all
 * BCM2837-specific adaptations live in Bellatrix-owned source files.
 *
 * Key adaptations:
 *
 *  1. rd32le/wr32le — BCM2837 runs AArch64 BE, so every 32-bit MMIO access
 *     needs a byte-swap.  dwc2_rd32/dwc2_wr32 wrap rd32le/wr32le.
 *
 *  2. Setup-packet LE bounce buffer — usb_setup_packet stores uint16_t fields
 *     in BE order on a BE host.  DWC2 DMA sends raw bytes verbatim, which
 *     QEMU/the device receives as LE and misinterprets.  setup_bounce_bufs[][]
 *     holds an 8-byte LE serialisation that actually reaches HCDMA.
 *
 *  3. BCM2837 DMA bus alias — on the UTMI path (real Pi 3B) HCDMA addresses
 *     need bit 30 set (0xC0000000 | phys). QEMU FS path does not need this.
 *
 *  4. SOF-driven channel kick — BCM2837 hardware can silently stall an armed
 *     control channel. On SOF, still-enabled channels with no HCINT and few
 *     prior kicks are re-enabled to give them another chance.
 */

#include "usbh_core.h"
#include "usbh_hub.h"
#include "usb_dwc2_reg.h"
#include "usb_dwc2_param.h"
#include "host/raspi3/console_log.h"
#include "mmu.h"
#include <stddef.h>

/* Implemented in usb_glue_dwc2_bellatrix.c */
void usb_hc_low_level_init(struct usbh_bus *bus);
void usb_hc_low_level_deinit(struct usbh_bus *bus);

#define DWC2_GLB_OFF(field)   ((uint32_t)offsetof(DWC2_GlobalTypeDef, field))
#define DWC2_HOST_OFF(field)  ((uint32_t)(USB_OTG_HOST_BASE + offsetof(DWC2_HostTypeDef, field)))
#define DWC2_HC_OFF(i, field) ((uint32_t)(USB_OTG_HOST_CHANNEL_BASE + ((i) * USB_OTG_HOST_CHANNEL_SIZE) + offsetof(DWC2_HostChannelTypeDef, field)))

static inline uint32_t dwc2_rd32(struct usbh_bus *bus, uint32_t off)
{
    return rd32le((uintptr_t)(bus->hcd.reg_base + off));
}

static inline void dwc2_wr32(struct usbh_bus *bus, uint32_t off, uint32_t val)
{
    wr32le((uintptr_t)(bus->hcd.reg_base + off), val);
}

static inline uint32_t dwc2_hc_rd32(struct usbh_bus *bus, uint8_t ch_num, uint32_t field_off)
{
    return dwc2_rd32(bus, DWC2_HC_OFF(ch_num, HCCHAR) - offsetof(DWC2_HostChannelTypeDef, HCCHAR) + field_off);
}

static inline void dwc2_hc_wr32(struct usbh_bus *bus, uint8_t ch_num, uint32_t field_off, uint32_t val)
{
    dwc2_wr32(bus, DWC2_HC_OFF(ch_num, HCCHAR) - offsetof(DWC2_HostChannelTypeDef, HCCHAR) + field_off, val);
}

struct dwc2_chan {
    uint8_t ep0_state;
    uint16_t num_packets;
    uint32_t xferlen;
    uint8_t chidx;
    bool inuse;
    bool do_ssplit;
    bool do_csplit;
    uint8_t hub_addr;
    uint8_t hub_port;
    uint16_t ssplit_frame;
    uint16_t last_start_frame;
    uint8_t sof_kicks;
    uint32_t saved_hcdma;
    uint32_t saved_hctsiz;
    usb_osal_sem_t waitsem;
    struct usbh_urb *urb;
    uint32_t iso_frame_idx;
};

struct dwc2_hcd {
    volatile bool port_csc;
    volatile bool port_pec;
    volatile bool port_occ;
    struct dwc2_hw_params hw_params;
    struct dwc2_user_params user_params;
    struct dwc2_chan chan_pool[16];
} g_dwc2_hcd[CONFIG_USBHOST_MAX_BUS];

/* Per-channel 32-byte aligned LE bounce buffers for setup packets.
 * On AArch64 BE, usb_setup_packet uint16_t fields are stored BE in memory.
 * DWC2 DMA sends raw bytes, which the device reads as LE. We serialise the
 * 8-byte setup packet field-by-field in LE order into this buffer before
 * handing it to HCDMA. */
static uint8_t setup_bounce_bufs[CONFIG_USBHOST_MAX_BUS][16][32]
    __attribute__((aligned(32)));

#define DWC2_EP0_STATE_SETUP     0
#define DWC2_EP0_STATE_INDATA    1
#define DWC2_EP0_STATE_OUTDATA   2
#define DWC2_EP0_STATE_INSTATUS  3
#define DWC2_EP0_STATE_OUTSTATUS 4

static inline uint16_t dwc2_get_full_frame_num(struct usbh_bus *bus);
static void dwc2_apply_port_speed_config(struct usbh_bus *bus, uint32_t hprt0);
static void dwc2_inchan_irq_handler(struct usbh_bus *bus, uint8_t ch_num);
static void dwc2_outchan_irq_handler(struct usbh_bus *bus, uint8_t ch_num);

static bool dwc2_service_pending_channels(struct usbh_bus *bus, bool log_fallback)
{
    uint32_t chan_int;
    bool handled = false;

    chan_int = (dwc2_rd32(bus, DWC2_HOST_OFF(HAINT)) &
                dwc2_rd32(bus, DWC2_HOST_OFF(HAINTMSK))) & 0xFFFFU;

    for (uint8_t i = 0U; i < g_dwc2_hcd[bus->hcd.hcd_id].hw_params.host_channels; i++) {
        if ((chan_int & (1UL << (i & 0xFU))) == 0U) {
            continue;
        }

        handled = true;
        if (log_fallback) {
            USB_LOG_INFO("Bellatrix DWC2: fallback ch=%u hcint=0x%08x hcchar=0x%08x haint=0x%08x\r\n",
                         (unsigned int)i,
                         (unsigned int)dwc2_hc_rd32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCINT)),
                         (unsigned int)dwc2_hc_rd32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCCHAR)),
                         (unsigned int)dwc2_rd32(bus, DWC2_HOST_OFF(HAINT)));
        }

        if ((dwc2_hc_rd32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCCHAR)) & USB_OTG_HCCHAR_EPDIR) == USB_OTG_HCCHAR_EPDIR) {
            dwc2_inchan_irq_handler(bus, i);
        } else {
            dwc2_outchan_irq_handler(bus, i);
        }
    }

    return handled;
}

static inline int dwc2_reset(struct usbh_bus *bus)
{
    volatile uint32_t count = 0U;

    do {
        if (++count > 200000U) {
            return -1;
        }
    } while ((dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) & USB_OTG_GRSTCTL_AHBIDL) == 0U);

    count = 0U;
    dwc2_wr32(bus, DWC2_GLB_OFF(GRSTCTL),
              dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) | USB_OTG_GRSTCTL_CSRST);

    if (g_dwc2_hcd[bus->hcd.hcd_id].hw_params.snpsid < 0x4F54420AU) {
        do {
            if (++count > 200000U) {
                USB_LOG_ERR("DWC2 reset timeout\r\n");
                return -1;
            }
        } while ((dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) & USB_OTG_GRSTCTL_CSRST) == USB_OTG_GRSTCTL_CSRST);
    } else {
        do {
            if (++count > 200000U) {
                USB_LOG_ERR("DWC2 reset timeout\r\n");
                return -1;
            }
        } while ((dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) & USB_OTG_GRSTCTL_CSRSTDONE) != USB_OTG_GRSTCTL_CSRSTDONE);

        dwc2_wr32(bus, DWC2_GLB_OFF(GRSTCTL),
                  dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) & ~USB_OTG_GRSTCTL_CSRST);
        dwc2_wr32(bus, DWC2_GLB_OFF(GRSTCTL),
                  dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) | USB_OTG_GRSTCTL_CSRSTDONE);
    }

    return 0;
}

static inline int dwc2_core_init(struct usbh_bus *bus)
{
    int ret;
    uint32_t regval;

    if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type == DWC2_PHY_TYPE_PARAM_FS) {
        dwc2_wr32(bus, DWC2_GLB_OFF(GUSBCFG),
                  dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG)) | USB_OTG_GUSBCFG_PHYSEL);
    } else {
        regval = dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG));
        regval &= ~USB_OTG_GUSBCFG_PHYSEL;
        regval &= ~(USB_OTG_GUSBCFG_ULPIEVBUSD | USB_OTG_GUSBCFG_ULPIEVBUSI);
        regval &= ~(USB_OTG_GUSBCFG_ULPIFSLS | USB_OTG_GUSBCFG_ULPICSM);

        switch (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type) {
            case DWC2_PHY_TYPE_PARAM_ULPI:
                regval |= USB_OTG_GUSBCFG_ULPI_UTMI_SEL;
                regval &= ~USB_OTG_GUSBCFG_PHYIF16;
                regval &= ~USB_OTG_GUSBCFG_DDR_SEL;
                break;
            case DWC2_PHY_TYPE_PARAM_UTMI:
                regval &= ~USB_OTG_GUSBCFG_ULPI_UTMI_SEL;
                regval &= ~USB_OTG_GUSBCFG_PHYIF16;
                if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_utmi_width == 16) {
                    regval |= USB_OTG_GUSBCFG_PHYIF16;
                }
                break;
            default:
                break;
        }
        dwc2_wr32(bus, DWC2_GLB_OFF(GUSBCFG), regval);
    }

    /* Pure host: disable the OTG session protocols. The firmware leaves
     * SRPCAP/HNPCAP/TSDPS set on BCM2837; with them active, the first
     * low-speed transaction triggers the OTG session state machine
     * (GINTSTS.OTGINT) and the root port gets disabled mid-enumeration.
     * USPI/Circle clear these in init for the same reason. */
    regval  = dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG));
    regval &= ~(USB_OTG_GUSBCFG_SRPCAP | USB_OTG_GUSBCFG_HNPCAP | USB_OTG_GUSBCFG_TSDPS);
    dwc2_wr32(bus, DWC2_GLB_OFF(GUSBCFG), regval);

    /* BCM2837: do NOT soft-reset the core. The VideoCore firmware initialises
     * the USB PHY at boot and a core soft reset leaves the PHY transmitter
     * dead: line-state sensing still works (connect/reset/enable all succeed)
     * but no packet is ever driven onto the bus and the HS chirp never
     * happens (the on-board LAN9514 hub falls back to FS). The AROS usb2otg
     * driver — same author as Emu68, same SoC — has the core reset
     * deliberately compiled out for this reason. */
    if (g_dwc2_hcd[bus->hcd.hcd_id].hw_params.snpsid == 0x4f54280aU) {
        return 0;
    }

    ret = dwc2_reset(bus);
    return ret;
}

static inline void dwc2_set_mode(struct usbh_bus *bus, uint8_t mode)
{
    dwc2_wr32(bus, DWC2_GLB_OFF(GUSBCFG),
              dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG)) & ~(USB_OTG_GUSBCFG_FHMOD | USB_OTG_GUSBCFG_FDMOD));

    if (mode == USB_OTG_MODE_HOST) {
        dwc2_wr32(bus, DWC2_GLB_OFF(GUSBCFG),
                  dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG)) | USB_OTG_GUSBCFG_FHMOD);
    } else if (mode == USB_OTG_MODE_DEVICE) {
        dwc2_wr32(bus, DWC2_GLB_OFF(GUSBCFG),
                  dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG)) | USB_OTG_GUSBCFG_FDMOD);
    }

    while (1) {
        if ((dwc2_rd32(bus, DWC2_GLB_OFF(GINTSTS)) & 0x1U) == USB_OTG_MODE_HOST) {
            break;
        }
        usb_osal_msleep(10);
    }
}

static inline int dwc2_flush_rxfifo(struct usbh_bus *bus)
{
    volatile uint32_t count = 0U;

    do {
        if (++count > 200000U) {
            return -1;
        }
    } while ((dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) & USB_OTG_GRSTCTL_AHBIDL) == 0U);

    count = 0;
    dwc2_wr32(bus, DWC2_GLB_OFF(GRSTCTL), USB_OTG_GRSTCTL_RXFFLSH);

    do {
        if (++count > 200000U) {
            return -1;
        }
    } while ((dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) & USB_OTG_GRSTCTL_RXFFLSH) == USB_OTG_GRSTCTL_RXFFLSH);

    return 0;
}

static inline int dwc2_flush_txfifo(struct usbh_bus *bus, uint32_t num)
{
    volatile uint32_t count = 0U;

    do {
        if (++count > 200000U) {
            return -1;
        }
    } while ((dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) & USB_OTG_GRSTCTL_AHBIDL) == 0U);

    count = 0;
    dwc2_wr32(bus, DWC2_GLB_OFF(GRSTCTL), (USB_OTG_GRSTCTL_TXFFLSH | (num << 6)));

    do {
        if (++count > 200000U) {
            return -1;
        }
    } while ((dwc2_rd32(bus, DWC2_GLB_OFF(GRSTCTL)) & USB_OTG_GRSTCTL_TXFFLSH) == USB_OTG_GRSTCTL_TXFFLSH);

    return 0;
}

static inline void dwc2_drivebus(struct usbh_bus *bus, uint8_t state)
{
    __IO uint32_t hprt0 = 0U;

    hprt0 = dwc2_rd32(bus, DWC2_HOST_OFF(HPRT));
    hprt0 &= ~(USB_OTG_HPRT_PENA | USB_OTG_HPRT_PCDET |
               USB_OTG_HPRT_PENCHNG | USB_OTG_HPRT_POCCHNG);

    if (((hprt0 & USB_OTG_HPRT_PPWR) == 0U) && (state == 1U)) {
        dwc2_wr32(bus, DWC2_HOST_OFF(HPRT), (USB_OTG_HPRT_PPWR | hprt0));
    }
    if (((hprt0 & USB_OTG_HPRT_PPWR) == USB_OTG_HPRT_PPWR) && (state == 0U)) {
        dwc2_wr32(bus, DWC2_HOST_OFF(HPRT), ((~USB_OTG_HPRT_PPWR) & hprt0));
    }
}

static inline uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
    __IO uint32_t hprt0 = 0U;
    uint8_t speed;
    (void)port;

    hprt0 = dwc2_rd32(bus, DWC2_HOST_OFF(HPRT));
    speed = (hprt0 & USB_OTG_HPRT_PSPD) >> 17;

    if (speed == HPRT0_PRTSPD_HIGH_SPEED) {
        return USB_SPEED_HIGH;
    } else if (speed == HPRT0_PRTSPD_FULL_SPEED) {
        return USB_SPEED_FULL;
    } else if (speed == HPRT0_PRTSPD_LOW_SPEED) {
        return USB_SPEED_LOW;
    } else {
        return USB_SPEED_UNKNOWN;
    }
}

static inline void dwc2_chan_char_init(struct usbh_bus *bus,
                                       uint8_t ch_num,
                                       uint8_t dev_addr,
                                       uint8_t ep_addr,
                                       uint8_t ep_type,
                                       uint16_t ep_mps,
                                       uint8_t ep_mult,
                                       uint8_t speed)
{
    uint32_t regval;

    regval = (((uint32_t)ep_mps << USB_OTG_HCCHAR_MPSIZ_Pos) & USB_OTG_HCCHAR_MPSIZ) |
             ((((uint32_t)ep_addr & 0x7FU) << USB_OTG_HCCHAR_EPNUM_Pos) & USB_OTG_HCCHAR_EPNUM) |
             (((uint32_t)ep_type << USB_OTG_HCCHAR_EPTYP_Pos) & USB_OTG_HCCHAR_EPTYP) |
             (((uint32_t)ep_mult << USB_OTG_HCCHAR_MC_Pos) & USB_OTG_HCCHAR_MC) |
             (((uint32_t)dev_addr << USB_OTG_HCCHAR_DAD_Pos) & USB_OTG_HCCHAR_DAD);

    if ((ep_addr & 0x80U) == 0x80U) {
        regval |= USB_OTG_HCCHAR_EPDIR;
    }

    if ((speed == USB_SPEED_LOW) && (usbh_get_port_speed(bus, 0) != USB_SPEED_LOW)) {
        regval |= USB_OTG_HCCHAR_LSDEV;
    }

    if (ep_type == USB_ENDPOINT_TYPE_INTERRUPT) {
        regval |= USB_OTG_HCCHAR_ODDFRM;
    }

    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR), regval);
}

static inline void dwc2_chan_splt_init(struct usbh_bus *bus, uint8_t ch_num)
{
    struct dwc2_chan *chan;
    uint32_t hcsplt;

    chan = &g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[ch_num];

    if (chan->do_ssplit) {
        hcsplt = USB_OTG_HCSPLT_SPLITEN;
        hcsplt |= (chan->hub_addr << USB_OTG_HCSPLT_HUBADDR_Pos);
        hcsplt |= chan->hub_port;

        if (chan->do_csplit) {
            hcsplt |= USB_OTG_HCSPLT_COMPLSPLT;
        } else {
            hcsplt &= ~USB_OTG_HCSPLT_COMPLSPLT;
        }

        dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCSPLT), hcsplt);
    } else {
        dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCSPLT), 0U);
    }
}

static void dwc2_chan_init(struct usbh_bus *bus,
                           uint8_t ch_num,
                           uint8_t dev_addr,
                           uint8_t ep_addr,
                           uint8_t ep_type,
                           uint16_t ep_mps,
                           uint8_t ep_mult,
                           uint8_t speed)
{
    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINTMSK), 0U);
    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINT), 0xFFFFFFFFU);

    dwc2_wr32(bus, DWC2_HOST_OFF(HAINTMSK),
              dwc2_rd32(bus, DWC2_HOST_OFF(HAINTMSK)) | (1UL << (ch_num & 0xFU)));

    dwc2_chan_char_init(bus, ch_num, dev_addr, ep_addr, ep_type, ep_mps, ep_mult, speed);
    dwc2_chan_splt_init(bus, ch_num);
}

static inline void dwc2_chan_transfer(struct usbh_bus *bus, uint8_t ch_num, uint8_t ep_addr,
                                      uint8_t *buf, uint32_t size, uint16_t num_packets, uint8_t pid)
{
    __IO uint32_t tmpreg;
    uint32_t dma_phys;
    uint32_t dma_addr;
    uint32_t hcintmsk;
    uint8_t is_oddframe;
    size_t flags;
    struct dwc2_chan *chan;

    (void)ep_addr;

    chan = &g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[ch_num];

    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCTSIZ),
                 (size & USB_OTG_HCTSIZ_XFRSIZ) |
                 (((uint32_t)num_packets << 19) & USB_OTG_HCTSIZ_PKTCNT) |
                 (((uint32_t)pid << 29) & USB_OTG_HCTSIZ_DPID));

    dma_phys = buf ? (uint32_t)mmu_virt2phys((uintptr_t)buf) : 0U;
    dma_addr = dma_phys;
    /* BCM2837 DMA bus alias: UTMI path (real Pi 3B) requires bit 30 set */
    if (dma_addr != 0U &&
        g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type != DWC2_PHY_TYPE_PARAM_FS) {
        dma_addr |= 0xC0000000U;
    }
    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCDMA), dma_addr);

    flags = usb_osal_enter_critical_section();

    hcintmsk = USB_OTG_HCINTMSK_XFRCM | USB_OTG_HCINTMSK_CHHM | USB_OTG_HCINTMSK_STALLM |
               USB_OTG_HCINTMSK_NAKM | USB_OTG_HCINTMSK_ACKM | USB_OTG_HCINTMSK_NYET |
               USB_OTG_HCINTMSK_TXERRM | USB_OTG_HCINTMSK_BBERRM |
               USB_OTG_HCINTMSK_FRMORM | USB_OTG_HCINTMSK_DTERRM | USB_OTG_HCINTMSK_AHBERR;
    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINTMSK), hcintmsk);

    /* Periodic transfers execute in the *next* frame, so ODDFRM must be the
     * opposite of the current frame parity (ST HAL does the same). Getting
     * this wrong makes the core flag FRMOR on every interrupt IN transfer. */
    is_oddframe = (((uint32_t)dwc2_rd32(bus, DWC2_HOST_OFF(HFNUM)) & 0x01U) != 0U) ? 0U : 1U;
    tmpreg = dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR));
    tmpreg &= ~USB_OTG_HCCHAR_ODDFRM;
    tmpreg |= (uint32_t)is_oddframe << 29;
    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR), tmpreg);

    tmpreg = dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR));
    tmpreg &= ~USB_OTG_HCCHAR_CHDIS;
    tmpreg |= USB_OTG_HCCHAR_CHENA;
    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR), tmpreg);

    chan->saved_hcdma  = dma_addr;
    chan->saved_hctsiz = (size & USB_OTG_HCTSIZ_XFRSIZ) |
                         (((uint32_t)num_packets << 19) & USB_OTG_HCTSIZ_PKTCNT) |
                         (((uint32_t)pid << 29) & USB_OTG_HCTSIZ_DPID);

    if (chan->urb && USB_GET_ENDPOINT_TYPE(chan->urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_CONTROL) {
        USB_LOG_INFO("Bellatrix DWC2: arm ch=%u ep=0x%02x state=%u hcintmsk=0x%08x hctsiz=0x%08x hcchar=0x%08x hcdma=0x%08x pid=%u size=%u pkts=%u\r\n",
                     (unsigned int)ch_num, (unsigned int)ep_addr,
                     (unsigned int)chan->ep0_state,
                     (unsigned int)hcintmsk,
                     (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCTSIZ)),
                     (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR)),
                     (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCDMA)),
                     (unsigned int)pid, (unsigned int)size, (unsigned int)num_packets);
        USB_LOG_INFO("Bellatrix DWC2: dma buf=%p phys=0x%08x alias=0x%08x\r\n",
                     buf, (unsigned int)dma_phys, (unsigned int)dma_addr);
    }

    chan->last_start_frame = dwc2_get_full_frame_num(bus);
    chan->sof_kicks = 0;

    usb_osal_leave_critical_section(flags);
}

static void dwc2_sof_kick_pending_channels(struct usbh_bus *bus)
{
    uint16_t frame;

    if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type == DWC2_PHY_TYPE_PARAM_FS) {
        return;
    }

    frame = dwc2_get_full_frame_num(bus);

    for (uint8_t i = 0U; i < g_dwc2_hcd[bus->hcd.hcd_id].hw_params.host_channels; i++) {
        struct dwc2_chan *chan;
        struct usbh_urb *urb;
        uint32_t hcchar;
        uint32_t hcint;

        chan = &g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[i];
        urb = chan->urb;
        if (!urb || USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) != USB_ENDPOINT_TYPE_CONTROL) {
            continue;
        }

        hcchar = dwc2_hc_rd32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCCHAR));
        hcint  = dwc2_hc_rd32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCINT));

        if ((hcchar & USB_OTG_HCCHAR_CHENA) == 0U) {
            continue;
        }
        if (hcint != 0U) {
            continue;
        }
        if (chan->sof_kicks >= 4U) {
            continue;
        }
        if (chan->last_start_frame == frame) {
            continue;
        }

        /* Restore HCTSIZ and HCDMA to the original arm values so that DMA
         * re-reads from the start of the bounce buffer rather than from
         * wherever it stopped after the first fetch. */
        dwc2_hc_wr32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCTSIZ), chan->saved_hctsiz);
        dwc2_hc_wr32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCDMA),  chan->saved_hcdma);

        hcchar &= ~(USB_OTG_HCCHAR_CHDIS | USB_OTG_HCCHAR_ODDFRM);
        hcchar |= (((uint32_t)dwc2_rd32(bus, DWC2_HOST_OFF(HFNUM)) & 0x01U) != 0U) ? 0U : USB_OTG_HCCHAR_ODDFRM;
        hcchar |= USB_OTG_HCCHAR_CHENA;
        dwc2_hc_wr32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCCHAR), hcchar);

        chan->last_start_frame = frame;
        chan->sof_kicks++;

        USB_LOG_INFO("Bellatrix DWC2: sof kick ch=%u frame=%u kicks=%u hcchar=0x%08x hctsiz=0x%08x hcdma=0x%08x\r\n",
                     (unsigned int)i, (unsigned int)frame, (unsigned int)chan->sof_kicks,
                     (unsigned int)dwc2_hc_rd32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCCHAR)),
                     (unsigned int)dwc2_hc_rd32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCTSIZ)),
                     (unsigned int)dwc2_hc_rd32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCDMA)));
    }
}

static inline void dwc2_chan_enable_csplit(struct usbh_bus *bus, uint8_t ch_num, bool enable)
{
    if (enable) {
        dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCSPLT),
                     dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCSPLT)) | USB_OTG_HCSPLT_COMPLSPLT);
    } else {
        dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCSPLT),
                     dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCSPLT)) & ~USB_OTG_HCSPLT_COMPLSPLT);
    }
}

static void dwc2_halt(struct usbh_bus *bus, uint8_t ch_num)
{
    volatile uint32_t ChannelEna = (dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR)) & USB_OTG_HCCHAR_CHENA) >> 31;
    volatile uint32_t count = 0U;
    volatile uint32_t value;

    if (((dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)) & USB_OTG_GAHBCFG_DMAEN) == USB_OTG_GAHBCFG_DMAEN) &&
        (ChannelEna == 0U)) {
        return;
    }

    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINTMSK), 0U);
    /* BCM2837: halt stalls when TX FIFO holds unacknowledged data.
     * Flush all TX FIFOs first so CHENA clears within the timeout. */
    dwc2_flush_txfifo(bus, 0x10U);

    value = dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR));
    value |= USB_OTG_HCCHAR_CHDIS;
    value |= USB_OTG_HCCHAR_CHENA;
    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR), value);

    do {
        if (++count > 200000U) {
            break;
        }
    } while (dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR)) & USB_OTG_HCCHAR_CHENA);

    dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINT),
                 dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINT)));
}

static int usbh_reset_port(struct usbh_bus *bus, const uint8_t port)
{
    __IO uint32_t hprt0 = 0U;
    volatile uint32_t timeout = 0;
    (void)port;

    hprt0 = dwc2_rd32(bus, DWC2_HOST_OFF(HPRT));
    hprt0 &= ~(USB_OTG_HPRT_PENA | USB_OTG_HPRT_PCDET |
               USB_OTG_HPRT_PENCHNG | USB_OTG_HPRT_POCCHNG);

    dwc2_wr32(bus, DWC2_HOST_OFF(HPRT), (USB_OTG_HPRT_PRST | hprt0));
    usb_osal_msleep(100U);
    dwc2_wr32(bus, DWC2_HOST_OFF(HPRT), ((~USB_OTG_HPRT_PRST) & hprt0));
    usb_osal_msleep(10U);

    while (!(dwc2_rd32(bus, DWC2_HOST_OFF(HPRT)) & USB_OTG_HPRT_PENA)) {
        if (!(dwc2_rd32(bus, DWC2_HOST_OFF(HPRT)) & USB_OTG_HPRT_PCSTS)) {
            return -USB_ERR_NOTCONN;
        }
        timeout++;
        if (timeout > 10) {
            USB_LOG_ERR("Reset port timeout\r\n");
            return -USB_ERR_TIMEOUT;
        }
        usb_osal_msleep(10U);
    }

    /* On BCM2837 the hub thread can observe PENA and start EP0 enumeration
     * before the pending PENCHNG interrupt is serviced. Program the FS/LS
     * timing immediately so the first SETUP does not use the stale HS/UTMI
     * host template. */
    hprt0 = dwc2_rd32(bus, DWC2_HOST_OFF(HPRT));
    dwc2_apply_port_speed_config(bus, hprt0);

    /* The reset itself raises PCDET/PENCHNG: the HPRT irq handler (pumped
     * from sem_take during the waits above) records them as port_csc/port_pec
     * and queues a root-hub wakeup. The hub thread is the caller of this
     * reset — these self-inflicted change events must not surface as a new
     * C_CONNECTION after enumeration, or the stack tears down the device it
     * just enumerated (observed: LAN9514 hub unregistered right after
     * interface_start). Clear them here; a genuine unplug raises CSC again
     * outside any reset window. */
    g_dwc2_hcd[bus->hcd.hcd_id].port_csc = 0;
    g_dwc2_hcd[bus->hcd.hcd_id].port_pec = 0;
    bus->hcd.roothub.int_buffer[0] = 0;
    return 0;
}

static inline uint32_t dwc2_get_glb_intstatus(struct usbh_bus *bus)
{
    uint32_t tmpreg;

    tmpreg = dwc2_rd32(bus, DWC2_GLB_OFF(GINTSTS));
    tmpreg &= dwc2_rd32(bus, DWC2_GLB_OFF(GINTMSK));

    return tmpreg;
}

static inline uint16_t dwc2_get_full_frame_num(struct usbh_bus *bus)
{
    uint16_t frame = usbh_get_frame_number(bus);

    return ((frame & 0x3FFF) >> 3);
}

uint32_t dwc2_calc_frame_interval(struct usbh_bus *bus)
{
    uint32_t usbcfg;
    uint32_t hprt0;
    int clock = 60;

    usbcfg = dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG));
    hprt0  = dwc2_rd32(bus, DWC2_HOST_OFF(HPRT));

    if (!(usbcfg & USB_OTG_GUSBCFG_PHYSEL) && (usbcfg & USB_OTG_GUSBCFG_ULPI_UTMI_SEL) &&
        !(usbcfg & USB_OTG_GUSBCFG_PHYIF16))
        clock = 60;
    if ((usbcfg & USB_OTG_GUSBCFG_PHYSEL) && g_dwc2_hcd[bus->hcd.hcd_id].hw_params.fs_phy_type ==
                                                 GHWCFG2_FS_PHY_TYPE_SHARED_ULPI)
        clock = 48;
    if (!(usbcfg & USB_OTG_GUSBCFG_PHYLPCS) && !(usbcfg & USB_OTG_GUSBCFG_PHYSEL) &&
        !(usbcfg & USB_OTG_GUSBCFG_ULPI_UTMI_SEL) && (usbcfg & USB_OTG_GUSBCFG_PHYIF16))
        clock = 30;
    if (!(usbcfg & USB_OTG_GUSBCFG_PHYLPCS) && !(usbcfg & USB_OTG_GUSBCFG_PHYSEL) &&
        !(usbcfg & USB_OTG_GUSBCFG_ULPI_UTMI_SEL) && !(usbcfg & USB_OTG_GUSBCFG_PHYIF16))
        clock = 60;
    if ((usbcfg & USB_OTG_GUSBCFG_PHYLPCS) && !(usbcfg & USB_OTG_GUSBCFG_PHYSEL) &&
        !(usbcfg & USB_OTG_GUSBCFG_ULPI_UTMI_SEL) && (usbcfg & USB_OTG_GUSBCFG_PHYIF16))
        clock = 48;
    if ((usbcfg & USB_OTG_GUSBCFG_PHYSEL) && !(usbcfg & USB_OTG_GUSBCFG_PHYIF16) &&
        g_dwc2_hcd[bus->hcd.hcd_id].hw_params.fs_phy_type == GHWCFG2_FS_PHY_TYPE_SHARED_UTMI)
        clock = 48;
    if ((usbcfg & USB_OTG_GUSBCFG_PHYSEL) &&
        g_dwc2_hcd[bus->hcd.hcd_id].hw_params.fs_phy_type == GHWCFG2_FS_PHY_TYPE_DEDICATED)
        clock = 48;

    if ((hprt0 & USB_OTG_HPRT_PSPD) >> USB_OTG_HPRT_PSPD_Pos == HPRT0_PRTSPD_HIGH_SPEED)
        return 125 * clock - 1;

    return 1000 * clock - 1;
}

static void dwc2_apply_port_speed_config(struct usbh_bus *bus, uint32_t hprt0)
{
    uint32_t regval;

    if ((hprt0 & USB_OTG_HPRT_PENA) != USB_OTG_HPRT_PENA) {
        return;
    }

    regval  = dwc2_rd32(bus, DWC2_HOST_OFF(HFIR));
    regval &= ~USB_OTG_HFIR_FRIVL;
    regval |= dwc2_calc_frame_interval(bus) & USB_OTG_HFIR_FRIVL;
    dwc2_wr32(bus, DWC2_HOST_OFF(HFIR), regval);

    regval  = dwc2_rd32(bus, DWC2_HOST_OFF(HCFG));
    regval &= ~(USB_OTG_HCFG_FSLSPCS | USB_OTG_HCFG_FSLSS);
    if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type == DWC2_PHY_TYPE_PARAM_FS) {
        /* PHYSEL=1: dedicated FS serial transceiver — PHY clock follows the
         * attached device speed and the core runs in FS/LS-only mode. */
        if ((hprt0 & USB_OTG_HPRT_PSPD) == (HPRT0_PRTSPD_LOW_SPEED << 17)) {
            regval |= USB_OTG_HCFG_FSLSPCLKSEL_6_MHZ | USB_OTG_HCFG_FSLSS;
        } else {
            regval |= USB_OTG_HCFG_FSLSPCLKSEL_48_MHZ | USB_OTG_HCFG_FSLSS;
        }
    } else {
        /* UTMI/ULPI PHY (BCM2837): the PHY clock is always 30/60 MHz, even
         * for FS/LS devices on the root port. Selecting 48 MHz here (the
         * STM32 FS-serial template) desyncs the host frame scheduler from
         * HFIR (computed for a 60 MHz clock) and the core never starts any
         * transaction — channel stays armed, HCINT=0 forever. */
        regval |= USB_OTG_HCFG_FSLSPCLKSEL_30_60_MHZ;
        if ((hprt0 & USB_OTG_HPRT_PSPD) != (HPRT0_PRTSPD_HIGH_SPEED << 17)) {
            /* FS/LS root port: FSLSSupp enables PRE-prefixed low-speed
             * packets for LS devices behind a FS hub. Without it, the first
             * LS transaction (LSDEV=1, no split) puts invalid signalling on
             * the bus and the core disables the root port (PENCHNG, ena=0,
             * XACTERR — observed with the keyboard on the LAN9514 port 3).
             * Keep the 30/60 MHz clock select — only the 48 MHz pairing was
             * wrong for UTMI. */
            regval |= USB_OTG_HCFG_FSLSS;
        }
    }
    dwc2_wr32(bus, DWC2_HOST_OFF(HCFG), regval);

    USB_LOG_INFO("Bellatrix DWC2: port speed config speed=%u hcfg=0x%08x hfir=0x%08x\r\n",
                 (unsigned int)((hprt0 & USB_OTG_HPRT_PSPD) >> USB_OTG_HPRT_PSPD_Pos),
                 (unsigned int)dwc2_rd32(bus, DWC2_HOST_OFF(HCFG)),
                 (unsigned int)dwc2_rd32(bus, DWC2_HOST_OFF(HFIR)));

    /* BCM2837: if EP0 was armed before the host-side speed template was
     * updated, the TX FIFO may still hold data prepared under the previous
     * HCFG. Flush it so the next arm uses the correct speed. */
    dwc2_flush_txfifo(bus, 0x10U);
}

static int dwc2_chan_alloc(struct usbh_bus *bus)
{
    size_t flags;
    int chidx;

    flags = usb_osal_enter_critical_section();
    for (chidx = 0; chidx < g_dwc2_hcd[bus->hcd.hcd_id].hw_params.host_channels; chidx++) {
        if (!g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx].inuse) {
            g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx].inuse = true;
            usb_osal_leave_critical_section(flags);
            g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx].do_ssplit = 0;
            g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx].do_csplit = 0;
            return chidx;
        }
    }
    usb_osal_leave_critical_section(flags);
    return -1;
}

static void dwc2_chan_free(struct dwc2_chan *chan)
{
    size_t flags;

    flags = usb_osal_enter_critical_section();
    if (chan->urb) {
        chan->urb->hcpriv = NULL;
        chan->urb = NULL;
    }
    chan->inuse = false;
    usb_osal_leave_critical_section(flags);
}

static uint16_t dwc2_calculate_packet_num(uint32_t input_size, uint8_t ep_addr, uint16_t ep_mps, uint32_t *output_size)
{
    uint16_t num_packets;

    num_packets = (uint16_t)((input_size + ep_mps - 1U) / ep_mps);

    if (num_packets > 0x3FF) {
        num_packets = 0x3FF;
    }

    if (input_size == 0) {
        num_packets = 1;
    }

    if (ep_addr & 0x80) {
        input_size = num_packets * ep_mps;
    }

    *output_size = input_size;
    return num_packets;
}

static void dwc2_control_urb_init(struct usbh_bus *bus, uint8_t chidx, struct usbh_urb *urb,
                                   struct usb_setup_packet *setup, uint8_t *buffer, uint32_t buflen)
{
    struct dwc2_chan *chan;
    uint32_t datalen;
    uint8_t data_pid;

    chan = &g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx];

    if (chan->do_ssplit && (chan->ep0_state == DWC2_EP0_STATE_INDATA || chan->ep0_state == DWC2_EP0_STATE_OUTDATA)) {
        if (buflen > USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize)) {
            datalen = USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize);
        } else {
            datalen = buflen;
        }

        if (urb->data_toggle == 0) {
            data_pid = HC_PID_DATA0;
        } else {
            data_pid = HC_PID_DATA1;
        }
    } else {
        datalen = buflen;
        data_pid = HC_PID_DATA1;
    }

    if (chan->ep0_state == DWC2_EP0_STATE_SETUP) {
        uint8_t *bounce = setup_bounce_bufs[bus->hcd.hcd_id][chidx];

        /* Serialise the 8-byte setup packet in LE byte order.
         * The struct fields are BE in memory on AArch64 BE; DWC2 DMA sends
         * raw bytes verbatim and the device interprets them as LE wire format. */
        bounce[0] = setup->bmRequestType;
        bounce[1] = setup->bRequest;
        bounce[2] = (uint8_t)(setup->wValue);
        bounce[3] = (uint8_t)(setup->wValue >> 8);
        bounce[4] = (uint8_t)(setup->wIndex);
        bounce[5] = (uint8_t)(setup->wIndex >> 8);
        bounce[6] = (uint8_t)(setup->wLength);
        bounce[7] = (uint8_t)(setup->wLength >> 8);
        usb_dcache_clean((uintptr_t)bounce, 32);

        USB_LOG_INFO("Bellatrix DWC2: setup bytes %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
                     (unsigned int)bounce[0], (unsigned int)bounce[1],
                     (unsigned int)bounce[2], (unsigned int)bounce[3],
                     (unsigned int)bounce[4], (unsigned int)bounce[5],
                     (unsigned int)bounce[6], (unsigned int)bounce[7]);

        chan->num_packets = dwc2_calculate_packet_num(8, 0x00, USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize), &chan->xferlen);
        dwc2_chan_init(bus, chidx, urb->hport->dev_addr, 0x00,
                       USB_ENDPOINT_TYPE_CONTROL,
                       USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize),
                       1, urb->hport->speed);
        dwc2_chan_transfer(bus, chidx, 0x00, bounce, chan->xferlen, chan->num_packets, HC_PID_SETUP);

    } else if (chan->ep0_state == DWC2_EP0_STATE_INDATA) {
        chan->num_packets = dwc2_calculate_packet_num(datalen, 0x80, USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize), &chan->xferlen);
        dwc2_chan_init(bus, chidx, urb->hport->dev_addr, 0x80,
                       USB_ENDPOINT_TYPE_CONTROL,
                       USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize),
                       1, urb->hport->speed);
        dwc2_chan_transfer(bus, chidx, 0x80, buffer, chan->xferlen, chan->num_packets, data_pid);

    } else if (chan->ep0_state == DWC2_EP0_STATE_OUTDATA) {
        chan->num_packets = dwc2_calculate_packet_num(datalen, 0x00, USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize), &chan->xferlen);
        dwc2_chan_init(bus, chidx, urb->hport->dev_addr, 0x00,
                       USB_ENDPOINT_TYPE_CONTROL,
                       USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize),
                       1, urb->hport->speed);
        dwc2_chan_transfer(bus, chidx, 0x00, buffer, chan->xferlen, chan->num_packets, data_pid);

    } else if (chan->ep0_state == DWC2_EP0_STATE_INSTATUS) {
        chan->num_packets = dwc2_calculate_packet_num(0, 0x80, USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize), &chan->xferlen);
        dwc2_chan_init(bus, chidx, urb->hport->dev_addr, 0x80,
                       USB_ENDPOINT_TYPE_CONTROL,
                       USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize),
                       1, urb->hport->speed);
        dwc2_chan_transfer(bus, chidx, 0x80, NULL, chan->xferlen, chan->num_packets, HC_PID_DATA1);

    } else if (chan->ep0_state == DWC2_EP0_STATE_OUTSTATUS) {
        chan->num_packets = dwc2_calculate_packet_num(0, 0x00, USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize), &chan->xferlen);
        dwc2_chan_init(bus, chidx, urb->hport->dev_addr, 0x00,
                       USB_ENDPOINT_TYPE_CONTROL,
                       USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize),
                       1, urb->hport->speed);
        dwc2_chan_transfer(bus, chidx, 0x00, NULL, chan->xferlen, chan->num_packets, HC_PID_DATA1);
    }
}

static void dwc2_bulk_intr_urb_init(struct usbh_bus *bus, uint8_t chidx, struct usbh_urb *urb,
                                     uint8_t *buffer, uint32_t buflen)
{
    struct dwc2_chan *chan;
    uint32_t datalen;

    chan = &g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx];

    if (chan->do_ssplit) {
        if (buflen > USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize)) {
            datalen = USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize);
        } else {
            datalen = buflen;
        }
    } else {
        datalen = buflen;
    }

    chan->num_packets = dwc2_calculate_packet_num(datalen, urb->ep->bEndpointAddress,
                                                  USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize),
                                                  &chan->xferlen);
    dwc2_chan_init(bus, chidx, urb->hport->dev_addr, urb->ep->bEndpointAddress,
                   USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes),
                   USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize),
                   USB_GET_MULT(urb->ep->wMaxPacketSize) + 1,
                   urb->hport->speed);
    dwc2_chan_transfer(bus, chidx, urb->ep->bEndpointAddress, buffer, chan->xferlen, chan->num_packets,
                       urb->data_toggle == 0 ? HC_PID_DATA0 : HC_PID_DATA1);
}

int usb_hc_init(struct usbh_bus *bus)
{
    int ret;

    memset(&g_dwc2_hcd[bus->hcd.hcd_id], 0, sizeof(struct dwc2_hcd));

    usb_hc_low_level_init(bus);

    USB_LOG_INFO("========== dwc2 hcd params ==========\r\n");
    USB_LOG_INFO("CID:%08x\r\n",      (unsigned int)dwc2_rd32(bus, DWC2_GLB_OFF(CID)));
    USB_LOG_INFO("GSNPSID:%08x\r\n",  (unsigned int)dwc2_rd32(bus, DWC2_GLB_OFF(GSNPSID)));
    USB_LOG_INFO("GHWCFG1:%08x\r\n",  (unsigned int)dwc2_rd32(bus, DWC2_GLB_OFF(GHWCFG1)));
    USB_LOG_INFO("GHWCFG2:%08x\r\n",  (unsigned int)dwc2_rd32(bus, DWC2_GLB_OFF(GHWCFG2)));
    USB_LOG_INFO("GHWCFG3:%08x\r\n",  (unsigned int)dwc2_rd32(bus, DWC2_GLB_OFF(GHWCFG3)));
    USB_LOG_INFO("GHWCFG4:%08x\r\n",  (unsigned int)dwc2_rd32(bus, DWC2_GLB_OFF(GHWCFG4)));

    dwc2_get_hwparams(bus->hcd.reg_base, &g_dwc2_hcd[bus->hcd.hcd_id].hw_params);
    dwc2_get_user_params(bus->hcd.reg_base, &g_dwc2_hcd[bus->hcd.hcd_id].user_params);

    if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_utmi_width == 0) {
        g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_utmi_width = 8;
    }
    if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.total_fifo_size == 0) {
        g_dwc2_hcd[bus->hcd.hcd_id].user_params.total_fifo_size =
            g_dwc2_hcd[bus->hcd.hcd_id].hw_params.total_fifo_size;
    }

    for (uint8_t chidx = 0; chidx < g_dwc2_hcd[bus->hcd.hcd_id].hw_params.host_channels; chidx++) {
        g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx].waitsem = usb_osal_sem_create(0);
    }

    USB_LOG_INFO("dwc2 has %d channels and dfifo depth(32-bit words) is %d\r\n",
                 g_dwc2_hcd[bus->hcd.hcd_id].hw_params.host_channels,
                 g_dwc2_hcd[bus->hcd.hcd_id].user_params.total_fifo_size);
    USB_LOG_INFO("Bellatrix DWC2: user phy_type=%u utmi_width=%u gccfg=0x%08x fifo(rx=%u np=%u p=%u)\r\n",
                 (unsigned int)g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type,
                 (unsigned int)g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_utmi_width,
                 (unsigned int)g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_gccfg,
                 (unsigned int)g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_rx_fifo_size,
                 (unsigned int)g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_nperio_tx_fifo_size,
                 (unsigned int)g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_perio_tx_fifo_size);

    USB_ASSERT_MSG(g_dwc2_hcd[bus->hcd.hcd_id].hw_params.arch == GHWCFG2_INT_DMA_ARCH,
                   "This dwc2 version does not support dma mode, so stop working");
    USB_ASSERT_MSG(((uint32_t)g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_rx_fifo_size +
                    (uint32_t)g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_nperio_tx_fifo_size +
                    (uint32_t)g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_perio_tx_fifo_size) <=
                       g_dwc2_hcd[bus->hcd.hcd_id].user_params.total_fifo_size,
                   "Your fifo config is overflow, please check");

    dwc2_wr32(bus, DWC2_GLB_OFF(GAHBCFG),
              dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)) & ~USB_OTG_GAHBCFG_GINT);

    dwc2_wr32(bus, DWC2_GLB_OFF(GCCFG), g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_gccfg);

    if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type != DWC2_PHY_TYPE_PARAM_FS) {
        USB_ASSERT_MSG(g_dwc2_hcd[bus->hcd.hcd_id].hw_params.hs_phy_type != 0,
                       "This dwc2 version does not support hs, so stop working");
    }

    ret = dwc2_core_init(bus);

    dwc2_set_mode(bus, USB_OTG_MODE_HOST);

    USB_ASSERT_MSG((dwc2_rd32(bus, DWC2_GLB_OFF(GRXFSIZ)) & 0xffff) >=
                       g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_rx_fifo_size,
                   "host_rx_fifo_size cannot be larger than power_on_value %u",
                   (unsigned int)(dwc2_rd32(bus, DWC2_GLB_OFF(GRXFSIZ)) & 0xffff));
    USB_ASSERT_MSG(((dwc2_rd32(bus, DWC2_GLB_OFF(DIEPTXF0_HNPTXFSIZ)) >> 16) & 0xffff) >=
                       g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_nperio_tx_fifo_size,
                   "host_nperio_tx_fifo_size cannot be larger than power_on_value %u",
                   (unsigned int)((dwc2_rd32(bus, DWC2_GLB_OFF(DIEPTXF0_HNPTXFSIZ)) >> 16) & 0xffff));
    USB_ASSERT_MSG(((dwc2_rd32(bus, DWC2_GLB_OFF(HPTXFSIZ)) >> 16) & 0xffff) >=
                       g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_perio_tx_fifo_size,
                   "host_perio_tx_fifo_size cannot be larger than power_on_value %u",
                   (unsigned int)((dwc2_rd32(bus, DWC2_GLB_OFF(HPTXFSIZ)) >> 16) & 0xffff));

    dwc2_wr32(bus, DWC2_GLB_OFF(GOTGCTL),
              dwc2_rd32(bus, DWC2_GLB_OFF(GOTGCTL)) & ~USB_OTG_GOTGCTL_BVALOEN);
    dwc2_wr32(bus, DWC2_GLB_OFF(GOTGCTL),
              dwc2_rd32(bus, DWC2_GLB_OFF(GOTGCTL)) & ~USB_OTG_GOTGCTL_BVALOVAL);

    /* USB turnaround time: Linux dwc2_gusbcfg_init uses 9 for 8-bit UTMI,
     * 5 for 16-bit. The core reset default of 5 is too short for the
     * BCM2837 8-bit UTMI PHY. TOCAL stays at its reset default. */
    if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type == DWC2_PHY_TYPE_PARAM_UTMI &&
        g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_utmi_width == 8) {
        uint32_t gusbcfg = dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG));
        gusbcfg &= ~USB_OTG_GUSBCFG_TRDT;
        gusbcfg |= (9U << USB_OTG_GUSBCFG_TRDT_Pos) & USB_OTG_GUSBCFG_TRDT;
        dwc2_wr32(bus, DWC2_GLB_OFF(GUSBCFG), gusbcfg);
    }

    dwc2_wr32(bus, USB_OTG_PCGCCTL_BASE, 0U);

    dwc2_wr32(bus, DWC2_HOST_OFF(HCFG),
              dwc2_rd32(bus, DWC2_HOST_OFF(HCFG)) & ~USB_OTG_HCFG_FSLSS);
    dwc2_wr32(bus, DWC2_HOST_OFF(HCFG),
              dwc2_rd32(bus, DWC2_HOST_OFF(HCFG)) & ~USB_OTG_HCFG_FSLSPCS);

    if (g_dwc2_hcd[bus->hcd.hcd_id].user_params.phy_type == DWC2_PHY_TYPE_PARAM_FS) {
        bus->hcd.roothub.speed = USB_SPEED_FULL;
        dwc2_wr32(bus, DWC2_HOST_OFF(HCFG),
                  dwc2_rd32(bus, DWC2_HOST_OFF(HCFG)) | USB_OTG_HCFG_FSLSPCLKSEL_48_MHZ);
    } else {
        bus->hcd.roothub.speed = USB_SPEED_HIGH;
        dwc2_wr32(bus, DWC2_HOST_OFF(HCFG),
                  dwc2_rd32(bus, DWC2_HOST_OFF(HCFG)) | USB_OTG_HCFG_FSLSPCLKSEL_30_60_MHZ);
        if (g_dwc2_hcd[bus->hcd.hcd_id].hw_params.snpsid == 0x4f54280aU) {
            /* BCM2837: FSLSSupp is sampled when the port becomes enabled —
             * setting it from the PENCHNG handler is too late, the core has
             * already latched HS-host mode and its babble detector (FS
             * timing) kills the port on the first PRE+LS packet (8x slower).
             * Set it here, before port power, so the port enables directly
             * in FS/LS-only mode. The root port never achieved HS on this
             * setup anyway, so nothing is lost. */
            bus->hcd.roothub.speed = USB_SPEED_FULL;
            dwc2_wr32(bus, DWC2_HOST_OFF(HCFG),
                      dwc2_rd32(bus, DWC2_HOST_OFF(HCFG)) | USB_OTG_HCFG_FSLSS);
        }
    }

    if (g_dwc2_hcd[bus->hcd.hcd_id].hw_params.snpsid > 0x4F54292AU) {
        dwc2_wr32(bus, DWC2_HOST_OFF(HFIR),
                  dwc2_rd32(bus, DWC2_HOST_OFF(HFIR)) | USB_OTG_HFIR_RELOAD_CTRL);
    }

    for (uint8_t i = 0U; i < g_dwc2_hcd[bus->hcd.hcd_id].hw_params.host_channels; i++) {
        dwc2_hc_wr32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCINT), 0xFFFFFFFFU);
        dwc2_hc_wr32(bus, i, offsetof(DWC2_HostChannelTypeDef, HCINTMSK), 0U);
    }

    dwc2_wr32(bus, DWC2_GLB_OFF(GINTMSK), 0U);
    dwc2_wr32(bus, DWC2_GLB_OFF(GINTSTS), 0xFFFFFFFFU);

    dwc2_wr32(bus, DWC2_GLB_OFF(GRXFSIZ), g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_rx_fifo_size);
    dwc2_wr32(bus, DWC2_GLB_OFF(DIEPTXF0_HNPTXFSIZ),
              (uint32_t)(((g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_nperio_tx_fifo_size << 16) & USB_OTG_NPTXFD) |
                         g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_rx_fifo_size));
    dwc2_wr32(bus, DWC2_GLB_OFF(HPTXFSIZ),
              (uint32_t)(((g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_perio_tx_fifo_size << 16) & USB_OTG_HPTXFSIZ_PTXFD) |
                         (g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_rx_fifo_size +
                          g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_nperio_tx_fifo_size)));

    ret = dwc2_flush_txfifo(bus, 0x10U);
    ret = dwc2_flush_rxfifo(bus);

    dwc2_wr32(bus, DWC2_GLB_OFF(GAHBCFG),
              dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)) & ~USB_OTG_GAHBCFG_HBSTLEN);
    if (g_dwc2_hcd[bus->hcd.hcd_id].hw_params.snpsid == 0x4f54280aU) {
        /* BCM283x: this DWC2 was synthesised with an AXI master, and the
         * GAHBCFG bits [4:1] do NOT follow the Synopsys HBSTLEN encoding:
         * bits[2:1] = max AXI burst, bit4 = wait for AXI writes.
         * Linux dwc2_set_bcm_params() programs ahbcfg=0x10 (wait-axi-writes,
         * burst 0); Circle/USPI do the same. Synopsys-style INCRx values put
         * garbage in these fields and the DMA fetch never completes. */
        dwc2_wr32(bus, DWC2_GLB_OFF(GAHBCFG),
                  dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)) | (1U << 4));
    } else {
        dwc2_wr32(bus, DWC2_GLB_OFF(GAHBCFG),
                  dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)) | USB_OTG_GAHBCFG_HBSTLEN_4);
    }
    dwc2_wr32(bus, DWC2_GLB_OFF(GAHBCFG),
              dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)) | USB_OTG_GAHBCFG_DMAEN);

    dwc2_wr32(bus, DWC2_GLB_OFF(GINTMSK),
              dwc2_rd32(bus, DWC2_GLB_OFF(GINTMSK)) |
                  (USB_OTG_GINTMSK_SOFM | USB_OTG_GINTMSK_PRTIM |
                   USB_OTG_GINTMSK_HCIM | USB_OTG_GINTSTS_DISCINT));

    dwc2_drivebus(bus, 1);
    usb_osal_msleep(200);

    USB_LOG_INFO("Bellatrix DWC2: post-core GUSBCFG=0x%08x HCFG=0x%08x HPRT=0x%08x\r\n",
                 (unsigned int)dwc2_rd32(bus, DWC2_GLB_OFF(GUSBCFG)),
                 (unsigned int)dwc2_rd32(bus, DWC2_HOST_OFF(HCFG)),
                 (unsigned int)dwc2_rd32(bus, DWC2_HOST_OFF(HPRT)));

    if (dwc2_rd32(bus, DWC2_HOST_OFF(HPRT)) & USB_OTG_HPRT_PCSTS) {
        g_dwc2_hcd[bus->hcd.hcd_id].port_csc = 1;
        bus->hcd.roothub.int_buffer[0] = (1 << 1);
        bus->hcd.roothub.int_buffer[1] = 0;
        USB_LOG_INFO("Bellatrix DWC2: seeded root-hub wakeup hprt=0x%08x csc=%u intbuf=0x%02x\r\n",
                     (unsigned int)dwc2_rd32(bus, DWC2_HOST_OFF(HPRT)),
                     (unsigned int)g_dwc2_hcd[bus->hcd.hcd_id].port_csc,
                     (unsigned int)bus->hcd.roothub.int_buffer[0]);
        usbh_hub_thread_wakeup(&bus->hcd.roothub);
    }

    /* ISSUE-0036: mini-UART corruption right around here was non-deterministic
     * and went away whenever diagnostic kprintf/mailbox calls happened to be
     * scattered through this exact spot (removed since -- see git history),
     * which points at bus contention between the DWC2's heavy back-to-back
     * MMIO/DMA setup and the AUX mini-UART peripheral (both share the same
     * internal peripheral bus) rather than a UART-side software bug. The
     * diag calls were accidentally pacing this code; replace that with a
     * deliberate short settle delay instead of relying on print side effects. */
    usb_osal_msleep(5);

    /* ISSUE-0036: re-reverted. Enabling GAHBCFG.GINT was NOT the fix for the
     * mini-UART corruption (that was the CONFIG_USB_ALIGN_SIZE cache-line
     * false-sharing bug in usb_config.h) -- confirmed on hardware that with
     * GINT enabled, every control transfer times out waiting on the
     * completion semaphore (HAINT/HCINT never see a completion the polling
     * loop can observe), because Bellatrix drives CherryUSB cooperatively
     * from usb_host_step()/usb_osal_sem_take() with no real ARM GIC vector
     * servicing the DWC2 IRQ line. Enabling GINT changes the hardware's
     * completion/ack semantics to expect that real interrupt service, which
     * never happens here. Keep it masked; the poll path services
     * GINTSTS/GINTMSK/HCINT directly instead. */
    kprintf("[USB] DWC2 GAHBCFG.GINT: %s (GAHBCFG=0x%08x)\n",
            (dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)) & USB_OTG_GAHBCFG_GINT) ? "enabled" : "masked/skipped",
            (unsigned int)dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)));

    return ret;
}

int usb_hc_deinit(struct usbh_bus *bus)
{
    dwc2_wr32(bus, DWC2_GLB_OFF(GAHBCFG),
              dwc2_rd32(bus, DWC2_GLB_OFF(GAHBCFG)) & ~USB_OTG_GAHBCFG_GINT);

    dwc2_flush_txfifo(bus, 0x10U);
    dwc2_flush_rxfifo(bus);

    for (uint8_t chidx = 0; chidx < g_dwc2_hcd[bus->hcd.hcd_id].hw_params.host_channels; chidx++) {
        dwc2_halt(bus, chidx);
    }

    dwc2_wr32(bus, DWC2_GLB_OFF(GINTMSK), 0U);
    dwc2_wr32(bus, DWC2_HOST_OFF(HAINT), 0xFFFFFFFFU);
    dwc2_wr32(bus, DWC2_GLB_OFF(GINTSTS), 0xFFFFFFFFU);

    dwc2_drivebus(bus, 0);
    usb_osal_msleep(200);

    for (uint8_t chidx = 0; chidx < g_dwc2_hcd[bus->hcd.hcd_id].hw_params.host_channels; chidx++) {
        usb_osal_sem_delete(g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx].waitsem);
    }

    usb_hc_low_level_deinit(bus);
    return 0;
}

uint16_t usbh_get_frame_number(struct usbh_bus *bus)
{
    return (dwc2_rd32(bus, DWC2_HOST_OFF(HFNUM)) & USB_OTG_HFNUM_FRNUM);
}

int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf)
{
    __IO uint32_t hprt0;
    uint8_t nports;
    uint8_t port;
    uint32_t status;

    nports = CONFIG_USBHOST_MAX_RHPORTS;
    port = setup->wIndex;

    if (setup->bmRequestType & USB_REQUEST_RECIPIENT_DEVICE) {
        switch (setup->bRequest) {
            case HUB_REQUEST_CLEAR_FEATURE:
                switch (setup->wValue) {
                    case HUB_FEATURE_HUB_C_LOCALPOWER:
                    case HUB_FEATURE_HUB_C_OVERCURRENT:
                        break;
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_SET_FEATURE:
                switch (setup->wValue) {
                    case HUB_FEATURE_HUB_C_LOCALPOWER:
                    case HUB_FEATURE_HUB_C_OVERCURRENT:
                        break;
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_GET_DESCRIPTOR:
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
                        dwc2_wr32(bus, DWC2_HOST_OFF(HPRT),
                                  dwc2_rd32(bus, DWC2_HOST_OFF(HPRT)) & ~USB_OTG_HPRT_PENA);
                        break;
                    case HUB_PORT_FEATURE_SUSPEND:
                    case HUB_PORT_FEATURE_C_SUSPEND:
                        break;
                    case HUB_PORT_FEATURE_POWER:
                        dwc2_drivebus(bus, 0);
                        break;
                    case HUB_PORT_FEATURE_C_CONNECTION:
                        g_dwc2_hcd[bus->hcd.hcd_id].port_csc = 0;
                        break;
                    case HUB_PORT_FEATURE_C_ENABLE:
                        g_dwc2_hcd[bus->hcd.hcd_id].port_pec = 0;
                        break;
                    case HUB_PORT_FEATURE_C_OVER_CURREN:
                        g_dwc2_hcd[bus->hcd.hcd_id].port_occ = 0;
                        break;
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
                        break;
                    case HUB_PORT_FEATURE_POWER:
                        dwc2_drivebus(bus, 1);
                        break;
                    case HUB_PORT_FEATURE_RESET:
                    {
                        int ret = usbh_reset_port(bus, port);
                        return ret;
                    }
                    default:
                        return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_GET_STATUS:
                if (!port || port > nports) {
                    return -USB_ERR_INVAL;
                }
                hprt0 = dwc2_rd32(bus, DWC2_HOST_OFF(HPRT));
                status = 0;

                if (g_dwc2_hcd[bus->hcd.hcd_id].port_csc)
                    status |= (1 << HUB_PORT_FEATURE_C_CONNECTION);
                if (g_dwc2_hcd[bus->hcd.hcd_id].port_pec)
                    status |= (1 << HUB_PORT_FEATURE_C_ENABLE);
                if (g_dwc2_hcd[bus->hcd.hcd_id].port_occ)
                    status |= (1 << HUB_PORT_FEATURE_C_OVER_CURREN);

                if (hprt0 & USB_OTG_HPRT_PCSTS)
                    status |= (1 << HUB_PORT_FEATURE_CONNECTION);
                if (hprt0 & USB_OTG_HPRT_PENA) {
                    status |= (1 << HUB_PORT_FEATURE_ENABLE);
                    if (usbh_get_port_speed(bus, port) == USB_SPEED_LOW)
                        status |= (1 << HUB_PORT_FEATURE_LOWSPEED);
                    else if (usbh_get_port_speed(bus, port) == USB_SPEED_HIGH)
                        status |= (1 << HUB_PORT_FEATURE_HIGHSPEED);
                }
                if (hprt0 & USB_OTG_HPRT_POCA)
                    status |= (1 << HUB_PORT_FEATURE_OVERCURRENT);
                if (hprt0 & USB_OTG_HPRT_PRST)
                    status |= (1 << HUB_PORT_FEATURE_RESET);
                if (hprt0 & USB_OTG_HPRT_PPWR)
                    status |= (1 << HUB_PORT_FEATURE_POWER);

                if (buf) {
                    buf[0] = (uint8_t)(status & 0xffU);
                    buf[1] = (uint8_t)((status >> 8) & 0xffU);
                    buf[2] = (uint8_t)((status >> 16) & 0xffU);
                    buf[3] = (uint8_t)((status >> 24) & 0xffU);
                }
                break;
            default:
                break;
        }
    }
    return 0;
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    struct dwc2_chan *chan;
    struct usbh_bus *bus;
    size_t flags;
    int ret = 0;
    int chidx;

    if (!urb || !urb->hport || !urb->ep || !urb->hport->bus) {
        return -USB_ERR_INVAL;
    }

    USB_ASSERT_MSG(!((uintptr_t)urb->setup % 4) && !((uintptr_t)urb->transfer_buffer % 4),
                   "urb->setup or urb->transfer_buffer is not aligned 4 bytes");

#ifdef CONFIG_USB_DCACHE_ENABLE
    USB_ASSERT_MSG(!((uintptr_t)urb->setup % CONFIG_USB_ALIGN_SIZE) &&
                       !((uintptr_t)urb->transfer_buffer % CONFIG_USB_ALIGN_SIZE),
                   "urb->setup or urb->transfer_buffer is not aligned %d", CONFIG_USB_ALIGN_SIZE);
#endif

    bus = urb->hport->bus;

    if (!(dwc2_rd32(bus, DWC2_HOST_OFF(HPRT)) & USB_OTG_HPRT_PCSTS) || !urb->hport->connected) {
        return -USB_ERR_NOTCONN;
    }

    if (urb->errorcode == -USB_ERR_BUSY) {
        return -USB_ERR_BUSY;
    }

    if (urb->ep->bEndpointAddress & 0x80) {
        if (USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize) >
            (g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_rx_fifo_size * 4)) {
            return -USB_ERR_RANGE;
        }
    } else {
        if (((USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_ISOCHRONOUS) ||
             (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_INTERRUPT)) &&
            USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize) >
                (g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_perio_tx_fifo_size * 4)) {
            return -USB_ERR_RANGE;
        } else {
            if (USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize) >
                (g_dwc2_hcd[bus->hcd.hcd_id].user_params.host_nperio_tx_fifo_size * 4)) {
                return -USB_ERR_RANGE;
            }
        }
    }

    chidx = dwc2_chan_alloc(bus);
    if (chidx == -1) {
        return -USB_ERR_NOMEM;
    }

    flags = usb_osal_enter_critical_section();

    chan = &g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[chidx];
    chan->chidx = chidx;
    chan->urb   = urb;
    chan->do_ssplit = 0;

    if (urb->hport->speed != USB_SPEED_HIGH &&
        usbh_get_port_speed(bus, 0) == USB_SPEED_HIGH) {
        chan->do_ssplit = 1;
        chan->do_csplit = 0;
        chan->hub_port  = urb->hport->port;
        chan->hub_addr  = urb->hport->parent->hub_addr;
    }

    urb->hcpriv        = chan;
    urb->errorcode     = -USB_ERR_BUSY;
    urb->actual_length = 0;

    usb_osal_leave_critical_section(flags);

    if (urb->setup) {
        usb_dcache_clean((uintptr_t)urb->setup,
                         USB_ALIGN_UP(sizeof(struct usb_setup_packet), CONFIG_USB_ALIGN_SIZE));
        if (urb->transfer_buffer) {
            if (urb->setup->bmRequestType & 0x80) {
                usb_dcache_invalidate((uintptr_t)urb->transfer_buffer,
                                      USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
            } else {
                usb_dcache_clean((uintptr_t)urb->transfer_buffer,
                                 USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
            }
        }
    } else if (urb->transfer_buffer &&
               (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) != USB_ENDPOINT_TYPE_ISOCHRONOUS)) {
        if (urb->ep->bEndpointAddress & 0x80) {
            usb_dcache_invalidate((uintptr_t)urb->transfer_buffer,
                                  USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
        } else {
            usb_dcache_clean((uintptr_t)urb->transfer_buffer,
                             USB_ALIGN_UP(urb->transfer_buffer_length, CONFIG_USB_ALIGN_SIZE));
        }
    }

    switch (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes)) {
        case USB_ENDPOINT_TYPE_CONTROL:
            chan->ep0_state = DWC2_EP0_STATE_SETUP;
            dwc2_control_urb_init(bus, chidx, urb, urb->setup,
                                  urb->transfer_buffer, urb->transfer_buffer_length);
            break;
        case USB_ENDPOINT_TYPE_BULK:
        case USB_ENDPOINT_TYPE_INTERRUPT:
            dwc2_bulk_intr_urb_init(bus, chidx, urb,
                                    urb->transfer_buffer, urb->transfer_buffer_length);
            break;
        case USB_ENDPOINT_TYPE_ISOCHRONOUS:
            break;
        default:
            break;
    }

    if (urb->timeout > 0) {
        ret = usb_osal_sem_take(chan->waitsem, urb->timeout);
        if (ret < 0) {
            goto errout_timeout;
        }
        urb->timeout = 0;
        ret = urb->errorcode;
        dwc2_chan_free(chan);
    }
    return ret;

errout_timeout:
    urb->timeout = 0;
    usbh_kill_urb(urb);
    return ret;
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    struct dwc2_chan *chan;
    struct usbh_bus *bus;
    size_t flags;

    if (!urb || !urb->hcpriv || !urb->hport->bus) {
        return -USB_ERR_INVAL;
    }

    bus = urb->hport->bus;

    flags = usb_osal_enter_critical_section();

    chan = (struct dwc2_chan *)urb->hcpriv;

    dwc2_halt(bus, chan->chidx);

    urb->errorcode = -USB_ERR_SHUTDOWN;

    if (urb->timeout) {
        usb_osal_sem_give(chan->waitsem);
    } else {
        dwc2_chan_free(chan);
    }

    if (urb->complete) {
        urb->complete(urb->arg, urb->errorcode);
    }

    usb_osal_leave_critical_section(flags);

    return 0;
}

static inline void dwc2_urb_waitup(struct usbh_urb *urb)
{
    struct dwc2_chan *chan;

    chan = (struct dwc2_chan *)urb->hcpriv;

    if (urb->timeout) {
        usb_osal_sem_give(chan->waitsem);
    } else {
        dwc2_chan_free(chan);
    }

    if (urb->complete) {
        if (urb->errorcode < 0) {
            urb->complete(urb->arg, urb->errorcode);
        } else {
            urb->complete(urb->arg, urb->actual_length);
        }
    }
}

static void dwc2_inchan_irq_handler(struct usbh_bus *bus, uint8_t ch_num)
{
    uint32_t chan_intstatus;
    struct dwc2_chan *chan;
    struct usbh_urb *urb;

    chan_intstatus = dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINT));

    chan = &g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[ch_num];
    urb = chan->urb;

    if (urb && USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_CONTROL) {
        USB_LOG_INFO("Bellatrix DWC2: in irq ch=%u hcint=0x%08x state=%u hctsiz=0x%08x hcchar=0x%08x err=%d actual=%u remain=%u\r\n",
                     (unsigned int)ch_num, (unsigned int)chan_intstatus,
                     (unsigned int)chan->ep0_state,
                     (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCTSIZ)),
                     (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR)),
                     urb->errorcode, (unsigned int)urb->actual_length,
                     (unsigned int)urb->transfer_buffer_length);
    }

    if (chan_intstatus & USB_OTG_HCINT_CHH) {
        dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINT), chan_intstatus);

        if (urb && USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) != USB_ENDPOINT_TYPE_CONTROL &&
            (chan_intstatus & (USB_OTG_HCINT_AHBERR | USB_OTG_HCINT_STALL | USB_OTG_HCINT_TXERR |
                               USB_OTG_HCINT_BBERR | USB_OTG_HCINT_DTERR | USB_OTG_HCINT_FRMOR))) {
            USB_LOG_INFO("Bellatrix DWC2: bulk/intr in err ch=%u hcint=0x%08x ep=0x%02x hctsiz=0x%08x hcchar=0x%08x\r\n",
                         (unsigned int)ch_num, (unsigned int)chan_intstatus,
                         (unsigned int)urb->ep->bEndpointAddress,
                         (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCTSIZ)),
                         (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR)));
        }

        if (chan_intstatus & USB_OTG_HCINT_XFRC) {
            uint32_t hctsiz = dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCTSIZ));
            uint32_t count  = chan->xferlen - (hctsiz & USB_OTG_HCTSIZ_XFRSIZ);
            uint8_t data_toggle = ((hctsiz & USB_OTG_HCTSIZ_DPID) >> USB_OTG_HCTSIZ_DPID_Pos);

            urb->actual_length += count;
            urb->transfer_buffer_length -= count;

            urb->data_toggle = (data_toggle == HC_PID_DATA0) ? 0 : 1;

            if (chan->do_csplit) {
                chan->do_csplit = 0;
                dwc2_chan_enable_csplit(bus, ch_num, false);
            }

            if (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_CONTROL) {
                if (chan->ep0_state == DWC2_EP0_STATE_INDATA) {
                    if (chan->do_ssplit && urb->transfer_buffer_length > 0 &&
                        (count == USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize))) {
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer + urb->actual_length - 8,
                                              urb->transfer_buffer_length);
                    } else {
                        chan->ep0_state = DWC2_EP0_STATE_OUTSTATUS;
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer, urb->transfer_buffer_length);
                    }
                } else if (chan->ep0_state == DWC2_EP0_STATE_INSTATUS) {
                    chan->ep0_state = DWC2_EP0_STATE_SETUP;
                    urb->errorcode = 0;
                    dwc2_urb_waitup(urb);
                }
            } else {
                if (chan->do_ssplit && urb->transfer_buffer_length > 0 &&
                    (count == USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize))) {
                    dwc2_bulk_intr_urb_init(bus, ch_num, urb,
                                            urb->transfer_buffer + urb->actual_length,
                                            urb->transfer_buffer_length);
                } else {
                    usb_dcache_invalidate((uintptr_t)urb->transfer_buffer,
                                         USB_ALIGN_UP(urb->actual_length, CONFIG_USB_ALIGN_SIZE));
                    urb->errorcode = 0;
                    dwc2_urb_waitup(urb);
                }
            }
        } else if (chan_intstatus & USB_OTG_HCINT_AHBERR) {
            urb->errorcode = -USB_ERR_IO;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_STALL) {
            urb->errorcode = -USB_ERR_STALL;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_NAK) {
            if (chan->do_ssplit) {
                switch (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes)) {
                    case USB_ENDPOINT_TYPE_CONTROL:
                        chan->do_csplit = 0;
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer + urb->actual_length - 8,
                                              urb->transfer_buffer_length);
                        break;
                    case USB_ENDPOINT_TYPE_BULK:
                        chan->do_csplit = 0;
                        dwc2_bulk_intr_urb_init(bus, ch_num, urb,
                                                urb->transfer_buffer + urb->actual_length,
                                                urb->transfer_buffer_length);
                        break;
                    case USB_ENDPOINT_TYPE_INTERRUPT:
                        chan->do_csplit = 0;
                        dwc2_chan_enable_csplit(bus, ch_num, false);
                        urb->errorcode = -USB_ERR_NAK;
                        dwc2_urb_waitup(urb);
                        break;
                    default:
                        break;
                }
            } else {
                urb->errorcode = -USB_ERR_NAK;
                dwc2_urb_waitup(urb);
            }
        } else if (chan_intstatus & USB_OTG_HCINT_ACK) {
            if (chan->do_ssplit) {
                chan->do_csplit   = 1;
                chan->ssplit_frame = dwc2_get_full_frame_num(bus);
                switch (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes)) {
                    case USB_ENDPOINT_TYPE_CONTROL:
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer + urb->actual_length - 8,
                                              urb->transfer_buffer_length);
                        break;
                    case USB_ENDPOINT_TYPE_BULK:
                    case USB_ENDPOINT_TYPE_INTERRUPT:
                        dwc2_bulk_intr_urb_init(bus, ch_num, urb,
                                                urb->transfer_buffer + urb->actual_length,
                                                urb->transfer_buffer_length);
                        break;
                    default:
                        break;
                }
            }
        } else if (chan_intstatus & USB_OTG_HCINT_NYET) {
            if (chan->do_ssplit) {
                chan->do_csplit   = 1;
                chan->ssplit_frame = dwc2_get_full_frame_num(bus);
                switch (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes)) {
                    case USB_ENDPOINT_TYPE_CONTROL:
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer + urb->actual_length - 8,
                                              urb->transfer_buffer_length);
                        break;
                    case USB_ENDPOINT_TYPE_BULK:
                    case USB_ENDPOINT_TYPE_INTERRUPT:
                        dwc2_bulk_intr_urb_init(bus, ch_num, urb,
                                                urb->transfer_buffer + urb->actual_length,
                                                urb->transfer_buffer_length);
                        break;
                    default:
                        break;
                }
            } else {
                urb->errorcode = -USB_ERR_NAK;
                dwc2_urb_waitup(urb);
            }
        } else if (chan_intstatus & USB_OTG_HCINT_TXERR) {
            urb->errorcode = -USB_ERR_IO;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_BBERR) {
            urb->errorcode = -USB_ERR_BABBLE;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_DTERR) {
            urb->errorcode = -USB_ERR_DT;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_FRMOR) {
            urb->errorcode = -USB_ERR_IO;
            dwc2_urb_waitup(urb);
        }
    }
}

static void dwc2_outchan_irq_handler(struct usbh_bus *bus, uint8_t ch_num)
{
    uint32_t chan_intstatus;
    struct dwc2_chan *chan;
    struct usbh_urb *urb;

    chan_intstatus = dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINT));

    chan = &g_dwc2_hcd[bus->hcd.hcd_id].chan_pool[ch_num];
    urb = chan->urb;

    if (urb && USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_CONTROL) {
        USB_LOG_INFO("Bellatrix DWC2: out irq ch=%u hcint=0x%08x state=%u hctsiz=0x%08x hcchar=0x%08x err=%d actual=%u remain=%u\r\n",
                     (unsigned int)ch_num, (unsigned int)chan_intstatus,
                     (unsigned int)chan->ep0_state,
                     (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCTSIZ)),
                     (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR)),
                     urb->errorcode, (unsigned int)urb->actual_length,
                     (unsigned int)urb->transfer_buffer_length);
    }

    if (chan_intstatus & USB_OTG_HCINT_CHH) {
        dwc2_hc_wr32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCINT), chan_intstatus);

        if (urb && USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) != USB_ENDPOINT_TYPE_CONTROL &&
            (chan_intstatus & (USB_OTG_HCINT_AHBERR | USB_OTG_HCINT_STALL | USB_OTG_HCINT_TXERR |
                               USB_OTG_HCINT_BBERR | USB_OTG_HCINT_DTERR | USB_OTG_HCINT_FRMOR))) {
            USB_LOG_INFO("Bellatrix DWC2: bulk/intr out err ch=%u hcint=0x%08x ep=0x%02x hctsiz=0x%08x hcchar=0x%08x\r\n",
                         (unsigned int)ch_num, (unsigned int)chan_intstatus,
                         (unsigned int)urb->ep->bEndpointAddress,
                         (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCTSIZ)),
                         (unsigned int)dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCCHAR)));
        }

        if (chan_intstatus & USB_OTG_HCINT_XFRC) {
            uint32_t hctsiz = dwc2_hc_rd32(bus, ch_num, offsetof(DWC2_HostChannelTypeDef, HCTSIZ));
            uint32_t count  = hctsiz & USB_OTG_HCTSIZ_XFRSIZ;
            uint32_t olen   = 0;
            uint8_t data_toggle = ((hctsiz & USB_OTG_HCTSIZ_DPID) >> USB_OTG_HCTSIZ_DPID_Pos);

            /* Zero-length control OUT status stages do not carry payload bytes.
             * The generic OUT packet accounting underflows there because the
             * hardware reports PKTCNT still equal to the original 1 packet. */
            if (!(USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_CONTROL &&
                  chan->ep0_state == DWC2_EP0_STATE_OUTSTATUS)) {
                uint32_t has_used_packets = chan->num_packets - ((hctsiz & USB_OTG_HCTSIZ_PKTCNT) >> 19);
                olen = (has_used_packets - 1) * USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize) + count;
                urb->actual_length += olen;
            }

            if (chan->ep0_state == DWC2_EP0_STATE_OUTDATA || urb->setup == NULL) {
                if (urb->transfer_buffer_length > olen) {
                    urb->transfer_buffer_length -= olen;
                } else {
                    urb->transfer_buffer_length = 0;
                }
            }

            urb->data_toggle = (data_toggle == HC_PID_DATA0) ? 0 : 1;

            if (chan->do_csplit) {
                chan->do_csplit = 0;
                dwc2_chan_enable_csplit(bus, ch_num, false);
            }

            if (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_CONTROL) {
                if (chan->ep0_state == DWC2_EP0_STATE_SETUP) {
                    if (urb->setup->wLength) {
                        if (urb->setup->bmRequestType & 0x80) {
                            chan->ep0_state = DWC2_EP0_STATE_INDATA;
                        } else {
                            chan->ep0_state = DWC2_EP0_STATE_OUTDATA;
                        }
                    } else {
                        chan->ep0_state = DWC2_EP0_STATE_INSTATUS;
                    }
                    dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                          urb->transfer_buffer, urb->transfer_buffer_length);
                } else if (chan->ep0_state == DWC2_EP0_STATE_OUTDATA) {
                    if (chan->do_ssplit && urb->transfer_buffer_length > 0) {
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer + urb->actual_length - 8,
                                              urb->transfer_buffer_length);
                    } else {
                        chan->ep0_state = DWC2_EP0_STATE_INSTATUS;
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer, urb->transfer_buffer_length);
                    }
                } else if (chan->ep0_state == DWC2_EP0_STATE_OUTSTATUS) {
                    /* For control-IN transfers urb->actual_length already tracks
                     * only the DATA stage bytes. The setup packet lives in the
                     * dedicated LE bounce buffer, not in transfer_buffer, so
                     * subtracting 8 here leaves the first descriptor bytes
                     * stale in cache and can corrupt enumeration/parsing. */
                    usb_dcache_invalidate((uintptr_t)urb->transfer_buffer,
                                         USB_ALIGN_UP(urb->actual_length, CONFIG_USB_ALIGN_SIZE));
                    chan->ep0_state = DWC2_EP0_STATE_SETUP;
                    urb->errorcode  = 0;
                    dwc2_urb_waitup(urb);
                }
            } else {
                if (chan->do_ssplit && urb->transfer_buffer_length > 0) {
                    dwc2_bulk_intr_urb_init(bus, ch_num, urb,
                                            urb->transfer_buffer + urb->actual_length,
                                            urb->transfer_buffer_length);
                } else {
                    urb->errorcode = 0;
                    dwc2_urb_waitup(urb);
                }
            }
        } else if (chan_intstatus & USB_OTG_HCINT_AHBERR) {
            urb->errorcode = -USB_ERR_IO;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_STALL) {
            urb->errorcode = -USB_ERR_STALL;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_NAK) {
            if (chan->do_ssplit) {
                switch (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes)) {
                    case USB_ENDPOINT_TYPE_CONTROL:
                        chan->do_csplit = 0;
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer + urb->actual_length - 8,
                                              urb->transfer_buffer_length);
                        break;
                    case USB_ENDPOINT_TYPE_BULK:
                        chan->do_csplit = 0;
                        dwc2_bulk_intr_urb_init(bus, ch_num, urb,
                                                urb->transfer_buffer + urb->actual_length,
                                                urb->transfer_buffer_length);
                        break;
                    case USB_ENDPOINT_TYPE_INTERRUPT:
                        chan->do_csplit = 0;
                        dwc2_chan_enable_csplit(bus, ch_num, false);
                        urb->errorcode = -USB_ERR_NAK;
                        dwc2_urb_waitup(urb);
                        break;
                    default:
                        break;
                }
            } else {
                urb->errorcode = -USB_ERR_NAK;
                dwc2_urb_waitup(urb);
            }
        } else if (chan_intstatus & USB_OTG_HCINT_ACK) {
            if (chan->do_ssplit) {
                chan->do_csplit   = 1;
                chan->ssplit_frame = dwc2_get_full_frame_num(bus);
                switch (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes)) {
                    case USB_ENDPOINT_TYPE_CONTROL:
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer + urb->actual_length - 8,
                                              urb->transfer_buffer_length);
                        break;
                    case USB_ENDPOINT_TYPE_BULK:
                    case USB_ENDPOINT_TYPE_INTERRUPT:
                        dwc2_bulk_intr_urb_init(bus, ch_num, urb,
                                                urb->transfer_buffer + urb->actual_length,
                                                urb->transfer_buffer_length);
                        break;
                    default:
                        break;
                }
            }
        } else if (chan_intstatus & USB_OTG_HCINT_NYET) {
            if (chan->do_ssplit) {
                chan->do_csplit   = 1;
                chan->ssplit_frame = dwc2_get_full_frame_num(bus);
                switch (USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes)) {
                    case USB_ENDPOINT_TYPE_CONTROL:
                        dwc2_control_urb_init(bus, ch_num, urb, urb->setup,
                                              urb->transfer_buffer + urb->actual_length - 8,
                                              urb->transfer_buffer_length);
                        break;
                    case USB_ENDPOINT_TYPE_BULK:
                    case USB_ENDPOINT_TYPE_INTERRUPT:
                        dwc2_bulk_intr_urb_init(bus, ch_num, urb,
                                                urb->transfer_buffer + urb->actual_length,
                                                urb->transfer_buffer_length);
                        break;
                    default:
                        break;
                }
            } else {
                urb->errorcode = -USB_ERR_NAK;
                dwc2_urb_waitup(urb);
            }
        } else if (chan_intstatus & USB_OTG_HCINT_TXERR) {
            urb->errorcode = -USB_ERR_IO;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_BBERR) {
            urb->errorcode = -USB_ERR_BABBLE;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_DTERR) {
            urb->errorcode = -USB_ERR_DT;
            dwc2_urb_waitup(urb);
        } else if (chan_intstatus & USB_OTG_HCINT_FRMOR) {
            urb->errorcode = -USB_ERR_IO;
            dwc2_urb_waitup(urb);
        }
    }
}

static void dwc2_port_irq_handler(struct usbh_bus *bus)
{
    __IO uint32_t hprt0, hprt0_dup;

    hprt0 = dwc2_rd32(bus, DWC2_HOST_OFF(HPRT));
    hprt0_dup = hprt0;
    USB_LOG_INFO("Bellatrix DWC2: HPRT irq raw=0x%08x\r\n", (unsigned int)hprt0);

    hprt0_dup &= ~(USB_OTG_HPRT_PENA | USB_OTG_HPRT_PCDET |
                   USB_OTG_HPRT_PENCHNG | USB_OTG_HPRT_POCCHNG);

    if ((hprt0 & USB_OTG_HPRT_PCDET) == USB_OTG_HPRT_PCDET) {
        if ((hprt0 & USB_OTG_HPRT_PCSTS) == USB_OTG_HPRT_PCSTS &&
            (hprt0 & USB_OTG_HPRT_PENA) == USB_OTG_HPRT_PENA) {
            /* PCDET with the port already ENABLED is the tail of a port reset
             * we issued ourselves (the irq is serviced later, from sem_take
             * pumping during the first control transfer). A genuine new
             * connect always arrives with PENA=0 — the port only becomes
             * enabled after the reset that follows it. Surfacing this as
             * C_CONNECTION makes the hub thread tear down the device it just
             * enumerated (LAN9514 unregistered right after interface_start). */
            USB_LOG_INFO("Bellatrix DWC2: self-reset PCDET suppressed hprt=0x%08x\r\n",
                         (unsigned int)hprt0);
        } else if ((hprt0 & USB_OTG_HPRT_PCSTS) == USB_OTG_HPRT_PCSTS) {
            bus->hcd.roothub.int_buffer[0] = (1 << 1);
            bus->hcd.roothub.int_buffer[1] = 0;
            usbh_hub_thread_wakeup(&bus->hcd.roothub);
            g_dwc2_hcd[bus->hcd.hcd_id].port_csc = 1;
            USB_LOG_INFO("Bellatrix DWC2: connect detected hprt=0x%08x intbuf=0x%02x\r\n",
                         (unsigned int)hprt0, (unsigned int)bus->hcd.roothub.int_buffer[0]);
        } else {
            g_dwc2_hcd[bus->hcd.hcd_id].port_csc = 1;
            USB_LOG_INFO("Bellatrix DWC2: disconnect detected hprt=0x%08x\r\n", (unsigned int)hprt0);
        }
        hprt0_dup |= USB_OTG_HPRT_PCDET;
    }

    if ((hprt0 & USB_OTG_HPRT_PENCHNG) == USB_OTG_HPRT_PENCHNG) {
        hprt0_dup |= USB_OTG_HPRT_PENCHNG;
        g_dwc2_hcd[bus->hcd.hcd_id].port_pec = 1;

        if ((hprt0 & USB_OTG_HPRT_PENA) == USB_OTG_HPRT_PENA) {
            dwc2_apply_port_speed_config(bus, hprt0);
        }
    }

    if ((hprt0 & USB_OTG_HPRT_POCCHNG) == USB_OTG_HPRT_POCCHNG) {
        hprt0_dup |= USB_OTG_HPRT_POCCHNG;
        g_dwc2_hcd[bus->hcd.hcd_id].port_occ = 1;
    }

    dwc2_wr32(bus, DWC2_HOST_OFF(HPRT), hprt0_dup);
}

void USBH_IRQHandler(uint8_t busid)
{
    uint32_t gint_status;
    struct usbh_bus *bus;

    bus = &g_usbhost_bus[busid];
    gint_status = dwc2_get_glb_intstatus(bus);

    if ((dwc2_rd32(bus, DWC2_GLB_OFF(GINTSTS)) & 0x1U) == USB_OTG_MODE_HOST) {
        if (gint_status == 0) {
            dwc2_service_pending_channels(bus, true);
            return;
        }

        if (gint_status & USB_OTG_GINTSTS_HPRTINT) {
            dwc2_port_irq_handler(bus);
        }
        if (gint_status & USB_OTG_GINTSTS_SOF) {
            dwc2_sof_kick_pending_channels(bus);
            dwc2_wr32(bus, DWC2_GLB_OFF(GINTSTS), USB_OTG_GINTSTS_SOF);
        }
        if (gint_status & USB_OTG_GINTSTS_DISCINT) {
            g_dwc2_hcd[bus->hcd.hcd_id].port_csc = 1;
            bus->hcd.roothub.int_buffer[0] = (1 << 1);
            usbh_hub_thread_wakeup(&bus->hcd.roothub);
            dwc2_wr32(bus, DWC2_GLB_OFF(GINTSTS), USB_OTG_GINTSTS_DISCINT);
        }
        if (gint_status & USB_OTG_GINTSTS_HCINT) {
            dwc2_service_pending_channels(bus, false);
            dwc2_wr32(bus, DWC2_GLB_OFF(GINTSTS), USB_OTG_GINTSTS_HCINT);
        }
    }
}
