/* BCM2837 physical IRQ ownership for Bellatrix.
 *
 * UART0 (GPU IRQ 57) is the sole enabled source. Unknown sources are counted
 * and contained by vectors.c; they are never converted into guest EXTER. */

#include "host/raspi3/physical_interrupts.h"

#include <stdatomic.h>

#include "io/bluetooth/bt_hal_raspi3.h"

#define ARM_PERI_VIRT_BASE 0xF2000000UL
#define ARM_IRQ_ENABLE2    (ARM_PERI_VIRT_BASE + 0xB214UL)
#define ARM_IRQ_DISABLE2   (ARM_PERI_VIRT_BASE + 0xB220UL)
#define ARM_IRQ_UART0      57u
#define ARM_IRQ_UART0_BIT  (1u << (ARM_IRQ_UART0 - 32u))

#if BELLATRIX_ENABLE_BTSTACK
static _Atomic bool s_bt_irq_route_enabled;
#endif
static _Atomic bool s_bt_irq_armed;
static _Atomic uint32_t s_bt_irq_count;
static _Atomic uint32_t s_unknown_irq_count;

static inline void irq_wr32(uintptr_t addr, uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    *(volatile uint32_t *)addr = __builtin_bswap32(value);
#else
    *(volatile uint32_t *)addr = value;
#endif
}

void bellatrix_physical_bt_irq_enable(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    atomic_store_explicit(&s_bt_irq_route_enabled, true, memory_order_release);
    atomic_store_explicit(&s_bt_irq_armed, true, memory_order_release);
    irq_wr32(ARM_IRQ_ENABLE2, ARM_IRQ_UART0_BIT);
    __asm__ volatile("dsb sy\n\tisb\n\tmsr daifclr, #2" ::: "memory");
#endif
}

void bellatrix_physical_bt_irq_disable(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    atomic_store_explicit(&s_bt_irq_route_enabled, false, memory_order_release);
    atomic_store_explicit(&s_bt_irq_armed, false, memory_order_release);
    irq_wr32(ARM_IRQ_DISABLE2, ARM_IRQ_UART0_BIT);
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
#endif
}

void bellatrix_physical_bt_irq_rearm(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    if (!atomic_load_explicit(&s_bt_irq_route_enabled, memory_order_acquire) ||
        atomic_exchange_explicit(&s_bt_irq_armed, true, memory_order_acq_rel))
        return;
    irq_wr32(ARM_IRQ_ENABLE2, ARM_IRQ_UART0_BIT);
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

bool bellatrix_physical_bt_irq_is_armed(void)
{
    return atomic_load_explicit(&s_bt_irq_armed, memory_order_acquire);
}

void __attribute__((target("general-regs-only")))
bellatrix_physical_irq_handler(uint32_t source)
{
    if (source != BELLATRIX_PHYSICAL_IRQ_UART0) {
        atomic_fetch_add_explicit(&s_unknown_irq_count, 1u,
                                  memory_order_relaxed);
        return;
    }

    /* UART RX/RT is level-sensitive. Contain the route before touching FIFO;
     * the host reactor rearms it after consuming the published work. */
    irq_wr32(ARM_IRQ_DISABLE2, ARM_IRQ_UART0_BIT);
    __asm__ volatile("dsb sy" ::: "memory");
    atomic_store_explicit(&s_bt_irq_armed, false, memory_order_release);
    atomic_fetch_add_explicit(&s_bt_irq_count, 1u, memory_order_relaxed);
#if BELLATRIX_ENABLE_BTSTACK
    bt_hal_raspi3_irq_rx();
#endif
}

uint32_t bellatrix_physical_bt_irq_count(void)
{
    return atomic_load_explicit(&s_bt_irq_count, memory_order_relaxed);
}

uint32_t bellatrix_physical_unknown_irq_count(void)
{
    return atomic_load_explicit(&s_unknown_irq_count, memory_order_relaxed);
}
