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
#define BELLATRIX_USB_OWNER_NONE UINT32_MAX

static USBHost *g_bellatrix_usb_host;

#ifdef BELLATRIX_USB_LOG
#define BELLATRIX_USB_LOGF(...) kprintf(__VA_ARGS__)
#else
#define BELLATRIX_USB_LOGF(...) do { } while (0)
#endif

static uint32_t bellatrix_usb_current_core(void)
{
#if defined(__aarch64__)
    uint64_t mpidr;
    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 3u);
#else
    return 0u;
#endif
}

static bool bellatrix_usb_owner_enter(USBHost *host, const char *operation)
{
    uint32_t core = bellatrix_usb_current_core();
    uint32_t expected = 0u;

    if (__atomic_load_n(&host->owner_active, __ATOMIC_ACQUIRE) == core + 1u) {
        __atomic_add_fetch(&host->owner_depth[core], 1u, __ATOMIC_RELAXED);
        return true;
    }

    if (!__atomic_compare_exchange_n(&host->owner_active, &expected, core + 1u,
                                     false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
        uint32_t active_core = expected - 1u;
        uint32_t violations = __atomic_add_fetch(&host->ownership_violations, 1u,
                                                 __ATOMIC_RELAXED);
        if (violations == 1u) {
            kprintf("[USB-OWNER] CONTENTION requester=core%u active=core%u op=%s; serializing\n",
                    (unsigned)core, (unsigned)active_core, operation);
        }

        do {
            expected = 0u;
#if defined(__aarch64__)
            __asm__ volatile("yield");
#endif
        } while (!__atomic_compare_exchange_n(&host->owner_active, &expected,
                                               core + 1u, false,
                                               __ATOMIC_ACQUIRE,
                                               __ATOMIC_RELAXED));
    }

    __atomic_store_n(&host->owner_depth[core], 1u, __ATOMIC_RELAXED);
    uint32_t previous = __atomic_exchange_n(&host->last_owner_core, core,
                                            __ATOMIC_RELAXED);
    __atomic_fetch_add(&host->calls_by_core[core], 1u, __ATOMIC_RELAXED);
#ifndef BELLATRIX_USB_LOG
    (void)previous;
#endif
    if (previous != core) {
        if (previous == BELLATRIX_USB_OWNER_NONE) {
            BELLATRIX_USB_LOGF("[USB-OWNER] core=%u op=%s previous=none\n",
                               (unsigned)core, operation);
        } else {
            BELLATRIX_USB_LOGF("[USB-OWNER] core=%u op=%s previous=core%u\n",
                               (unsigned)core, operation, (unsigned)previous);
        }
    }
    return true;
}

static void bellatrix_usb_owner_leave(USBHost *host)
{
    uint32_t core = bellatrix_usb_current_core();
    uint32_t depth = __atomic_load_n(&host->owner_depth[core], __ATOMIC_RELAXED);

    if (depth > 1u) {
        __atomic_store_n(&host->owner_depth[core], depth - 1u, __ATOMIC_RELAXED);
        return;
    }

    __atomic_store_n(&host->owner_depth[core], 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&host->owner_active, 0u, __ATOMIC_RELEASE);
}

bool usb_host_stack_enter(const char *operation)
{
    USBHost *host = __atomic_load_n(&g_bellatrix_usb_host, __ATOMIC_ACQUIRE);
    return host && bellatrix_usb_owner_enter(host, operation);
}

void usb_host_stack_leave(void)
{
    USBHost *host = __atomic_load_n(&g_bellatrix_usb_host, __ATOMIC_ACQUIRE);
    if (host)
        bellatrix_usb_owner_leave(host);
}

static void usbh_bellatrix_poll(uint8_t busid)
{
    USBH_IRQHandler(busid);
}

static void bellatrix_usb_pump_events(unsigned int passes)
{
    /* ISSUE-0036: this function runs continuously from usb_host_step() during
     * normal operation, not just once at boot -- a per-pass diag print here
     * floods the mini-UART forever (confirmed on hardware: ring buffer
     * overwritten faster than it drains, producing endless truncated
     * fragments). Keep this function print-free. */
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

    host->owner_active = 0u;
    host->last_owner_core = BELLATRIX_USB_OWNER_NONE;
    host->ownership_violations = 0u;
    for (unsigned int i = 0u; i < 4u; ++i)
        host->calls_by_core[i] = 0u;
    for (unsigned int i = 0u; i < 4u; ++i)
        host->owner_depth[i] = 0u;
    __atomic_store_n(&g_bellatrix_usb_host, host, __ATOMIC_RELEASE);

    if (!bellatrix_usb_owner_enter(host, "init")) {
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

    bellatrix_usb_owner_leave(host);
    return true;
}

void usb_host_step(USBHost *host)
{
    if (!host || !host->enabled || !host->initialized) {
        return;
    }

    if (!bellatrix_usb_owner_enter(host, "step")) {
        return;
    }

    host->poll_count++;

    if (host->controller_ready) {
        bellatrix_usb_pump_events(2u);
        if (host->poll_count <= 16u || (host->poll_count % 4096u) == 0u) {
            bellatrix_usb_log_hprt(host, "poll");
        }
    }

    bellatrix_usb_owner_leave(host);
}

void usb_host_shutdown(USBHost *host)
{
    if (!host || !host->initialized) {
        return;
    }

    if (!bellatrix_usb_owner_enter(host, "shutdown")) {
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
    BELLATRIX_USB_LOGF("[USB-OWNER] calls core0=%u core1=%u core2=%u core3=%u violations=%u\n",
                       (unsigned)host->calls_by_core[0],
                       (unsigned)host->calls_by_core[1],
                       (unsigned)host->calls_by_core[2],
                       (unsigned)host->calls_by_core[3],
                       (unsigned)host->ownership_violations);
    __atomic_store_n(&g_bellatrix_usb_host, NULL, __ATOMIC_RELEASE);
    bellatrix_usb_owner_leave(host);
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
    host->owner_active = 0u;
    host->last_owner_core = UINT32_MAX;
    host->ownership_violations = 0u;
    for (unsigned int i = 0u; i < 4u; ++i)
        host->calls_by_core[i] = 0u;
    for (unsigned int i = 0u; i < 4u; ++i)
        host->owner_depth[i] = 0u;
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

bool usb_host_stack_enter(const char *operation)
{
    (void)operation;
    return false;
}

void usb_host_stack_leave(void)
{
}

#endif
