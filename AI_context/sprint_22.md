// AI_context/sprint_22.md

# Sprint 22 — CIA/Paula/Input completion pass

## Description

This sprint focused on making the current Bellatrix CIA and Paula
implementations materially more complete in the areas that affect real input,
interrupt delivery, timer behavior, and the SDL harness path.

The main constraints behind the work were:

- stop organizing CIA around `cia_a.c` / `cia_b.c`
- finish the refactor into behavior-oriented modules
- close the most important missing 8520 behavior before deeper cleanup
- stop scattering controller-port behavior between `machine`, `input`, and
  `Paula`

## CIA changes

Files:

- `src/chipset/cia/cia.c`
- `src/chipset/cia/cia.h`
- `src/chipset/cia/cia_interrupt.c`
- `src/chipset/cia/cia_interrupt.h`
- `src/chipset/cia/cia_ports.c`
- `src/chipset/cia/cia_ports.h`
- `src/chipset/cia/cia_serial.c`
- `src/chipset/cia/cia_serial.h`
- `src/chipset/cia/cia_timers.c`
- `src/chipset/cia/cia_timers.h`
- `src/chipset/cia/cia_tod.c`
- removed: `src/chipset/cia/cia_a.c`
- removed: `src/chipset/cia/cia_b.c`

What changed:

1. CIA internals were split by responsibility:
   - interrupts
   - ports / external lines
   - timers
   - serial
   - TOD
2. Timer A and Timer B gained the essential non-Phi2 modes:
   - CNT clocking
   - Timer B counting Timer A underflow
   - Timer B counting Timer A underflow while CNT is high
3. `PBON` / `OUTMODE` now drive `PB6` / `PB7` through the real timer path
   instead of existing only as control-register bits.
4. `FLG` is now modeled as a falling-edge external input source.
5. The CIA serial path now has real internal shift state for:
   - input byte reception
   - output shifting from `SDR`
   - `SP` / `CNT` line ownership
6. TOD clock writes now stop the clock until the low byte write resumes it,
   matching the essential 8520 behavior needed by software that programs TOD
   explicitly.
7. A later follow-up fixed an input regression where keyboard reception could
   stall after the first handshake if `SPMODE` remained set.

## Keyboard and controller-port changes

Files:

- `src/input/keyboard.c`
- `src/input/keyboard.h`
- `src/input/controller_port.c`
- `src/input/controller_port.h`
- removed: `src/input/mouse.c`
- removed: `src/input/mouse.h`
- `src/core/machine.c`
- `src/core/machine.h`
- `src/host/pal.h`
- `src/host/posix/pal_posix.c`
- `src/host/raspi3/pal_core.c`
- `tools/harness/main.c`

What changed:

1. The host keyboard no longer tries to emulate the entire wire protocol bit by
   bit from `src/input`.
2. `bellatrix_keyboard_step()` now delivers a queued byte when the CIA is ready
   and then waits for the CIA-side handshake before releasing the next one.
3. A new controller-port layer was introduced so the machine owns resolved
   Amiga port semantics instead of raw per-device hacks.
4. This layer now maps:
   - left mouse button -> `CIAAPRA`
   - middle / right mouse buttons -> `POTGOR` / `POTxDAT`
   - mouse motion -> `JOY0DAT` / `JOY1DAT`
   - joystick directional state -> `JOYxDAT`
5. The SDL harness now feeds mouse delta and three buttons through the new
   machine/controller-port API.

## Paula changes

Files:

- `src/chipset/paula/paula.c`
- `src/chipset/paula/paula.h`
- `src/chipset/paula/paula_disk.c`
- `src/chipset/paula/paula_disk.h`
- `src/chipset/paula/paula_input.c`
- `src/chipset/paula/paula_input.h`
- `src/chipset/paula/paula_serial.c`

What changed:

1. Paula now exposes the controller-port registers that were still missing from
   the live decode:
   - `JOY0DAT`
   - `JOY1DAT`
   - `POT0DAT`
   - `POT1DAT`
   - `DSKDATR`
2. The Paula input block was rewritten to consume resolved controller-port
   signals:
   - `joydat`
   - `pot_button_x`
   - `pot_button_y`
3. The serial block no longer starts in "instant TX" mode by default.
   Transmission now advances through `paula_step()`, while instant mode remains
   available for focused tests or bridge-style usage.
4. Disk read DMA now latches the current fetched word into `DSKDATR`, making
   the data path visible through the register surface instead of only through
   `DSKBYTR`.

## Build-system and validation changes

Files:

- `tools/harness/CMakeLists.txt`
- `emu68/CMakeLists.txt`
- `tests/unit/test_cia_core.c`
- `tests/unit/test_uart_null_modem.c`
- `tests/integration/test_machine_overlay.c`

What changed:

1. The harness source list now builds the modular CIA files and the new
   controller-port module.
2. A dedicated CIA unit target was added to the harness test build.
3. The main `Emu68.elf` build was updated to include
   `src/input/controller_port.c` after `machine.c` started depending on that
   module.
4. New unit coverage was added for:
   - CIA timer CNT modes
   - Timer B underflow+CNT behavior
   - `FLG`
   - TOD stop/resume behavior
   - `PBON` / `OUTMODE`
   - CIA serial receive/transmit flow
5. Existing Paula serial tests were updated for the non-instant default.
6. The integration overlay test gained coverage for:
   - CIA serial IRQ generation through the new timer-backed path
   - mouse/controller-port wiring
   - joystick/controller-port wiring
   - `POT0DAT` / `POT1DAT`
   - `POTGOR`
   - `JOY0DAT` / `JOY1DAT`
   - `DSKDATR`

## Validation

Validated locally with:

- `cmake --build out/harness`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_unit_cia`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_unit_uart`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_integration_overlay`

The harness tests passed after the CIA/Paula/input refactor and again after the
keyboard handshake fix.
