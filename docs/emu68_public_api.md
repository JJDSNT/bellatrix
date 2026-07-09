# Emu68 Public API

## Purpose

This document defines the first public API boundary between Bellatrix and the
Emu68 backend.

The goal is not to make Emu68 look like Musashi. The goal is to expose a small
host-facing contract around the Emu68 runtime/JIT so Bellatrix can stop depending
directly on private details where a public boundary is practical.

## Ownership Rule

Bellatrix does not keep direct source changes inside the `emu68/` submodule.

Rules:

- Bellatrix-owned API code lives under `src/cpu/emu68/`.
- Build integration lives in `cmake/bellatrix-variant.cmake`.
- Changes to the Emu68 submodule exist only as files in `patches/`.
- Emu68 patches should be limited to places where upstream code must call into
  the Bellatrix-owned layer.

## Current Files

Bellatrix-owned API:

- `src/cpu/emu68/emu68_api.h`
- `src/cpu/emu68/emu68_api_adapter.c`

Boot integration:

- `src/cpu/emu68/bellatrix.c` creates the singleton API handle during
  Bellatrix initialization and registers the bus callbacks.
- Successful registration prints `[EMU68-API] v1 bus registered` on the serial
  log.

Build registration:

- `cmake/bellatrix-variant.cmake`

Emu68 patch:

- `patches/0021-emu68-public-bus-dispatch.patch`

## Relationship To Patch 0002

`patches/0002-add-bellatrix-bus-hook.patch` remains the original Emu68 bus hook.
It installs the Bellatrix fault/MMIO path in `vectors.c` and related startup
integration.

`patches/0021-emu68-public-bus-dispatch.patch` does not replace that hook. It
adds a public dispatch layer inside the existing hook:

```text
Emu68 vectors.c fault path
  -> emu68_api_dispatch_bus_access(...)
      -> registered Bellatrix bus callbacks
          -> bellatrix_bus_access(...)
  -> fallback bellatrix_bus_access(...) if no API handler is active
```

This keeps the proven live path while introducing a public boundary.

## API Surface In Phase 1

Implemented and usable:

- `emu68_api_version()`
- `emu68_create()`
- `emu68_destroy()`
- `emu68_set_bus()`
- `emu68_set_event_callback()`
- `emu68_get_stats()`
- `emu68_reset_stats()`
- `emu68_reset()`
- `emu68_set_irq_level()`
- `emu68_get_state()`
- `emu68_set_state()`
- `emu68_invalidate_code_range()`
- `emu68_invalidate_all_code()`
- `emu68_api_dispatch_bus_access()`

Reserved but not operational yet:

- `emu68_run_cycles()`
- `emu68_step()`
- `emu68_set_hle()`

## Instance Model

The public type is `emu68_t *`, but the current implementation is a singleton.

Reason: the current Emu68 runtime is still global and driven by `MainLoop()`.
The public handle exists so callers do not need to know that implementation
detail, and so the API can evolve toward a real instance model later.

## Bus Model

The bus API supports 8/16/32-bit reads and writes with an access space:

- `EMU68_SPACE_DATA`
- `EMU68_SPACE_PROGRAM`
- `EMU68_SPACE_CPU`

Current Bellatrix registration maps all live fault-path accesses as
`EMU68_SPACE_DATA`.

Return status:

- `EMU68_BUS_OK`: access completed.
- `EMU68_BUS_SYNC_REQUIRED`: access completed, but host synchronization is
  requested.
- `EMU68_BUS_ERROR`: access was not handled; the caller may use fallback.

In phase 1, `EMU68_BUS_SYNC_REQUIRED` emits an event but does not stop execution,
because there is no real `run_cycles()` window yet.

Bellatrix currently returns `EMU68_BUS_SYNC_REQUIRED` for writes to selected
critical custom registers and CIA windows:

- `BLTSIZE`
- `COPJMP1`
- `COPJMP2`
- `DMACON`
- `INTENA`
- `INTREQ`
- CIA-A / CIA-B register windows

This is intentionally conservative. It provides the public signal now while the
live path remains continuous.

## Statistics

The adapter exposes `emu68_stats_t` through:

- `emu68_get_stats()`
- `emu68_reset_stats()`

Tracked counters:

- bus reads
- bus writes
- sync-required bus results
- bus errors
- unhandled dispatches
- unsupported access sizes
- stop requests
- invalidations

These counters are intended for diagnostics and API validation. They should not
be used as timing or compatibility semantics.

Bellatrix exposes a temporary diagnostic control address:

- write `0x01` to `0xDFFF08`: dump API stats
- write `0x02` to `0xDFFF08`: reset API stats
- write `0x03` to `0xDFFF08`: dump then reset API stats

This mirrors the existing private control-address style used by btrace/profile
and is not an Amiga hardware register.

## Execution Windows

`emu68_run_cycles()` and `emu68_step()` currently return unsupported.

The reason is structural: Emu68 execution is still owned by the continuous
`MainLoop()` JIT path. A real implementation needs a separate change to make the
loop return to the host on cycle budget, sync boundary, stop request, exception,
or halt.

## IRQ

`emu68_set_irq_level()` updates the current Emu68 interrupt level through
`__m68k_state`.

This is enough for the current Bellatrix runtime, but a future API pass should
clarify whether IRQ injection belongs to the public adapter or remains owned by
the existing `CpuBackend` abstraction.

## State

The phase-1 state API exposes:

- D0-D7
- A0-A7
- PC
- SR
- USP
- SSP
- VBR

It does not expose FPU, MMU, CACR, SFC/DFC, MSP, or binary snapshots.

## Invalidation

`emu68_invalidate_code_range()` and `emu68_invalidate_all_code()` call the Emu68
ICACHE invalidation functions.

This is the right public shape, but it still needs validation against all JIT
translation-unit invalidation cases required by self-modifying code, overlays,
ROM patching, and executable RAM writes.

## HLE

The HLE callback structure exists in the header, but no trap/A-line/F-line or
illegal-instruction dispatch is wired yet.

This remains phase 2.

## Next Steps

1. Keep the current bus dispatch API and validate it does not regress boot.
2. Decide whether `CpuBackend` should consume `emu68_t` directly.
3. Add real execution window support in the Emu68 loop.
4. Make `EMU68_BUS_SYNC_REQUIRED` terminate a run window once windowed execution
   exists.
5. Wire HLE callbacks only after the lifecycle/run contract is stable.

## Validation Notes

Validated on 2026-07-09:

- `scripts/build.sh` succeeds with the default launcher-enabled image.
- `BELLATRIX_LAUNCHER=0 scripts/build.sh` succeeds for non-interactive QEMU.
- `scripts/setup.sh --verify` succeeds and includes
  `0021-emu68-public-bus-dispatch.patch`.
- A short QEMU run with `BELLATRIX_LAUNCHER=0` reaches Bellatrix init, prints
  `[EMU68-API] v1 bus registered`, enters the Emu68 JIT, disables overlay, and
  configures the legacy Z2 Fast RAM board.
- A QEMU run with `src/roms/aros.rom` confirms that the public dispatcher is in
  the live fault path:

```text
[EMU68-API] v1 bus registered
[EMU68-API] first bus write addr=000000e4 size=4 value=03fbdec0 pc=00000000
[EMU68-API] first bus read addr=00f00000 size=2 value=00000000 pc=00f8011c
[EMU68-API] first sync-required addr=00bfe001 size=1 value=00000000 pc=00f80144
```

The same run reaches AROS serial output, including resident module listing,
`ROMInfo: 1MiB ROM detected`, and Z2 Fast RAM autoconfig.

The QEMU run is intentionally treated as a boot sanity check because Emu68 under
QEMU TCG is slow. The dispatcher path is confirmed by the first-occurrence API
logs above; explicit stats dump through a guest write to `0xDFFF08` remains a
separate diagnostic.
