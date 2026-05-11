# Sprint 27 — Bare Metal Multicore Runtime

## Goal
Wire RPi3 multicore for Bellatrix: Core 1 dedicated to chipset advance (Agnus/CIA/Paula/Denise).

## Status: DONE

## What was done

### Cleaned up wrong harness POSIX multicore (from Sprint 26 mistake)
- Deleted `src/runtime/multicore.c` and `src/runtime/multicore.h` (POSIX pthread — wrong target)
- Reverted `tools/harness/main.c`: removed BellatrixMulticore API, using `bellatrix_bridge_cpu_progress()` directly
- Removed `${SRC}/runtime/multicore.c` from CMakeLists.txt BELLATRIX_SOURCES

### Bare metal multicore implemented in `src/cpu/bellatrix.c`
- `s_cpu_cycles_pending` (`_Atomic uint32_t`) — Core 0 accumulates JIT quanta here
- `s_chipset_lock` (`atomic_flag`) — spinlock protecting `bellatrix_machine_advance()` and MMIO from concurrent access
- Strong override `bellatrix_runtime_notify_cpu_progress()` — posts cycles + SEV in multicore mode, or advances directly in single-core
- Strong override `bellatrix_runtime_host_step()` — drains counter, takes lock, calls `bellatrix_machine_advance()`, releases lock + SEV
- Strong override `bellatrix_runtime_mmio_barrier()` — DMB ISH
- `bellatrix_bus_access()` now acquires chipset lock around `bellatrix_bridge_cpu_write/read()` for multicore safety
- `bellatrix_init()` calls `PAL_Core_SetMulticoreEnabled(1)` + `PAL_Core_LaunchChipset(NULL)` to start Core 1

### Bridge update (`src/bridge/bellatrix_bridge.c`)
- `bellatrix_bridge_cpu_progress()` now calls `bellatrix_runtime_notify_cpu_progress(cycles)` instead of `bellatrix_machine_advance()` directly

### Harness stub (`src/host/posix/pal_posix.c`)
- Weak `bellatrix_runtime_notify_cpu_progress()` added — falls back to `bellatrix_machine_advance()` for single-core harness

### Secondary core boot (`emu68/src/aarch64/start.c`)
- Added `#elif defined(BELLATRIX)` block in `secondary_boot()`:
  - cpu_id==1 calls `bellatrix_core1_entry()` (defined in `pal_core.c`, waits for `s_chipset_entry` → `chipset_core_loop()`)
  - Other cores fall through to `while(1) wfe` (pending: Core 2 for GFX, Core 3 for audio)

### Patch updated (`patches/0002-add-bellatrix-bus-hook.patch`)
- Regenerated to include the secondary_boot BELLATRIX hunk alongside existing changes

## Architecture: 4-core plan

| Core | Role |
|------|------|
| 0    | Emu68 JIT (CPU emulation) |
| 1    | Chipset main tick (Agnus/CIA/Paula) — WIRED |
| 2    | GFX/Denise — pending |
| 3    | Audio — pending |

## Synchronization invariant
- Core 0 (MMIO) and Core 1 (advance) never hold `s_chipset_lock` simultaneously
- `atomic_exchange` on `s_cpu_cycles_pending` ensures only one caller processes the cycles
- SEV/WFE used for efficient wakeup (no busy-spin when idle)

## Tests
- 4/4 harness tests pass after changes
- Multicore only activates on bare metal (pal_posix returns IsMulticoreEnabled=0)
