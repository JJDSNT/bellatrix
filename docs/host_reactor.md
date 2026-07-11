# Bellatrix Host Reactor

## Status

Implemented and validated on Raspberry Pi 3B with the Musashi multicore
runtime, launcher, USB HID, HDMI audio and KS1.3/Battle Squadron.

## Active core topology

| Core | Plane | Responsibility |
| --- | --- | --- |
| 0 | Control | boot, supervision and physical host I/O |
| 1 | CPU | Musashi or Emu68 68k execution |
| 2 | Chipset | exclusive Rigel ownership |
| 3 | Acceleration | parked; reserved for future RTG/AHI jobs |

Core numbers describe ownership, not interrupt priority. Physical ARM IRQs and
emulated Amiga IPL are separate contracts.

## Core 0 ownership

Core 0 is the sole owner of CherryUSB/DWC2, Bluetooth host processing, the
physical UART and deferred console drain. Core 2 owns Paula and exchanges
serial bytes with Core 0 through SPSC queues. Core 0 does not access Rigel
state directly.

The launcher and runtime use the same service point:

```text
launcher (cooperative) ─┐
                       ├─> bellatrix_runtime_io_step() ─> Host Reactor
supervisor (1 kHz) ─────┘
```

`bellatrix_launcher_pump_usb()` remains as a compatibility callback, but no
longer calls `usb_host_step()` directly. There is no USB ownership handoff and
no `launcher_owns_usb` gate.

## Activation and dispatch

The reactor has an atomic pending bitmap for USB, Bluetooth, serial and
console work. Polling currently marks these bits at approximately 1 kHz. A
future IRQ handler will mark the same bits, acknowledge the physical source,
wake Core 0 and return. Driver stacks must not execute in the IRQ handler.

Dispatch currently preserves this order:

```text
Bluetooth → USB → Paula/physical serial bridge → console
```

The 1 ms interval is an observable budget, not yet a preemptive boundary.
Drivers that exceed it complete normally and increment `budget_miss`.

## Measured baseline

Raspberry Pi 3B runtime after launcher:

```text
calls=4000 pending=00 budget_miss=0 avg=7us max=26us late_max=0us
usb=19us bt=0us serial=0us console=7us
```

The average reactor cost is about 0.7% of one core at 1 kHz. The observed
maximum is 2.6% of the 1 ms budget.

Launcher instrumentation exposed synchronous USB work:

```text
calls=16543 budget_miss=2 max=624899us late_max=650434us usb=624893us
```

These stalls happen before CPU and chipset workers launch and are reported
separately as `[CORE0-IO-LAUNCHER]`. Runtime statistics are reset afterward and
reported as `[CORE0-IO]`.

## Single-core behavior

The ownership model is identical in single-core builds. The activation source
changes:

```text
CPU backend → PAL_Runtime_Poll() → Host Reactor
```

Musashi returns cooperatively after execution blocks. Emu68 must preserve the
same bounded-return contract. IRQs can later reduce polling but do not change
the reactor API or ownership.

## Core 3 policy

Core 3 is not a generic I/O core. It is an acceleration plane for measured,
expensive, asynchronous jobs such as RTG conversion or AHI mixing/resampling.
Control, completion ordering and physical device ownership remain on Core 0.
No service moves to Core 3 without measurements and an explicit queue/job
contract.

## Current limitations

- USB/MSC operations are still synchronous and can block for hundreds of
  milliseconds. This is acceptable during launcher boot, but runtime MSC must
  be measured and may require request/completion or a Core 3 worker.
- The budget detects overruns but cannot preempt a driver.
- Polling adds up to approximately 1 ms activation latency.
- HDMI audio progression is currently coupled to chipset advancement rather
  than fully scheduled by the Host Reactor.
- The latest unified reactor requires a dedicated single-core hardware smoke
  before that configuration is promoted to the same validated baseline.

## Invariants

- Exactly one physical owner enters each host driver stack.
- Core 1 observes emulated IPL, never physical device IRQs.
- Core 2 exclusively owns Rigel.
- Core 0 performs bounded, non-blocking work during runtime where possible.
- Heavy computation may move to Core 3; control-plane ownership does not.
- Polling and future IRQ activation must produce equivalent service semantics.
