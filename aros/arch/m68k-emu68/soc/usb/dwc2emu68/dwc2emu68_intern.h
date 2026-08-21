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
    volatile ULONG          channel_pending;
    volatile ULONG          periodic_due;
    volatile BOOL           periodic_waiting;
    ULONG                   saved_gusbcfg;
    UBYTE                   host_channels;
    BOOL                    initialized;
    BOOL                    hardware_ok;
    BOOL                    port_changed;
    UBYTE                   hub_address;
    struct IOUsbHWReq      *hub_interrupt;
    struct IOUsbHWReq      *active_request;
    struct List             transfer_queue;
    struct List             periodic_queue;
    APTR                    dma_raw;
    UBYTE                  *dma_buffer;
    ULONG                   dma_length;
    ULONG                   active_length;
    UBYTE                   transfer_stage;
    UBYTE                   retry_count;
    ULONG                   data_toggle[128];
    UBYTE                   interrupt_log_count[128];
    UBYTE                   periodic_log_count;
    UBYTE                   sof_log_count;
    UBYTE                   transfer_log_count;
    BOOL                    opened;
};

struct DWC2Device
{
    struct Device           device;
    struct DWC2Unit        *unit;
    IPTR                    peripheral_base;
    IPTR                    register_base;
    APTR                    kernel_base;
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
void dwc2_transfer_abort_active(struct DWC2Unit *unit);
void dwc2_watchdog_arm(struct DWC2Unit *unit);
void dwc2_watchdog_cancel(struct DWC2Unit *unit);
ULONG dwc2_readl(const struct DWC2Device *device, ULONG offset);
void dwc2_writel(const struct DWC2Device *device, ULONG offset, ULONG value);

#endif
