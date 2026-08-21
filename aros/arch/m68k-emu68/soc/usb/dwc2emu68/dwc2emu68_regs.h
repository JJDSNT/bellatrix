#ifndef DWC2EMU68_REGS_H
#define DWC2EMU68_REGS_H

#define DWC2_GAHBCFG            0x0008
#define DWC2_GUSBCFG            0x000c
#define DWC2_GRSTCTL            0x0010
#define DWC2_GINTSTS            0x0014
#define DWC2_GINTMSK            0x0018
#define DWC2_GRXFSIZ            0x0024
#define DWC2_GNPTXFSIZ          0x0028
#define DWC2_GHWCFG2            0x0048
#define DWC2_GHWCFG3            0x004c
#define DWC2_HPTXFSIZ           0x0100
#define DWC2_HCFG               0x0400
#define DWC2_HFIR               0x0404
#define DWC2_HFNUM              0x0408
#define DWC2_HAINT              0x0414
#define DWC2_HAINTMSK           0x0418
#define DWC2_HPRT               0x0440
#define DWC2_HC_BASE(n)         (0x0500 + ((n) * 0x20))
#define DWC2_HCCHAR(n)          (DWC2_HC_BASE(n) + 0x00)
#define DWC2_HCSPLT(n)          (DWC2_HC_BASE(n) + 0x04)
#define DWC2_HCINT(n)           (DWC2_HC_BASE(n) + 0x08)
#define DWC2_HCINTMSK(n)        (DWC2_HC_BASE(n) + 0x0c)
#define DWC2_HCTSIZ(n)          (DWC2_HC_BASE(n) + 0x10)
#define DWC2_HCDMA(n)           (DWC2_HC_BASE(n) + 0x14)
#define DWC2_PCGCCTL            0x0e00

#define DWC2_GAHBCFG_GLBLINTRMSK        (1UL << 0)
#define DWC2_GAHBCFG_HBSTLEN_INCR       (1UL << 1)
#define DWC2_GAHBCFG_WAIT_AXI_WRITES    (1UL << 4)
#define DWC2_GAHBCFG_DMAEN              (1UL << 5)
#define DWC2_GAHBCFG_NPTXFEMPLVL        (1UL << 7)

#define DWC2_GUSBCFG_FORCEHOSTMODE      (1UL << 29)
#define DWC2_GUSBCFG_FORCEDEVMODE       (1UL << 30)

#define DWC2_GRSTCTL_CSFTRST            (1UL << 0)
#define DWC2_GRSTCTL_RXFFLSH            (1UL << 4)
#define DWC2_GRSTCTL_TXFFLSH            (1UL << 5)
#define DWC2_GRSTCTL_TXFNUM_ALL         (0x10UL << 6)
#define DWC2_GRSTCTL_AHBIDLE            (1UL << 31)

#define DWC2_GINTSTS_SOF                (1UL << 3)
#define DWC2_GINTSTS_PRTINT             (1UL << 24)
#define DWC2_GINTSTS_HCHINT             (1UL << 25)
#define DWC2_GINTSTS_DISCONNINT         (1UL << 29)

#define DWC2_HPRT_CONNSTS               (1UL << 0)
#define DWC2_HPRT_CONNDET               (1UL << 1)
#define DWC2_HPRT_ENA                   (1UL << 2)
#define DWC2_HPRT_ENCHNG                (1UL << 3)
#define DWC2_HPRT_OVRCURRCHNG           (1UL << 5)
#define DWC2_HPRT_RST                   (1UL << 8)
#define DWC2_HPRT_PWR                   (1UL << 12)
#define DWC2_HPRT_CHANGE_BITS           (DWC2_HPRT_CONNDET | DWC2_HPRT_ENCHNG | DWC2_HPRT_OVRCURRCHNG)

#define DWC2_HCCHAR_EPDIR_IN            (1UL << 15)
#define DWC2_HCCHAR_EPTYPE_CONTROL      (0UL << 18)
#define DWC2_HCCHAR_EPTYPE_BULK         (2UL << 18)
#define DWC2_HCCHAR_EPTYPE_INTERRUPT    (3UL << 18)
#define DWC2_HCCHAR_DEVADDR(v)          (((ULONG)(v) & 0x7f) << 22)
#define DWC2_HCCHAR_EPNUM(v)            (((ULONG)(v) & 0x0f) << 11)
#define DWC2_HCCHAR_MPS(v)              ((ULONG)(v) & 0x7ff)
#define DWC2_HCCHAR_MULTICNT_ONE        (1UL << 20)
#define DWC2_HCCHAR_ODDFRM              (1UL << 29)
#define DWC2_HCCHAR_CHDIS               (1UL << 30)
#define DWC2_HCCHAR_CHENA               (1UL << 31)

#define DWC2_HCTSIZ_XFERSIZE(v)         ((ULONG)(v) & 0x7ffff)
#define DWC2_HCTSIZ_PKTCNT(v)           (((ULONG)(v) & 0x3ff) << 19)
#define DWC2_HCTSIZ_PID_DATA0           (0UL << 29)
#define DWC2_HCTSIZ_PID_DATA1           (2UL << 29)
#define DWC2_HCTSIZ_PID_SETUP           (3UL << 29)

#define DWC2_HCINT_XFERCOMP             (1UL << 0)
#define DWC2_HCINT_CHHLTD               (1UL << 1)
#define DWC2_HCINT_AHBERR               (1UL << 2)
#define DWC2_HCINT_STALL                (1UL << 3)
#define DWC2_HCINT_NAK                  (1UL << 4)
#define DWC2_HCINT_ACK                  (1UL << 5)
#define DWC2_HCINT_NYET                 (1UL << 6)
#define DWC2_HCINT_XACTERR              (1UL << 7)
#define DWC2_HCINT_BBLERR               (1UL << 8)
#define DWC2_HCINT_FRMOVRUN             (1UL << 9)
#define DWC2_HCINT_DATATGLERR           (1UL << 10)
#define DWC2_HCINT_ALL                  0x3fffUL

#define DWC2_GHWCFG2_NUM_HOST_CHAN(v)   ((((v) >> 14) & 0x0f) + 1)

#endif
