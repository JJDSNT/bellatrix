---
id: ISSUE-0057
title: "Implement the fault-independent Emu68 public machine API"
status: todo
priority: high
type: architecture
owner: unassigned
created_at: 2026-07-12
updated_at: 2026-07-13
tags: [emu68, jit, bus-api, exceptions, fault-handler, architecture]
related_files:
  - docs/emu68_public_api.md
  - AI_context/consolidated/emu68_public_api.md
  - emu68/src/M68k_EA.c
  - emu68/src/ExecutionLoop.c
  - emu68/src/aarch64/mmu.c
  - emu68/src/aarch64/vectors.c
  - src/cpu/emu68/bellatrix.c
  - src/cpu/emu68/emu68_api.h
  - src/cpu/emu68/emu68_api_adapter.c
  - patches/0002-add-bellatrix-bus-hook.patch
  - patches/0021-emu68-public-bus-dispatch.patch
---

# Objective

Implement the complete contract in `docs/emu68_public_api.md` inside Emu68 so a
machine host can use the JIT without Data Abort/page fault as the normal bus
mechanism. The implementation point is the JIT memory-emission boundary, before
any native access to an external or unmapped guest address.

# Authoritative current status

**The public API is not implemented.**

The existing Bellatrix “API v2” is an experimental fault-path adapter. Its
actual bus flow is:

```text
JIT LDR/STR -> Data Abort -> SYSPageFault*Handler
            -> SYS*ValFromAddr -> emu68_api_dispatch_bus_access
            -> callback/fallback
```

Registration logs, callback counters, successful builds, and AROS boot progress
validate only this adapter. They must not be cited as validation of the public
machine API.

# Required implementation

- Add the upstream-neutral public header and runtime in the main repository at
  `src/cpu/emu68/emu68_machine.*`; keep Emu68 patches limited to unavoidable
  JIT integration hooks.
- Implement lifecycle and the opaque single-runtime handle without exposing
  `M68KState` or Bellatrix types.
- Implement non-overlapping DIRECT, EXTERNAL and UNMAPPED region registration.
- Require executable code to be DIRECT, classify the complete access width, and
  reject cross-region or permission-violating accesses before a native load or
  store.
- Preserve `mmu_map()`, TTBR tables, aliases, JIT protection and direct RAM/ROM
  mappings.
- Normalize computed guest addresses to 32 bits before region classification.
- Route every effective-address load/store form through a shared JIT decision
  point, including indexed, pre/post-update, floating-point and wide accesses.
- Emit the existing native load/store for DIRECT regions.
- Emit an explicit host bridge for EXTERNAL regions before any host memory
  access can fault.
- Apply defined 68k bus-error/unmapped behavior for UNMAPPED addresses.
- Implement the documented synchronous callback semantics and ordering.
- Implement the documented cooperative run, pending-access and completion
  semantics for reads and writes.
- Implement modeled 68k cycle and retired-instruction accounting for bounded
  runs.
- Implement reset, stop request, guest IPL and code invalidation with the
  documented thread/ordering contract.
- Reset receives the host-resolved initial SSP and PC; it must not depend on
  Bellatrix reset globals or create a cooperative access while resetting.
- Keep physical ARM IRQ/FIQ, exception-vector ownership and host devices outside
  Emu68's public machine contract.
- Do not include transfer or removal of the legacy physical exception/IRQ
  infrastructure in this issue. It may coexist while the API is implemented,
  but the completed API must not depend on it.
- Keep PiStorm behind its platform backend without exposing its GPIO/slot
  protocol through the public ABI.
- Do not add backend selection, Musashi initialization, or the generic
  `CpuBackend` execution loop to the Emu68 machine API. Historical ISSUE-0043
  was archived without implementing that separation: generic and Musashi
  responsibilities must move to `src/cpu/`, while the Emu68 adapter remains
  under `src/cpu/emu68/`.
- Remove every Bellatrix normal-bus dependency on
  `SYSPageFaultReadHandler()`, `SYSPageFaultWriteHandler()`,
  `SYSReadValFromAddr()`, `SYSWriteValToAddr()`, and the legacy fallback.
- Leave Data Abort installed only for real host/JIT faults and diagnostics.

# Validation

- Verify the public header ABI and structure-size/version behavior.
- Test DIRECT, EXTERNAL and UNMAPPED classification across 32-bit alias forms.
- Test all access widths, function codes, address spaces and effective-address
  modes.
- Test synchronous ordering and bus-error propagation.
- Test cooperative pending-access identity, completion, reset cancellation,
  stop request and concurrent IPL publication.
- Test translation invalidation after every topology change.
- Boot KS1.3, KS3.1 and AROS with CIA, custom registers, autoconfig, ROM overlay
  and Fast RAM coverage.
- Validate both single-core and multicore ownership paths.
- Verify PiStorm behavior remains isolated and functional.
- Instrument the exception path and prove that normal bus transactions cause
  zero Data Aborts.

# Acceptance criteria

The issue is complete only when the normative document is fully implemented and
validated. A Bellatrix Emu68 build must use explicit JIT dispatch as the sole
normal external-access path; Data Abort must identify only a real fault or
diagnostic condition. No adapter registration message, partial callback path,
boot milestone, or subset of the API is sufficient to mark it complete.
