# The fault handler's role: routing, not synchronization

## Status

Stable. Unlike the multicore core placement (see
[`multicore_topology.md`](../AI_context/consolidated/multicore_topology.md)),
this is not a provisional stabilization choice — it is Emu68's original,
supported mechanism for external bus access, and Bellatrix's confirmed,
current integration point for it.

## What the fault handler is

Emu68's JIT emits native AArch64 loads/stores for guest memory operands.
Chip RAM and ROM are mapped directly via the AArch64 MMU (`mmu_map()`), so
ordinary RAM traffic never faults — it executes as a plain load/store at
full native speed. Addresses that are *not* directly mapped (custom-chip
registers, CIA, Autoconfig) are left absent/protected on purpose. Touching
one of those produces a synchronous ARM Data Abort, which Emu68 decodes and
turns into a bus access:

```text
JIT load/store (AArch64)
  -> address not mapped -> Data Abort
  -> emu68/src/aarch64/vectors.c: SYSHandler
  -> SYSPageFaultReadHandler() / SYSPageFaultWriteHandler()
  -> SYSReadValFromAddr() / SYSWriteValToAddr()
  -> src/cpu/emu68/vectors.inc (Bellatrix's platform adapter, #included
     into vectors.c under #elif defined(BELLATRIX))
  -> bellatrix_bus_access() -> Rigel/chipset owner
```

That is the fault handler's entire job: **given an address the CPU just
touched, decide what it is and where the access goes.** It answers "what is
this address, and what should happen to it?" — nothing more.

## What the fault handler is *not*

The fault handler is not, and has never been, the mechanism that keeps the
emulated chipset clock moving. It cannot be: it is only entered when the CPU
touches a *non-RAM* address. A guest code path that only reads/writes chip
RAM (the AmigaOS Exec idle loop is the textbook example) never triggers a
Data Abort, so a design that relied on the fault handler to advance Rigel's
time would silently stop advancing VBL/CIA/IPL the moment the CPU stopped
touching hardware registers.

This exact confusion — treating "replace the fault handler with something
more explicit" as if it also meant "build a better way to synchronize
progress" — caused a real regression (2026-07-16): a refactor coupled the
chipset progress report to the same flag that selected the routing
mechanism, and when fault-driven routing became the default, the progress
report got compiled out along with it. Boot hung silently after
`[JIT] Let it go...` on every build variant. The full write-up is in
`AI_context/consolidated/emu68_routing_vs_synchronization.md`; the fix and
its verification are in `AI_context/issues/ISSUE-0061.md`.

## The actual synchronization mechanism (separate, and still evolving)

Chipset time is advanced by `bellatrix_emu68_report_jit_progress()`, called
unconditionally on every pass through `MainLoop()` in
`emu68/src/ExecutionLoop.c` — regardless of whether that pass touched mapped
RAM, faulted into the bus, or did neither. This is what guarantees Rigel
keeps advancing even during RAM-only code. See
[`emu68_internals.md`](emu68_internals.md) for where this sits inside the
execution loop.

As a belt-and-suspenders measure, `src/cpu/emu68/vectors.inc` *also* calls
the same progress-report function on every fault, before dispatching the
access. This is not the primary mechanism — it is a second, redundant call
site inherited from when the `MainLoop` call was (incorrectly) the only one
left standing. It is harmless to keep, but it must not be read as evidence
that fault-driven routing is what makes the chipset clock work.

**This is genuinely not settled yet.** The current design reports CPU
*progress* (retired instructions / modeled cycles) as the input Rigel uses
to advance its own clock — a CPU-progress-driven model. `AI_context/issues/
ISSUE-0058.md` (section 3.1.1) raises, and does not close, a more fundamental
question: should Rigel's clock instead be driven by its own timer on its own
core (Core 2 already runs a ~250 kHz event stream), independent of whatever
the CPU is doing, with CPU/MMIO contact only used to *observe or stamp*
chipset time rather than *drive* it? The `BELLATRIX_TIMELINE_MODE=cpu|
realtime|hybrid` policies in
[`runtime_and_timing.md`](runtime_and_timing.md) are the current, still
evolving, answer to parts of that question — they are recent additions
(introduced alongside the Core 3 host-reactor split), not a long-settled
design.

The retired "public machine API" (see
[`emu68_public_api.md`](emu68_public_api.md)) got the routing half of its
premise wrong, but its `progress()` callback — a host-neutral, explicit
report of cycle/instruction deltas at safe boundaries — is architecturally
close to what `bellatrix_emu68_report_jit_progress()` does today by hand.
That document is kept, not deleted, precisely because this synchronization
question is still open and its design work may be worth revisiting once the
CPU-driven-vs-timer-driven question above is resolved.

## Practical rule

Before gating any block of code behind an existing flag, ask: *does this
flag decide the same question this block answers, or is it a different
question that just happened to be born in the same commit?* If it's a
different question, the wrong flag is the tell that two distinct decisions
got fused into one. This is the rule the 2026-07-16 regression violated, and
the one to check against next time routing or synchronization code moves.
