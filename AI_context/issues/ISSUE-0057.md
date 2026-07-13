---
id: ISSUE-0057
title: "Implement the fault-independent Emu68 public machine API"
status: done
priority: high
type: architecture
owner: unassigned
created_at: 2026-07-12
updated_at: 2026-07-13
tags: [emu68, jit, bus-api, exceptions, fault-handler, architecture]
related_files:
  - docs/emu68_public_api.md
  - AI_context/consolidated/emu68_public_api.md
  - src/cpu/emu68/emu68_machine.h
  - src/cpu/emu68/emu68_machine.c
  - src/cpu/emu68/emu68_machine_emit.c
  - src/cpu/emu68/emu68_machine_platform.c
  - src/cpu/emu68/emu68_backend.c
  - emu68/src/ExecutionLoop.c
  - emu68/src/M68k_EA.c
---

# Result

The public machine API defined by `docs/emu68_public_api.md` is implemented at
the JIT memory-emission boundary. Bellatrix uses it as the sole normal Emu68
external-access path. DIRECT memory stays native, EXTERNAL memory reaches an
explicit callback or cooperative exit, and UNMAPPED/disallowed accesses enter
guest 68k fault semantics before any host access can fault.

The obsolete `emu68_api.h`/`emu68_api_adapter.c` fault wrapper, its dispatcher
patch, and its fallback to `bellatrix_bus_access()` are removed. Data Abort is
not a normal machine bus mechanism.

# Architectural invariants

- The public ABI is opaque, versioned, and host-neutral.
- Every guest access path uses full-width DIRECT/EXTERNAL/UNMAPPED
  classification, including instruction fetch and exception traffic.
- Synchronous and cooperative external accesses are ordered and never replay a
  completed write.
- Bounded execution, modeled cycles, progress callbacks, reset, STOP, stop
  request, guest IPL, and invalidation are owned by the machine API.
- Emu68 does not own physical ARM IRQ/FIQ, vector policy, timers, or host
  devices.
- Legacy physical-infrastructure removal and Bluetooth are independent work;
  neither belongs to this API, and this API does not depend on the legacy path.
- PiStorm remains behind its platform backend.

# Validation record

- Emu68 machine API unit tests pass, including ABI, regions, access modes,
  continuation, bounded runs, progress and exception frames.
- The complete patch stack verifies from pinned submodule commits.
- Bellatrix Emu68 builds pass with Emu68 boards disabled and enabled.
- Bellatrix's Musashi-selected build also passes, confirming backend separation.

The cycle model is deterministic and instruction-aware, not a cycle-exact
MC68040 pipeline/cache/bus simulation. The normative API document is the source
of truth for all public behavior and limits.
