// AI_context/consolidated/issue_logging_miniuart.md

# Issue: Log output via mini-UART when Bluetooth owns the PL011

## Status: resolved (2026-06-18)

PL011 belongs to Bluetooth, **unconditionally, in every build** — `kprintf`
never touches it, BTSTACK compiled in or not. It shares the mini-UART with
Paula's emulated Amiga serial port via a priority ring buffer: Paula's
traffic always wins, `kprintf` only fills idle gaps. `BELLATRIX_UART_PL011`
(Paula's serial bridged to real PL011 hardware) is removed outright, not
carved out as an exception — it physically could never coexist with this
design (same header pins PL011/ALT0 vs mini-UART/ALT5 need).

### Why the previous BTSTACK-only fix wasn't the final answer

An earlier pass in this session made `kprintf` claim the mini-UART
permanently, but only on `BELLATRIX_ENABLE_BTSTACK` builds, replacing a
fragile runtime handoff (`bt_console_handoff()`/`bt_console_release()` in
`bt_host.c`, called from 4 different points in BT's bootstrap/retry/timeout
state machine — correctness was coupled to that state machine being exactly
right on every path). The reference that broke that fragility was Emu68's
own `PISTORM` variant: it picks a `kprintf` transport once, at the earliest
point in boot, and never switches it.

That fix was then found incomplete: PL011 should be Bluetooth's "whether
it's present or not" (i.e. regardless of whether `BELLATRIX_ENABLE_BTSTACK`
is compiled in for this build) — but builds without BTSTACK already give the
mini-UART to Paula's serial bridge (`uart_host_open_miniuart()`, working
feature, used whenever PTY isn't available — always true on real hardware).
Making `kprintf` claim it unconditionally would have silently broken that.

### The fix: Paula always wins, kprintf is opportunistic

Traced Paula's real TX path: `machine_step_host_serial_rigel()`
(`src/machine/machine_rigel_step.c`) drains Paula's emulated serial TX FIFO
straight to hardware, unconditionally, once per quantum (~6.8ms at PAL) —
the only place Paula bytes ever reach the wire.

`kprintf` never writes mini-UART hardware directly anymore. It pushes into
a small non-blocking ring buffer (`src/host/raspi3/console_log.c`); the
*only* place that ring ever drains to hardware is `console_log_drain()`,
called immediately **after** Paula's existing drain loop in
`machine_step_host_serial_rigel()`, every quantum, unconditionally (not
gated on BT or anything else). Same tick, strictly after — Paula's bytes
always go out first. If Paula has continuous traffic, `kprintf`'s ring just
backs up and old bytes get dropped (correct: Paula's protocol is never
corrupted, log completeness degrades gracefully under contention instead of
ever stomping on it).

Both consumers share one physical wire at one baud (9600, matching Paula's
existing `uart_host_open_miniuart(&m->uart_host, 9600)` — `MiniUartBackend`
isn't a real per-caller hardware handle, there's one physical AUX
peripheral addressed via fixed MMIO macros, so two independent `open()`
calls only conflict if they pick *different* baud rates).

### `BELLATRIX_UART_PL011` removed, not carved out

Considered keeping a carve-out (skip the early console claim when this flag
is defined, since Paula's real-PL011 bridge needs the exact header pins
(GPIO 14/15, ALT0) mini-UART would need (ALT5) — a physical pin conflict,
not a sharing problem the ring buffer could fix). Explicitly rejected: the
flag is removed outright.

* `cmake/bellatrix-variant.cmake` — `BELLATRIX_UART_PL011` option deleted.
* `scripts/build.sh` — `BELLATRIX_SERIAL=pl011` case deleted (kept
  `BELLATRIX_SERIAL_LOOPBACK`, which also applies to the mini-UART backend).
* `src/cpu/emu68/bellatrix.c` — the `#elif defined(BELLATRIX_UART_PL011)`
  serial-setup branch deleted; `bellatrix_init_bluetooth()`'s
  `#if defined(BELLATRIX_UART_PL011): skip BT` branch and the
  `if (m->uart_host.backend_type != UART_HOST_BACKEND_NONE) skip BT` guard
  above it both deleted — that guard existed only because Paula might be on
  PL011, which BT also needs; with the flag gone, Paula's backends
  (PTY/mini-UART/log) never touch PL011, so BT and Paula are always on
  disjoint hardware.
* `src/io/serial/uart_host.h`/`.c` — `uart_host_open_pl011()`,
  `UART_HOST_BACKEND_PL011`, and the `PL011Backend pl011` field deleted.

**Not touched**: `src/host/raspi3/pl011_backend.c`/`.h` and
`BELLATRIX_ENABLE_PL011_BACKEND` (the always-on flag gating that file) —
still load-bearing for BT's own direct PL011 use (`bt_hal_raspi3.c`) and for
`pl011_backend_route_header_to_miniuart()`. Only the
*Paula-bridges-to-real-PL011* feature and its flag are gone; the generic
PL011 driver underneath stays, BT needs it.

## Files relevant to this resolution

* `src/host/raspi3/console_log.c`/`.h` — ring buffer, `console_log_init()`
  (called once, early boot), `console_log_drain()` (called every quantum)
* `src/host/raspi3/pl011_backend.c`/`.h` — `pl011_backend_route_header_to_miniuart()`
  (GPIO 14/15 → ALT5 only, extracted from `pl011_backend_route_bluetooth_pi3()`,
  which still does the BT-internal pin routing — GPIO 30-33/43 — unchanged,
  at its normal time in BT bring-up)
* `src/host/raspi3/vc_mailbox.c`/`.h` — shared VideoCore mailbox + real core
  clock query (`vc_get_core_clock_hz()`), needed before any Bellatrix
  subsystem exists — never hardcode the clock, firmware can run the core at
  a different rate even with `enable_uart=1` pinning it
  (`scripts/config.txt` / `patches/0009-bellatrix-boot-config.patch`)
* `emu68/src/aarch64/start.c` — `console_log_init()` call, right after
  `setup_serial()`, before the first `kprintf` (covered by
  `patches/0007-bellatrix-boot-sequence.patch`)
* `src/machine/machine_rigel_step.c` — `console_log_drain()` call in
  `machine_step_host_serial_rigel()`, right after Paula's TX-drain loop;
  weak no-op fallback for `tools/harness` (doesn't link `host/raspi3/*`)
* `patches/0008-bellatrix-console-redirect.patch` — unchanged; still the
  patch that adds `kprintf_set_putc_override()`/`kprintf_set_enabled()` to
  Emu68's `support_rpi.c`, now consumed by `console_log.c`
