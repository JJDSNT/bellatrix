/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * Bellatrix-owned DWC2 host-controller driver for AROS m68k-emu68.
 */

#ifndef DWC2EMU68_INTERN_H
#define DWC2EMU68_INTERN_H

#include LC_LIBDEFS_FILE

#include <aros/libcall.h>
#include <aros/symbolsets.h>
#include <aros/types/spinlock_s.h>

#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/types.h>

#include <devices/usbhardware.h>
#include <devices/newstyle.h>
#include <devices/timer.h>
#include <utility/tagitem.h>

struct DWC2Device;

/*
 * The DWC2 has a fixed set of host channels -- eight on the BCM2835 family --
 * and a channel is the unit of concurrency: one transaction in flight each,
 * with its own endpoint context, its own split state and its own interrupt
 * bit in HAINT. A host that drives one channel can hold one transaction open
 * at a time, which is enough to enumerate a single device and not enough to
 * poll a keyboard and a mouse behind a hub while an Ethernet endpoint runs.
 *
 * Everything a channel is doing lives here rather than on the unit, so that
 * "which transfer is this interrupt about" is answered by the channel index
 * and never by ambient state. The unit task is the only writer; the hardware
 * ISR only records which channels raised an interrupt and what their HCINT
 * said, and wakes the task.
 */
#define DWC2_MAX_CHANNELS 8

/*
 * Split phases. A transaction to a low-/full-speed device behind a
 * high-speed hub is two hardware transactions: the start-split that hands
 * the work to the hub's transaction translator, and the complete-split that
 * collects the result. NONE means the device is high-speed, or is the root
 * port itself, and the transaction runs whole.
 */
#define DWC2_SPLIT_NONE     0
#define DWC2_SPLIT_START    1
#define DWC2_SPLIT_COMPLETE 2

/*
 * How many complete-splits a translator is given before we call it wedged.
 * NYET means "not finished yet" and is normal; the budget exists so a hub
 * that never finishes cannot hold a channel open indefinitely. Eight
 * microframes is one full-speed frame, which is the longest a translator may
 * legitimately take.
 */
#define DWC2_SPLIT_NYET_LIMIT 8

/*
 * How long to wait before collecting a split result, in microframes.
 *
 * Issuing the complete-split in the same microframe as the start-split asks
 * the translator for an answer it cannot have yet, and hammers the same
 * microframe rather than landing in the window where the result appears.
 *
 * The two values are ../usb2otg's, bisected on hardware. Interrupt gets one
 * microframe -- enough to land in the result window. Control gets sixteen
 * (2 ms), which is generous and affordable because split control traffic is
 * enumeration and setup only. Bulk is re-armed immediately: it has no
 * deadline to miss and the throughput matters.
 */
#define DWC2_SPLIT_PACE_INT     1
#define DWC2_SPLIT_PACE_CTRL    16

struct DWC2Channel
{
    struct IOUsbHWReq      *request;        /* NULL when the channel is free */
    UBYTE                   index;          /* hardware channel number */
    UBYTE                   stage;          /* DWC2_STAGE_* */
    UBYTE                   split;          /* DWC2_SPLIT_* phase */
    UBYTE                   split_retries;  /* complete-splits spent waiting */
    UBYTE                   split_delay;    /* microframes before the CSPLIT */
    UBYTE                   retries;
    ULONG                   armed_length;   /* bytes programmed this transaction */
    UBYTE                  *buffer;         /* bounce buffer, cache-line aligned */
    APTR                    buffer_raw;     /* what to free */
    ULONG                   buffer_size;
    ULONG                   watchdog_ticks;
    volatile ULONG          pending;        /* HCINT bits taken by the ISR */
};

struct DWC2Unit
{
    struct Unit             unit;
    struct DWC2Device      *device;
    struct Task            *task;
    struct MsgPort         *port;
    struct MsgPort         *timer_port;
    struct timerequest     *timer_request;
    BOOL                    watchdog_active;
    cpumask_t               affinity;
    APTR                    irq_handle;
    struct Interrupt        soft_irq;
    volatile ULONG          irq_pending;
    volatile ULONG          channels_pending;   /* bitmap of channels, not HCINT bits */
    volatile ULONG          periodic_due;
    volatile BOOL           periodic_waiting;
    ULONG                   saved_gusbcfg;
    UBYTE                   host_channels;
    BOOL                    initialized;
    BOOL                    hardware_ok;
    BOOL                    port_changed;
    UBYTE                   hub_address;
    struct IOUsbHWReq      *hub_interrupt;
    struct DWC2Channel      channel[DWC2_MAX_CHANNELS];
    struct List             transfer_queue;
    struct List             periodic_queue;
    ULONG                   dma_length;         /* bounce buffer size per channel */
    ULONG                   data_toggle[128];
    UBYTE                   interrupt_log_count[128];
    UBYTE                   periodic_log_count;
    UBYTE                   sof_log_count;
    UBYTE                   transfer_log_count;
    UBYTE                   watchdog_log_count;
    ULONG                   watchdog_recoveries; /* uncapped: see the watchdog */
    ULONG                   watchdog_reported;
    BOOL                    opened;
};

struct DWC2Device
{
    struct Device           device;
    struct DWC2Unit        *unit;
    IPTR                    peripheral_base;
    IPTR                    register_base;
    APTR                    kernel_base;
    APTR                    mbox_base;
    struct Library         *utility_base;
};

struct DWC2NSQueryResult
{
    ULONG                   DevQueryFormat;
    ULONG                   SizeAvailable;
    UWORD                   DeviceType;
    UWORD                   DeviceSubType;
    const UWORD            *SupportedCommands;
};

#define DWC2_FNAME(x) DWC2__Dev__ ## x

void DWC2_FNAME(UnitTask)(struct DWC2Unit *unit);
AROS_INTP(DWC2_FNAME(SoftIRQ));
BOOL dwc2_platform_probe(struct DWC2Device *device);
void dwc2_platform_cpu0_mask(struct DWC2Unit *unit);
void dwc2_platform_log_clocks(struct DWC2Device *device);
BOOL dwc2_controller_start(struct DWC2Unit *unit);
void dwc2_controller_drain_irq(struct DWC2Unit *unit);
BOOL dwc2_delay_us(struct DWC2Unit *unit, ULONG microseconds);
BYTE dwc2_root_control(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq);
BOOL dwc2_root_interrupt(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq);
void dwc2_root_poll(struct DWC2Unit *unit);
BOOL dwc2_transfer_submit(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq);
void dwc2_transfer_irq(struct DWC2Unit *unit);
void dwc2_transfer_sof(struct DWC2Unit *unit);
void dwc2_transfer_service(struct DWC2Unit *unit);
void dwc2_transfer_watchdog(struct DWC2Unit *unit);
void dwc2_transfer_abort_all(struct DWC2Unit *unit);
BOOL dwc2_transfer_idle(const struct DWC2Unit *unit);
BOOL dwc2_transfer_pending_abort(const struct DWC2Unit *unit);
void dwc2_watchdog_arm(struct DWC2Unit *unit);
void dwc2_watchdog_cancel(struct DWC2Unit *unit);
ULONG dwc2_readl(const struct DWC2Device *device, ULONG offset);
void dwc2_writel(const struct DWC2Device *device, ULONG offset, ULONG value);

#endif
