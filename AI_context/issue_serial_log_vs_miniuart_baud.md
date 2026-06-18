// AI_context/issue_serial_log_vs_miniuart_baud.md

# Issue: Split serial log mode from raw mini-UART mode and revisit baud policy

## Status: open

The current console/logging work clarified two distinct use cases that were
being treated as one:

1. **Debug/log sink**
   - Goal: see early `kprintf` boot logs and useful Paula/DiagROM text in QEMU
     or on a USB-TTL adapter.
   - Text-oriented, lossy under pressure is acceptable.
   - Should be fast enough to avoid drowning boot/runtime diagnostics.
   - A higher physical baud such as 115200 is likely appropriate.

2. **Raw Amiga serial bridge**
   - Goal: expose Paula serial as a real byte stream.
   - Must not have arbitrary Bellatrix logs mixed into the stream if the user
     wants to test serial protocols, binary transfer, or a null-modem style
     connection.
   - Baud semantics need to be explicit. The host wire rate does not have to
     equal Paula's internal SERPER timing if the backend is a byte bridge, but
     the chosen behavior must be documented.

## What was learned

The QEMU failure was not proof that mini-UART logging was fundamentally broken.
It was a wiring issue: QEMU's `raspi3b` exposes PL011 as the first serial and
the AUX mini-UART as the second serial. Using only `-serial stdio` attaches the
terminal to PL011, while Bellatrix had moved the console to mini-UART. The
working QEMU shape is:

```sh
-serial null -serial stdio
```

With that, boot logs and DiagROM output are visible from the mini-UART path.

The current implementation still shares one physical mini-UART between
`kprintf` and Paula in the raw `miniuart` backend. It uses 9600 baud because
Paula's existing host bridge used 9600, and both users currently program the
same physical UART. This is conservative but not necessarily the right long
term UX.

## Desired design direction

Keep the modes conceptually separate:

- `BELLATRIX_SERIAL=log`
  - Debug-first mode.
  - Single console/log sink.
  - Paula TX can be presented as text via `[SERIAL]` lines.
  - Candidate default for the TUI because it makes logs visible and avoids raw
    protocol ambiguity.
  - Candidate physical baud: 115200 or another higher rate.

- `BELLATRIX_SERIAL=miniuart`
  - Raw mini-UART bridge mode.
  - Intended for a real USB/serial debugger or terminal connected to GPIO
    14/15, and for QEMU tests of the actual UART bridge.
  - Should avoid mixing arbitrary `kprintf` bytes into the raw stream, or make
    that mixing explicitly opt-in.
  - Candidate default for non-TUI scripts until the raw/log split is fully
    specified.

## Open decisions

- Should `log` mode own the physical mini-UART at 115200 by default?
- Should `miniuart` mode suppress `kprintf` entirely, or keep the current
  opportunistic drain with Paula priority?
- Should the baud be configured by one shared variable, e.g.
  `BELLATRIX_CONSOLE_BAUD`, or should `log` and `miniuart` have separate baud
  settings?
- If Paula's byte bridge uses a host baud different from Amiga SERPER timing,
  what should the user-facing documentation say?
- For real hardware, should the recommended PuTTY setting depend on selected
  mode (`log` vs `miniuart`)?

## Files to revisit

- `src/host/raspi3/console_log.c`
- `src/cpu/emu68/bellatrix.c`
- `src/io/serial/uart_host.c`
- `src/host/raspi3/miniuart_backend.c`
- `scripts/build.sh`
- `run.sh`
- `tools/launcher/tui.go`

