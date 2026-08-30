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
/*
 * Microframes to wait before going back for a split's result.
 *
 * An interrupt endpoint's split runs on a microframe pipeline -- start-split
 * in one, complete-split in the next -- so it waits exactly one. Bulk re-arms
 * immediately. Control waits a full inter-transaction pace.
 *
 * The sixteen is not arrived at from the specification, and it is kept
 * because it has already been ruled out rather than because it is understood:
 * ../usb2otg carries the same value and records that a tester's low-speed
 * keyboard stormed identically at eight and at sixteen, which is what
 * disproved pacing as the cause there. Diverging from a number that has been
 * tested on this hardware needs a better reason than symmetry with bulk.
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
    UBYTE                   xact_errors;    /* attempts spent on bus errors */
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
    /* Every interrupt-IN completion, not just the logged ones: the heartbeat
     * above counts from this, so a silent pipe is distinguishable from a
     * budget that ran out. */
    ULONG                   interrupt_log_seen[128];
    UBYTE                   periodic_log_count;
    UBYTE                   sof_log_count;
    /*
     * Trace budget, one per device address rather than one for the unit.
     *
     * A single counter is spent by whatever talks first, which on this board
     * is the root hub and then the hub behind it -- so the device that
     * actually fails is the one whose transfers were never printed. That has
     * now happened twice, at 32 lines and again at 160. A budget per address
     * cannot be exhausted on somebody else's behalf: a device enumerating
     * late arrives with its own.
     */
    UBYTE                   transfer_log[128];
    /*
     * ...and one account per address for everything that is not endpoint 0.
     *
     * Enumeration is dozens of control transfers on endpoint 0, which spends
     * the whole budget before the device is used for anything. The trace then
     * goes silent exactly when the interrupt endpoint starts, and a silence
     * that begins there reads like a device that stopped talking. It was read
     * that way twice. Endpoint 0 can no longer spend what the data endpoints
     * need.
     */
    UBYTE                   transfer_log_ep[128];
    UBYTE                   transfer_log_count;
    UBYTE                   watchdog_log_count;
    /* Channels waiting out a paced complete-split. Written by the unit
     * task, read by the interrupt top half to decide whether the SOF tick is
     * needed this frame. */
    volatile ULONG          split_pacing;
    ULONG                   split_starts;
    ULONG                   watchdog_recoveries; /* uncapped: see the watchdog */
    ULONG                   watchdog_reported;
    ULONG                   watchdog_heartbeat_ticks;
    ULONG                   watchdog_heartbeats;
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
/* Brings up the VideoCore-owned USB power domain. FALSE only when the
 * firmware answered and the answer was bad; silence is reported and allowed. */
BOOL dwc2_platform_power_on(struct DWC2Device *device);
void dwc2_platform_cpu0_mask(struct DWC2Unit *unit);
void dwc2_platform_log_clocks(struct DWC2Device *device);
BOOL dwc2_controller_start(struct DWC2Unit *unit);
/* Publishes the frame interval for the speed the root port has negotiated. */
void dwc2_controller_speed(struct DWC2Unit *unit);
void dwc2_controller_drain_irq(struct DWC2Unit *unit);
BOOL dwc2_delay_us(struct DWC2Unit *unit, ULONG microseconds);
BYTE dwc2_root_control(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq);
BOOL dwc2_root_interrupt(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq);
void dwc2_root_poll(struct DWC2Unit *unit);
BOOL dwc2_transfer_submit(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq);
void dwc2_transfer_irq(struct DWC2Unit *unit);
void dwc2_transfer_sof(struct DWC2Unit *unit);
/* Both run in the interrupt top half, where the translator's deadline can
 * still be met. dwc2_transfer_split_irq() returns TRUE when it has taken the
 * channel's interrupt entirely and the unit task needs no part of it. */
BOOL dwc2_transfer_split_irq(struct DWC2Unit *unit, UBYTE index, ULONG status);
void dwc2_transfer_split_sof(struct DWC2Unit *unit);
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
