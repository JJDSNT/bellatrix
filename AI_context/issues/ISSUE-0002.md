---
id: ISSUE-0002
title: "Emu68 integration — performance architecture"
status: doing
priority: high
type: refactor
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - emu68
  - performance
  - mmio
  - jit
  - rigel
related_files:
  - src/cpu/emu68/bellatrix.c
  - src/machine/machine_rigel_step.c
  - src/machine/machine_rigel_bus.c
  - external/rigel/include/rigel/rigel_time.h
  - external/rigel/include/rigel/rigel_bus.h
---

# Issue: Emu68 integration performance architecture

## Context

Emu68 should outperform Musashi on normal CPU execution, but the current
Bellatrix integration can make it slower in practice. Recent profiling shows
the JIT progress hook is now working (`advance_time.calls > 0`) and
`PAL_Runtime_Poll()` is no longer the dominant cost. The new bottleneck is the
combination of:

- every Emu68 MMIO access entering Bellatrix through an AArch64 data abort;
- `bellatrix_machine_read/write()` flushing Rigel timing before every bus
  access;
- `bellatrix_emu68_report_jit_progress()` advancing the chipset at MMIO
  granularity;
- generic machine dispatch doing a full read/write path even for hot registers.

Observed profile after the cleanup:

```text
bellatrix_bus_access       ~6K CNT cycles per MMIO
advance_time               ~5K CNT cycles per progress call
disp_read / bridge_ref     ~5.6K-5.8K CNT cycles
fault_overhead_estimate    ~400 CNT cycles
poll                       ~30 CNT cycles
```

The data-abort overhead matters, but it is not the dominant cost anymore. The
dominant cost is how often Bellatrix asks Rigel to synchronize and how generic
the dispatch path is for very hot MMIO registers.

## Rigel APIs to use

Prefer public Rigel APIs before reaching into internals:

- `rigel_step(ctx, cycles)` and `rigel_step_until(ctx, target_time)` from
  `rigel_time.h`.
- `rigel_get_time(ctx)`, `rigel_get_next_deadline(ctx)`,
  `rigel_get_next_bus_change(ctx)`, `rigel_get_cpu_resume_time(ctx)`.
- `rigel_get_bus_state(ctx)`, `rigel_cpu_can_access_custom(ctx)`,
  `rigel_cpu_can_access_chip_ram(ctx)` from `rigel_bus.h`.
- `rigel_custom_read16/write16(ctx, reg)` for DFFxxx custom chip registers.
- `rigel_get_intreq(ctx)`, `rigel_get_intena(ctx)`, `rigel_get_ipl(ctx)` for
  interrupt fast reads/publication.
- `rigel_cia_read/write(ctx, cia_id, reg)` for CIA accesses.
- `rigel_get_frame()` and frame events only after `RIGEL_EVENT_FRAME_READY`.

Existing Bellatrix helpers already use the temporal API:

- `machine_next_quantum()` combines `rigel_get_next_deadline()` and
  `rigel_get_next_bus_change()`.
- `machine_step_components()` accumulates approximate M68K cycles and advances
  Rigel only when the current quantum is consumed.
- `machine_flush_for_bus()` forces partial advancement before bus reads/writes.

The next integration should build on these instead of adding another scheduler.

## Architectural direction

### 1. Separate CPU progress from bus synchronization

Current behavior effectively does:

```text
JIT MMIO fault
  -> report JIT progress
  -> advance Rigel
  -> machine_read/write
  -> flush Rigel again for bus correctness
```

This is correct enough to make frames advance, but too expensive.

Target behavior:

```text
JIT progress
  -> accumulate M68K cycles
  -> publish/flush only at temporal deadlines or required bus boundaries

MMIO access
  -> classify address
  -> flush only if this access needs up-to-date chipset/CIA state
  -> direct/public Rigel API fast path where safe
  -> fallback to generic machine_dispatch
```

Add an Emu68-facing adapter, not more logic inside `vectors.c`:

```c
uint32_t bellatrix_emu68_mmio_read(uint32_t addr, unsigned size);
void     bellatrix_emu68_mmio_write(uint32_t addr, uint32_t value, unsigned size);
void     bellatrix_emu68_report_jit_progress(uint64_t insn_count, uint32_t pc);
void     bellatrix_emu68_flush_cpu_progress(enum flush_reason reason);
```

`vectors.c` should only save/restore live JIT registers and call this adapter.

### 2. Make flush policy explicit

Introduce a small policy table for MMIO flush decisions.

Must flush before read:

- beam/position reads: `VPOSR`/`VHPOSR` (`DFF004`, `DFF006`);
- interrupt readbacks: `INTENAR`/`INTREQR` (`DFF01C`, `DFF01E`);
- CIA timer/ICR/SDR reads when the register can change due to elapsed time or
  host input;
- bus-state-sensitive reads if `rigel_get_bus_state()` reports the CPU would
  stall or a bus owner change is due.

Flush before write:

- writes that schedule DMA/copper/blitter/audio/disk work:
  `DMACON`, `COPJMP`, copper pointers, bitplane pointers, blitter registers,
  audio registers, disk registers;
- interrupt writes `INTENA`/`INTREQ`, so post-write IPL publication is correct.

May defer or use coarse flush:

- color palette writes during boot/fill loops, unless mid-frame rendering is
  active;
- repeated polling of static board/autoconfig registers;
- memory/expansion IO that does not affect Rigel timing.

The first implementation should be conservative: flush all custom/CIA reads,
but coalesce pure CPU progress. Then relax hot registers one by one based on
tests and traces.

### 3. Reduce `advance_time` calls

Add an Emu68 progress accumulator:

```text
pending_m68k_cycles += delta_cycles

flush when:
  pending >= recommended quantum
  or MMIO policy requires fresh chipset state
  or IRQ publication must be current
  or before returning to guest after a long block
```

Use the existing Rigel-aware recommendation instead of a hardcoded constant:

- `bellatrix_machine_recommended_cpu_quantum(max_cycles)` already converts the
  next Rigel deadline/bus-change into approximate CPU cycles.
- For Emu68, cap it with a small max first (`128`, `256`, maybe `512`) to avoid
  visible timing drift while measuring.

New profiling counters to add:

- `advance_flush.calls`
- `advance_flush.cycles_total`
- `advance_flush.reason_mmio_sensitive`
- `advance_flush.reason_quantum`
- `advance_flush.reason_irq`
- `pending_m68k_cycles_avg/max`

Success criterion: `advance_time.calls` should drop significantly while frame
count and keyboard/IRQ behavior remain correct.

### 4. Add an Emu68 MMIO fast path

The hot path should classify common MMIO without the full generic dispatch:

- DFFxxx custom registers:
  - use `rigel_custom_read16/write16()` for normal 16-bit accesses;
  - use `rigel_get_intreq/intena/ipl` for interrupt readback/publication where
    it avoids extra domain dispatch;
  - keep fallback for byte/long odd cases and traced/debug paths.
- CIA:
  - decode `(addr >> 8) & 0x0f`;
  - call `rigel_cia_read/write()`;
  - preserve keyboard SDR side effects and tracing currently in
    `machine_dispatch_read/write`.
- Autoconfig/Z2/Z3/superbuster/expansion:
  - keep generic dispatch initially.

Do not bypass `machine_dispatch_*` until the side effects are audited. Some
current code logs keyboard SDR, publishes floppy trace, syncs IPL after writes,
and handles byte merging. The fast path needs either to preserve those effects
or explicitly opt out only for addresses where they are irrelevant.

### 5. Avoid direct Rigel internals in the final shape

There is existing code that includes `core/rigel_context.h` for diagnostics.
That is acceptable for short-term debug, but performance architecture should
prefer public APIs or add small public Rigel helpers when needed.

Potential public API additions to Rigel if profiling justifies them:

- `rigel_custom_peek16(ctx, reg)` for readback without side effects, only where
  semantically valid.
- `rigel_get_dmacon(ctx)` if `DMACONR` polling remains hot and the current
  public custom-register read path is too generic.
- richer deadline reasons from `rigel_get_next_deadline()` so Bellatrix knows
  whether it is waiting for beam, IRQ, blitter, copper, audio, or DMA.

## Multicore performance plan

The multicore path should be used to remove Rigel stepping from the CPU/JIT
critical path, not to add more locks around each MMIO.

### Current multicore shape

`bellatrix_runtime_publish_cpu_cycles()` converts M68K cycles to CCK and adds
them to `s_cpu_cck_target`. Core 1 runs `bellatrix_runtime_host_step()` and
calls `rigel_step()` in `CHIPSET_QUANTUM=128` chunks until it catches up.

This is a good base, but it needs bus synchronization semantics:

- Core 0 owns Emu68 execution and MMIO fault handling.
- Core 1 owns Rigel stepping.
- MMIO reads/writes that need exact state must synchronize with Core 1 before
  dispatch.
- Non-sensitive writes/progress can be published asynchronously.

### Proposed multicore model

Use a single-producer/single-consumer command queue from Core 0 to Core 1:

```text
Core 0:
  publish cycles asynchronously
  enqueue MMIO command only when access needs Core 1 ownership
  wait for response for reads/synchronous writes

Core 1:
  drain cycle target up to requested time/deadline
  execute MMIO command against Rigel
  publish result + IPL/frame events
```

Command types:

- `ADVANCE_TO_CCK`
- `MMIO_READ`
- `MMIO_WRITE`
- `SYNC_IPL`
- `PRESENT_FRAME` or frame-ready notification

Important rule: avoid taking a coarse chipset lock inside every Emu68 fault.
The queue should batch progress and only round-trip for accesses that require
fresh chipset state or a return value.

### Multicore fast paths

Not every MMIO needs a round-trip:

- Writes to many custom registers can be queued and the CPU can continue if the
  guest does not immediately read dependent state.
- Reads must usually wait, but hot readbacks can use a published shadow if the
  value is explicitly maintained by Core 1:
  - `INTREQ/INTENA/IPL` shadows;
  - beam position shadow with timestamp/cycle, if validated;
  - keyboard/CIA SDR shadow only if side effects are preserved.

Start with synchronous reads and queued writes. Optimize read shadows only
after correctness is stable.

### Core assignment

Recommended staged use:

1. Core 0: Emu68 JIT and MMIO fault handler.
2. Core 1: Rigel chipset step + custom/CIA MMIO command execution.
3. Core 3: physical IO (USB/BT/SD/host UART) as already intended.
4. Core 2: audio only after Paula/audio timing is isolated enough to avoid
   locking the whole Rigel context from two chipset cores.

Do not split Rigel domains across cores until Rigel itself has a domain-safe
API. The first useful multicore performance win is moving the whole Rigel
stepper off Core 0, not parallelizing Agnus/Paula/Denise internally.

## Measurement plan

Before changing behavior, add counters that make regressions obvious:

- MMIO count by flush policy result: no-flush, pending-flush, forced-flush.
- Core 1 queue depth and wait time.
- MMIO read round-trip cycles in multicore.
- cycles published per Core 1 wake.
- `rigel_step()` total cycles and average step size.
- hot address table split by read/write and by flush reason.
- frame counter per wall-clock second.
- Emu68 executed M68K cycles per wall-clock second.

Compare these configurations:

1. Musashi single-core baseline.
2. Emu68 current single-core.
3. Emu68 with progress coalescing.
4. Emu68 with MMIO fast path.
5. Emu68 multicore with synchronous reads and queued writes.

Target for the next sprint:

- `advance_time.calls` reduced by at least 4x on boot/Kickstart polling loops.
- effective per-MMIO cost below 2K-3K CNT cycles for hot readbacks.
- no regression in frame count, keyboard input, IRQ delivery, or autoconfig.
- Emu68 reaches or exceeds Musashi throughput on a ROM boot workload.

## Suggested implementation order

1. Add profiling buckets for progress flush reasons and MMIO policy decisions.
2. Add Emu68 pending-cycle accumulator using
   `bellatrix_machine_recommended_cpu_quantum()`.
3. Keep all MMIO dispatch generic but flush only through the new policy.
4. Validate hardware logs and compare against Musashi.
5. Add fast paths for `DFF004/DFF006/DFF01C/DFF01E/DFF09A/DFF09C` and CIA hot
   reads one at a time.
6. Prototype multicore as whole-Rigel-on-Core1 with a command queue.
7. Only after that, consider deeper Emu68 JIT integration to avoid data aborts
   entirely for known MMIO pages.
