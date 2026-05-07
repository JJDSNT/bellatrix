// AI_context/sprint_20.md

# Sprint 20 — bare-metal serial bridge fixed, DiagROM 1.3/2.0 status

## Description

This sprint focused on the bare-metal serial path on Raspberry Pi 3 and on
getting meaningful progress from DiagROM over the Bellatrix -> Paula -> host
UART bridge.

The main goals were:

- confirm whether Paula TX was working at all
- confirm whether the host bridge was actually draining Paula TX
- fix any bridge/backend bug that prevented bytes from reaching the physical
  UART
- compare DiagROM 1.3 and DiagROM 2.0 behavior after the bridge started
  working
- experiment with software loopback support for the older DiagROM line

## Issue 1 — bare-metal serial transport was not draining Paula TX

### Root cause

The critical bug was in `src/io/serial/uart_host.c`.

`bellatrix_machine_init()` correctly attached:

- `m->uart_host.paula_serial = &m->paula.serial`

But later, during host UART open:

- `uart_host_open_pl011()` called `uart_host_shutdown(host)`
- `uart_host_shutdown()` cleared `host->paula_serial = NULL`

From that point on, `uart_host_poll()` always returned early because:

```c
if (!host || !host->enabled || !host->paula_serial) {
    return;
}
```

That explained the previous symptom perfectly:

- `[SERIAL-TX] first SERDAT=...` appeared
- but no bridge log ever appeared
- and nothing reached the real UART

### What was implemented

Preserved the Paula attachment across backend reopen in:

- `src/io/serial/uart_host.c`

Specifically:

- `uart_host_open_pl011()` now saves/restores `host->paula_serial`
- `uart_host_open_miniuart()` now saves/restores `host->paula_serial`

### Result

Bare metal now confirms the full path:

- Paula TX write appears
- bridge drains Paula TX
- PL011 backend write runs
- text reaches the external serial terminal

Representative successful log sequence:

```text
[SERIAL-TX] first SERDAT=0149 byte=49
[SERIAL-BRIDGE] draining Paula TX byte=49
[PL011-WRITE] first byte=49 CR=00000301 FR=00000019
[SERIAL-BRIDGE] first PL011 backend send byte=49 open=1 enabled=1 ok=1
[SERIAL-BRIDGE] first PL011 TX byte=49
```

### Current status

Delivered and confirmed on real hardware.

This is the main breakthrough of the sprint.

## Issue 2 — additional serial bridge / backend fixes and instrumentation

### What was implemented

Several supporting fixes were added while narrowing down the failure:

1. **TX byte loss fix**

Before:

- `uart_host_poll()` popped the TX byte before trying to send it

After:

- `uart_host_poll()` peeks first
- sends to backend
- only pops after successful send

Files:

- `src/chipset/paula/paula_serial.h`
- `src/chipset/paula/paula_serial.c`
- `src/io/serial/uart_host.c`

2. **PL011/MMIO alias fix**

The Raspi backends must use Emu68's peripheral virtual alias, not raw physical
MMIO addresses.

Files:

- `src/host/raspi3/pl011_backend.c`
- `src/host/raspi3/miniuart_backend.c`

3. **PL011 first-byte forward-progress simplification**

For the bridge path, `pl011_backend_write_byte()` was changed to write directly
to `DR` instead of depending on the previous `TXFF` gate, which appeared to
wedge the first byte on bare metal.

File:

- `src/host/raspi3/pl011_backend.c`

4. **Instrumentation**

Added targeted one-shot logs for:

- first `SERPER`
- first `SERDATR`
- first `SERDAT`
- first bridge drain
- first PL011 backend send
- first PL011 write

Files:

- `src/chipset/paula/paula.c`
- `src/io/serial/uart_host.c`
- `src/host/raspi3/pl011_backend.c`
- `emu68/src/aarch64/vectors.c`

### Current status

Most of these logs are still useful for bring-up, but they are now temporary
instrumentation and should eventually be reduced or removed.

## Issue 3 — DiagROM 2.0 now advances meaningfully over bare-metal serial

### Observed behavior

With `src/roms/diagrom2.rom`, the serial output on bare metal is no longer
dead. It advances through several early checks and prints recognizable
diagnostic text, including:

- ROM/version banner
- mouse button prompt
- OVL test
- chipmem scan
- stack setup
- start of allocated RAM use

Representative excerpts observed on target:

```text
$VER: DiagROM Amiga Diagnostic by John Hertell. V2.0 2026-03-20
Checking addressdata of ROM-Space
Release mousebuttons now...
Checking if OVL works: FAILED
Scan for usable Chipmem
Stack starts at: $0007F8C4
Setting stack to: $000838C4
Starting to use allocated RAM now
```

### Interpretation

This means:

- serial transport is alive
- the ROM is running far enough to exercise multiple subsystems
- the current blockers are no longer “no serial output” but correctness and
  fidelity issues in emulation

### Current status

DiagROM 2.0 is currently the better ROM for continued Bellatrix bring-up.

## Issue 4 — DiagROM 1.3 still preferred by user, but loopback handling is not solved yet

### Local ROM/source inventory

The workspace contains:

- `src/roms/diagrom.rom` -> embedded string reports `V1.3`
- `src/roms/diagrom2.rom` -> embedded string reports `V2.0 2026-03-20`
- `diagrom.s` in repo root -> clearly from the older DiagROM family and
  contains explicit serial loopback code / strings

Notable strings and code in `diagrom.s`:

- `Testing if serial loopbackadapter is installed`
- `RealLoopbacktest`
- `Loopbacktest`

### Loopback experiments

The project already had a `NULL_MODEM_LOOPBACK` mode, but originally it was not
a real internal loopback.

This sprint added:

1. **full internal loopback**

- echoes TX back into RX through a local FIFO
- still mirrors bytes to host serial so the external console remains visible

2. **probe/oneshot loopback**

- intended to echo only a small initial window and then fall back to normal
  mode

Files:

- `src/io/serial/null_modem.h`
- `src/io/serial/null_modem.c`
- `scripts/build.sh`
- `src/cpu/bellatrix.c`

### Outcome

The loopback experiments were not sufficient yet:

- full loopback makes DiagROM 1.3 see its own console stream too aggressively
  and pollutes the session
- probe/oneshot loopback still did **not** make DiagROM 1.3 report
  `loopback adapter detected`
- the serial stream is active, but timing/selectivity are still wrong for the
  exact pattern DiagROM 1.3 expects

Observed target result with probe loopback:

```text
Testing if serial loopbackadapter <> NOT DETECTED
```

### Current status

Not solved.

The remaining work is no longer “how to echo bytes at all”, but “how to echo
the right subset at the right time without feeding the whole console back into
RX”.

## Issue 5 — current interpretation of remaining problems

### What is now known good

- bare-metal Bellatrix image boots
- PL011 host bridge opens
- Paula serial TX reaches the bridge
- bridge reaches PL011
- external serial terminal receives guest output

### What is still wrong/incomplete

1. **serial text quality / fragmentation**

Characters are still partially garbled or interleaved in some boots, especially
with older DiagROM output.

2. **DiagROM 1.3 loopback detection**

The older DiagROM line still does not consider the current software loopback
implementation a valid adapter.

3. **emulation correctness beyond transport**

DiagROM 2.0 still reports:

- `Checking if OVL works: FAILED`

So after the serial transport breakthrough, the next blockers are chipset and
system-behavior correctness, not transport availability.

## Validation

Validation achieved in this sprint:

- project rebuilt successfully through the normal Bellatrix build flow
- bare-metal boot on Raspberry Pi 3 confirmed the fixed serial bridge
- serial output from both DiagROM families was observed on the physical host
  UART

Representative build/deploy command used:

```bash
BELLATRIX_SERIAL=pl011 ./scripts/build.sh clean
```

Loopback experiments used:

```bash
BELLATRIX_SERIAL=pl011 BELLATRIX_SERIAL_LOOPBACK=1 ./scripts/build.sh clean
BELLATRIX_SERIAL=pl011 BELLATRIX_SERIAL_LOOPBACK=probe ./scripts/build.sh clean
```

## Files modified during this sprint

- `src/io/serial/uart_host.c`
- `src/chipset/paula/paula_serial.h`
- `src/chipset/paula/paula_serial.c`
- `src/chipset/paula/paula.c`
- `src/host/raspi3/pl011_backend.c`
- `src/host/raspi3/miniuart_backend.c`
- `src/io/serial/null_modem.h`
- `src/io/serial/null_modem.c`
- `src/cpu/bellatrix.c`
- `scripts/build.sh`
- `emu68/src/aarch64/vectors.c`
- `scripts/setup.sh`

## Next steps

1. Reduce or remove the temporary serial instrumentation once the next phase is
   stable.

2. Focus on DiagROM 2.0 as the primary bring-up ROM, because it currently gives
   the clearest forward progress.

3. Revisit DiagROM 1.3 loopback with a **selective/protocol-aware**
   implementation rather than full echo or fixed-length oneshot echo.

4. Investigate why DiagROM 2.0 reports:

- `Checking if OVL works: FAILED`

5. After OVL and early correctness issues improve, revisit output cleanliness
   and any remaining serial framing/timing anomalies.

## Follow-up — Paula modularization

After the serial work, Paula was moved further toward the split-file layout:

- the legacy `src/chipset/paula/uart.c` / `uart.h` path is now gone from the
  live tree
- the unit test now targets `PaulaSerial` directly instead of the removed
  `UARTState` API
- `Paula` now owns `PaulaInterrupt irq` instead of flat `intena/intreq/ipl`
  fields
- `paula.c` now delegates IRQ register state and IPL computation to
  `paula_interrupt.c`
- `agnus.c`, `machine.c`, the harness build, and the overlay integration test
  were updated to use `m->paula.irq.{intena,intreq}`
- harness POSIX support gained local `kprintf_get_enabled()` /
  `kprintf_set_enabled()` stubs so `uart_host.c` still links in host-side
  integration tests

Validation for this modularization step:

- `bellatrix_unit_uart` passes
- `bellatrix_integration_overlay` builds and passes

This leaves `paula.c` smaller than before, but it still owns POTGO/POTGOR,
mouse-right handling, lifecycle glue, and top-level register dispatch. Disk,
serial, and interrupts are now on the new split path.

Additional follow-up split:

- `POTGO` / `POTGOR` and mouse-right state moved into
  `src/chipset/paula/paula_input.c` / `paula_input.h`
- `Paula` now owns `PaulaInput input`
- `paula.c` keeps only the top-level register dispatch and delegates input
  behavior to the new module
