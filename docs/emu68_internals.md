# Emu68 internals used by the Bellatrix integration

## Status

Describes the current, temporary stabilization state (Emu68 on Core 0, to
minimize integration variables; see
[`multicore_topology.md`](../AI_context/consolidated/multicore_topology.md)
and `AI_context/issues/ISSUE-0058.md`). The target architecture keeps Core 0
as Control — the CPU is expected to move off it once this integration is
proven stable; see `docs/system_architecture.md`. This is a
functional-responsibility overview, not a line-by-line walkthrough — the goal
is to know which file owns which decision before changing any of them.

Three files carry almost all of the integration surface:
`emu68/src/aarch64/start.c`, `emu68/src/aarch64/vectors.c`, and
`emu68/src/ExecutionLoop.c`.

## `start.c` — platform bring-up

`boot()` runs on the primary PE (Core 0 in the current topology) and is
responsible for the parts of the machine that exist before any 68k
instruction executes: parsing the device tree, setting up the TLSF
allocators, building the MMU tables (`mmu_map()` for direct RAM/ROM,
`TTBR0_EL1`/`TTBR1_EL1`), waking the three secondary PEs by writing their
spin-table boot addresses and issuing `SEV`, installing `VBAR_EL1`, and
enabling the PMU cycle counter. Only after all of that does it call
`M68K_StartEmu()` — directly, on the same core that ran `boot()`. Bellatrix's
`bellatrix_launch_cpu_and_park()` wraps that call but does not hand the CPU
off anywhere: it invokes the selected backend's entry function on the
current PE and never returns. (A stray comment in the current
`patches/0007-bellatrix-boot-sequence.patch` still says "hands the CPU
backend off to Core 1" — that line predates the Core-0 rebaseline and no
longer describes what the code does; left as-is rather than hand-editing a
patch file, but don't trust that comment.)

`secondary_boot()` runs on each of the other three PEs: same cache/PMU/VBAR
setup, then dispatch by `cpu_id` to `bellatrix_core1_entry()` (auxiliary,
parks in `WFE`), `bellatrix_core2_entry()` (parks until
`PAL_Core_LaunchChipset()` assigns the Rigel role), and
`bellatrix_core3_entry()` (parks until assigned the host-reactor role). The
comments next to this dispatch in `start.c` itself are also stale (they
still read "Core 1 — CPU" / "Core 3 — IO", left over from a topology that
predates the current one) — `src/host/raspi3/pal_core.c` is the accurate,
current source for what each core actually does.

GPU IRQ routing to Core 0 (`0x4000000c`) and per-core PMU/local-timer setup
also happen here, inherited unchanged from upstream Emu68; see
[`irq_and_interrupts.md`](irq_and_interrupts.md) for why none of that gets
removed casually.

## `vectors.c` — the ARM exception vector table

This is the ARM64 exception vector table proper: eight fixed 0x80-byte
slots (sync/IRQ/FIQ/SError, each for SP0 and SPx contexts). Two of its jobs
are documented in their own files rather than repeated here:

- **MMIO/bus routing** — Data Abort decode and dispatch to
  `bellatrix_bus_access()` via `src/cpu/emu68/vectors.inc`. See
  [`fault_handler.md`](fault_handler.md).
- **Physical IRQ/FIQ delivery** — the SPx IRQ/FIQ slots, `INT_shadow`, and
  the Bluetooth UART0 discrimination inside the architectural 0x80-byte
  budget. See [`irq_and_interrupts.md`](irq_and_interrupts.md).

The one thing worth stating plainly here: both mechanisms exist inside the
*same file* because both are, architecturally, "something happened that the
translated 68k code wasn't expecting" — a data fault or a physical
interrupt. That surface-level similarity is exactly why they're easy to
conflate (see the routing-vs-synchronization discussion in
`fault_handler.md`) even though they answer unrelated questions.

## `ExecutionLoop.c` — the JIT dispatch loop (`MainLoop`)

`MainLoop()` is Emu68's central, historically non-returning loop: load the
global `M68KState`, keep hot 68k values pinned in AArch64 registers (see the
ABI constraint in the top-level `CLAUDE.md` — x13-x29 pinned, x18 = M68K PC,
x12 = JIT temp, never touch these in hot-path code), locate or JIT-compile a
translation unit for the current PC, and run it. A translation unit runs
until it hits a control-flow edge (branch, exception, STOP), then control
returns to `MainLoop` to look up the next one.

The one Bellatrix addition to this loop is the progress report described in
[`fault_handler.md`](fault_handler.md): on every pass, unconditionally,
`bellatrix_emu68_report_jit_progress(v30_now, pc_now)` is called, where
`v30` is the SIMD register Emu68 uses as a running modeled-cycle
accumulator. This is what lets chipset time advance even when the 68k code
being executed never touches a single mapped-hardware address — the JIT
loop itself is the only thing guaranteed to run on every instruction,
so it's the only place a synchronization hook can live without being tied to
routing. The 2026-07-16 regression (a refactor deleted this call from
`MainLoop` and relied on a routing-path call instead) is the concrete case
study for why this call belongs here and must stay unconditional; see
`AI_context/issues/ISSUE-0061.md`.

STOP (`emu68/src/M68k_LINE4.c`, `EMIT_STOP`) is generated code, not part of
`MainLoop` itself, but it interacts with the same synchronization question:
it decides whether the JIT should keep crediting fake progress while the
guest is halted (so the chipset clock and IPL delivery keep working) or
actually block on `wfi`/`wfe`. The current Bellatrix STOP checks the
pending-interrupt state and credits `v30` if none is pending, rather than
sleeping — see `AI_context/consolidated/history/ISSUE-0038.md` for the
history of why a raw `wfi()` here is unsafe (nothing routes a real interrupt
to wake it in this integration).

## Where the open questions live

None of this file's three components are settled forever:

- Whether CPU-progress-driven chipset time (what's implemented today) is
  the right long-term model, versus a timer-driven Rigel clock independent
  of the CPU loop, is open — see `fault_handler.md` and
  `runtime_and_timing.md`.
- Whether Emu68 stays on Core 0 at all is an explicitly provisional
  stabilization choice, not a final placement — see
  `multicore_topology.md`.
- The physical-IRQ path's hardware validation is deferred pending
  authorization — see `irq_and_interrupts.md`.
