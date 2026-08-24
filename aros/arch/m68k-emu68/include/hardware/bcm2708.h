/*
    Copyright (C) 2026, The Bellatrix Project. All rights reserved.

    Desc: BCM283x interrupt numbering for the legacy interrupt controller.
*/

#ifndef HARDWARE_BCM2708_H
#define HARDWARE_BCM2708_H

/*
 * The numbering arch/arm-native uses, carried here rather than included.
 *
 * The definitions below are copied verbatim from
 * arch/arm-native/soc/broadcom/2708/include/hardware/bcm2708.h so the numbers
 * have one origin. That file is not put on this target's include path because
 * its directory also holds a videocore.h and an arasan.h, and this port has
 * its own with the same names -- adding the -I would make which one wins
 * depend on include order.
 *
 * Only the interrupt map is carried. The register macros in that header expand
 * against ARM_PERIIOBASE, a compile-time constant this port deliberately does
 * not have: the peripheral base is discovered at runtime from the device tree
 * and reaches drivers through KrnGetSystemAttr(KATTR_PeripheralBase).
 *
 * Two drivers were each defining the one number they needed, which is how a
 * shared numbering quietly becomes two private ones that can disagree.
 */

#define IRQ_MASK(irq) (1 << ((irq) & 0x1f))
#define IRQ_BANK(irq) ((irq) >> 5)

#define GPUIRQ0_BASE  (0 << 5)
#define IRQ_VC_USB    (GPUIRQ0_BASE + 9)
#define IRQ_AUX       (GPUIRQ0_BASE + 29)

#define IRQ_DMA0      (GPUIRQ0_BASE + 16)

#define GPUIRQ1_BASE       (1 << 5)
#define IRQ_VC_ARASANSDIO  (GPUIRQ1_BASE + 30)
#define IRQ_VC_UART        (GPUIRQ1_BASE + 25)
#define IRQ_VC_SDIO        (GPUIRQ1_BASE + 24)

#define ARMIRQ_BASE   (2 << 5)

/*
 * Register macros, for the case where the note above does not apply.
 *
 * The peripheral base is discovered at runtime here, so a header that expands
 * addresses against a compile-time ARM_PERIIOBASE is normally useless to this
 * port. arch/arm-native's own drivers show the way around it: dma_private.h
 * and sdcard_sdhost_intern.h each define ARM_PERIIOBASE as a *runtime
 * expression* immediately before including this header, so the macros expand
 * against whatever the including file decided the base is.
 *
 * They are deliberately *not* guarded on ARM_PERIIOBASE being defined here.
 * A macro is only expanded where it is used, so an unused definition costs
 * nothing -- while a guard inside an include guard is a trap: the first
 * translation unit to reach this file before defining ARM_PERIIOBASE takes
 * the guard, and every later include is a no-op, so the block silently never
 * appears. That cost an hour. arch/arm-native defines them unconditionally
 * for the same reason.
 *
 * Copied verbatim from the same arm-native header as the interrupt map above,
 * for the same reason: one origin for the numbers.
 */

/*
 * The SoC bases. Nothing here runs on a BCM2711 or BCM2712, but drivers shared
 * with arch/arm-native test for them to select SoC-specific implementations,
 * so the constants have to exist. PERIBUSBASE is the VideoCore's view of the
 * peripherals, which is what a DMA descriptor has to carry.
 */
#define BCM2711_PERIIOBASE   0xFE000000
#define BCM2712_PERIIOBASE   0x107C000000ULL
#define BCM2835_PERIBUSBASE  0x7E000000

#define GPIO_BASE           (ARM_PERIIOBASE + 0x200000)
#define GPFSEL0             (GPIO_BASE + 0x00)
#define GPFSEL1             (GPIO_BASE + 0x04)
#define GPFSEL2             (GPIO_BASE + 0x08)
#define GPFSEL3             (GPIO_BASE + 0x0C)
#define GPFSEL4             (GPIO_BASE + 0x10)
#define GPFSEL5             (GPIO_BASE + 0x14)
#define GPSET0              (GPIO_BASE + 0x1C)
#define GPSET1              (GPIO_BASE + 0x20)
#define GPCLR0              (GPIO_BASE + 0x28)
#define GPCLR1              (GPIO_BASE + 0x2C)
#define GPPUD               (GPIO_BASE + 0x94)
#define GPPUDCLK0           (GPIO_BASE + 0x98)
#define GPPUDCLK1           (GPIO_BASE + 0x9C)

/*
 * Guarded because soc/sdcard's own private header defines it too, and a
 * translation unit that reaches this file through a driver shared with
 * arch/arm-native can see both.
 */
#ifndef BCM2835_PERIPHYSBASE
#define BCM2835_PERIPHYSBASE 0x20000000
#endif

/*
 * And the one this machine actually has.
 *
 * BCM2836 -- the Pi 2 and Pi 3 -- moved the peripheral window to 0x3f000000.
 * This header carried only the BCM2835 value, so any driver shared with
 * arch/arm-native that tests for the Pi 3 base did not compile here; the
 * arm-native copy of this file has defined both from the start.
 */
#ifndef BCM2836_PERIPHYSBASE
#define BCM2836_PERIPHYSBASE 0x3f000000
#endif

#define SYSTIMER_BASE       (ARM_PERIIOBASE + 0x003000)
#define SYSTIMER_CS         (SYSTIMER_BASE + 0x00)
#define SYSTIMER_CLO        (SYSTIMER_BASE + 0x04)
#define SYSTIMER_CHI        (SYSTIMER_BASE + 0x08)

#define DMA0_BASE           (ARM_PERIIOBASE + 0x007000)
#define DMA_CH_BASE(ch)     (DMA0_BASE + (ch) * 0x100)
#define DMA_CS(ch)          (DMA_CH_BASE(ch) + 0x00)
#define DMA_CONBLK_AD(ch)   (DMA_CH_BASE(ch) + 0x04)
#define DMA_DEBUG(ch)       (DMA_CH_BASE(ch) + 0x20)
#define DMA_INT_STATUS      (DMA0_BASE + 0xFE0)
#define DMA_ENABLE_REG      (DMA0_BASE + 0xFF0)

/* DMA CS bits */
#define DMA_CS_ACTIVE           (1 << 0)
#define DMA_CS_END              (1 << 1)
#define DMA_CS_INT              (1 << 2)
#define DMA_CS_WAIT_FOR_WRITES  (1 << 28)
#define DMA_CS_PANIC_PRI(x)     (((x) & 0xF) << 20)
#define DMA_CS_PRI(x)           (((x) & 0xF) << 16)
#define DMA_CS_ABORT            (1 << 30)
#define DMA_CS_RESET            (1 << 31)

/* DMA TI (Transfer Information) bits */
#define DMA_TI_INTEN            (1 << 0)
#define DMA_TI_TDMODE           (1 << 1)
#define DMA_TI_WAIT_RESP        (1 << 3)
#define DMA_TI_DEST_INC         (1 << 4)
#define DMA_TI_DEST_WIDTH       (1 << 5)
#define DMA_TI_DEST_DREQ        (1 << 6)
#define DMA_TI_SRC_INC          (1 << 8)
#define DMA_TI_SRC_WIDTH        (1 << 9)
#define DMA_TI_SRC_DREQ         (1 << 10)
#define DMA_TI_BURST_LENGTH(x)  (((x) & 0xF) << 12)
#define DMA_TI_PERMAP(x)        (((x) & 0x1F) << 16)
#define DMA_TI_NO_WIDE_BURSTS   (1 << 26)

/*
 * The PL011 UART, as offsets from the peripheral base.
 *
 * Offsets rather than addresses: arch/arm-native and Emu68 both write
 * PL011_0_BASE as (ARM_PERIIOBASE + 0x201000) because they have the base as a
 * compile-time constant. This port does not -- it is discovered from the device
 * tree and reaches drivers through KrnGetSystemAttr(KATTR_PeripheralBase) --
 * so the base is a runtime value and only the layout belongs in a header.
 *
 * Carried here because the same map was already written out three times: in
 * Emu68's include/support_rpi.h, in arm-native's own header, and privately in
 * this port's Bluetooth driver.
 */
#define PL011_OFFSET 0x00201000UL

#define PL011_OFFSET 0x00201000UL
#define PL011_DR   0x00
#define PL011_FR   0x18
#define PL011_IBRD 0x24
#define PL011_FBRD 0x28
#define PL011_LCRH 0x2c
#define PL011_CR   0x30
#define PL011_IMSC 0x38
#define PL011_ICR  0x44
#define PL011_IFLS 0x34
#define PL011_MIS  0x40
#define PL011_FR_BUSY (1UL << 3)
#define PL011_FR_RXFE (1UL << 4)
/* Per-byte receive status, in the upper bits of the data register. OE means
 * the FIFO overflowed and bytes were lost before this one -- the H4 framer
 * downstream then reads payload as a packet type and invents packets, so this
 * is worth reporting rather than masking away. */
#define PL011_DR_OE   (1UL << 11)
#define PL011_DR_ERR  (0xfUL << 8)
#define PL011_FR_TXFF (1UL << 5)
/* Receive and receive-timeout interrupts. RTIM is what delivers the tail of a
 * burst: RXIM alone only fires at the watermark, so the last few bytes of a
 * packet would sit in the FIFO until the next one arrived. */
#define PL011_INT_RX  (1UL << 4)
#define PL011_INT_RT  (1UL << 6)
#define PL011_INT_OE  (1UL << 10)
/* Receive watermark at 1/8 of the FIFO -- two bytes -- so the handler is
 * entered early and the FIFO is never close to full when it runs. */
#define PL011_IFLS_RX18 (0UL << 3)
#define PL011_LCRH_FEN   (1UL << 4)
#define PL011_LCRH_WLEN8 (3UL << 5)
#define PL011_CR_UARTEN (1UL << 0)
#define PL011_CR_TXE    (1UL << 8)
#define PL011_CR_RXE    (1UL << 9)
#define PL011_CR_RTSEN  (1UL << 14)
#define PL011_CR_CTSEN  (1UL << 15)
#define PL011_WAIT_LIMIT 1000000UL


#endif /* HARDWARE_BCM2708_H */
