# Sprint 28 — Activate Cores 2 and 3 (Audio + IO)

## Goal

Split the Core 1 "all chipset" monolith into three dedicated bare-metal secondary cores:
- Core 1 — GFX/Agnus (was already running all three)
- Core 2 — Paula audio (now activated)
- Core 3 — CIA / serial / disk (now activated)

## Changes

### `src/host/pal.h`
- Added `PAL_Core_LaunchAudio()` and `PAL_Core_LaunchIO()` declarations.

### `src/host/raspi3/pal_core.c`
- Added `__attribute__((weak))` stubs for `bellatrix_runtime_audio_step()` and `bellatrix_runtime_io_step()`.
- Added `chipset_audio_loop()` — Core 2 loop: reads cntpct, calls `bellatrix_runtime_audio_step()`, WFE.
- Added `chipset_io_loop()` — Core 3 loop: reads cntpct, calls `bellatrix_runtime_io_step()`, WFE.
- Added `PAL_Core_LaunchAudio()` — sets `s_audio_entry = chipset_audio_loop`, dsb+sev.
- Added `PAL_Core_LaunchIO()` — sets `s_io_entry = chipset_io_loop`, dsb+sev.

### `src/host/posix/pal_posix.c`
- Added no-op stubs for `PAL_Core_LaunchAudio()` and `PAL_Core_LaunchIO()` (harness path).

### `src/cpu/bellatrix.c`

**Cycle accounting split:**
- Replaced `s_cpu_cycles_pending` with two separate accumulators:
  - `s_gfx_cycles_pending` — Core 1 drains
  - `s_io_cycles_pending` — Core 3 drains
- Added `s_published_master_cycles` (`_Atomic uint64_t`) — Core 1 publishes GFX master time after each step; Core 2 reads it via acquire load.

**`bellatrix_runtime_notify_cpu_progress()`** now adds to both `s_gfx_cycles_pending` and `s_io_cycles_pending` simultaneously.

**`bellatrix_runtime_host_step()` (Core 1):** GFX-only now. Drains `s_gfx_cycles_pending`, calls `core_gfx_step()`, publishes `gfx.master_cycles` to `s_published_master_cycles`.

**Added `bellatrix_runtime_audio_step()` (Core 2):** Reads `s_published_master_cycles`, acquires chipset lock, calls `core_audio_step(&g_runtime.audio, master)`.

**Added `bellatrix_runtime_io_step()` (Core 3):** Drains `s_io_cycles_pending`, acquires chipset lock, calls `core_io_step(&g_runtime.io, cycles)`.

**`bellatrix_init()`:** Added `PAL_Core_LaunchAudio()` and `PAL_Core_LaunchIO()` after `PAL_Core_LaunchChipset()`. Updated log message.

## Locking model

`s_chipset_lock` (atomic_flag TAS spinlock + WFE) protects all shared chipset state.
Holders: Core 0 (MMIO), Core 1 (GFX step), Core 2 (Audio step), Core 3 (IO step).
Lock is held briefly per step; contention is low because each core sleeps on WFE between steps.

## Audio master cycle handoff

Audio (Core 2) does not read `s_gfx_cycles_pending` directly. Instead:
1. Core 1 advances GFX and publishes `gfx.master_cycles` to `s_published_master_cycles` (release store).
2. Core 2 reads `s_published_master_cycles` (acquire load) and calls `core_audio_step()` with that value.
3. `core_audio_step()` advances only the delta since `last_master_cycles` (with 4096-cycle clamp).

## Result

7/7 tests pass. Build green.

## Next

- `core_io_step()` currently also calls `bellatrix_machine_sync_ipl()` which publishes IPL through the CPU backend. This is correct but could be moved to be Core-1-driven (after GFX step) for latency reasons.
- Cross-core events: `RuntimeMailbox`/`RuntimeEvent` not yet wired. VBLANK from Core 1 to Core 0 (JIT IPL injection) still goes through the existing `PAL_Runtime_GetPendingIPL()` path.
- `RuntimeSync` ready-flags not yet set by any core (designed but not wired).
