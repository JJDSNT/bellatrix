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

#define ARMIRQ_BASE   (2 << 5)

/*
 * Register macros, for the one case where the note above does not apply.
 *
 * The peripheral base is discovered at runtime here, so a header that expands
 * addresses against a compile-time ARM_PERIIOBASE is normally useless to this
 * port. arch/arm-native's dma.resource shows the way around it: dma_private.h
 * defines ARM_PERIIOBASE as a *runtime expression* -- `DMABase->dma_periiobase`
 * -- immediately before including this header, so the macros expand against
 * whatever the including file decided the base is.
 *
 * These are therefore behind the definition rather than in front of it. A
 * translation unit that has not said where the peripherals are does not get
 * addresses, and the failure is a compile error naming ARM_PERIIOBASE rather
 * than a driver quietly poking address 0x7000.
 *
 * Copied verbatim from the same arm-native header as the interrupt map above,
 * for the same reason: one origin for the numbers.
 */
#ifdef ARM_PERIIOBASE

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

#endif /* ARM_PERIIOBASE */

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
