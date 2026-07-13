# Emu68 Public Machine API — Current Architecture

The normative contract is
[`docs/emu68_public_api.md`](../../docs/emu68_public_api.md). This context records
the implemented architecture so future work does not confuse the public API
with the legacy physical fault path.

## Status

The fault-independent public machine API is implemented. Bellatrix selects it
through `src/cpu/emu68/emu68_backend.c`; there is no compatibility fallback to
the old fault-dispatch wrapper.

The normal machine-profile access flow is:

```text
translated 68k access
    -> JIT classifies the complete normalized 32-bit access
       -> DIRECT: native AArch64 load/store
       -> EXTERNAL: public callback or cooperative pending access
       -> UNMAPPED/disallowed: guest 68k access error
```

Normal external accesses do not enter Data Abort,
`SYSPageFaultReadHandler()`, `SYSPageFaultWriteHandler()`,
`SYSReadValFromAddr()`, or `SYSWriteValToAddr()`. The former
`emu68_api.h`/`emu68_api_adapter.c` wrapper and its bus-dispatch patch are not
part of the tree.

## Ownership

Emu68 owns 68k execution, architectural CPU state, exception entry, STOP,
guest IPL semantics, JIT classification, modeled-cycle accounting, and exact
continuation after a cooperative access.

The machine host owns guest topology, external devices, scheduling, physical
ARM IRQ/FIQ and exception-vector policy, and the decision to use synchronous or
cooperative access. Bellatrix's adapter maps its topology, converts public bus
descriptors to `bellatrix_bus_access()`, and consumes the generic progress
callback to advance Rigel.

The public ABI contains no Bellatrix, Rigel, PiStorm, GPIO, ARM-vector, MMU-table,
or JIT-register type. Emu68's translated loop reports progress only through the
host-neutral API callback; it does not call a Bellatrix function.

Physical-infrastructure removal and Bluetooth work are independent changes.
The intended dependency order is public API, physical-infrastructure removal,
then Bluetooth, but work already present in another area does not change these
ownership boundaries. Neither later change is part of the API implementation,
and the API does not depend on the physical path that may be removed.

## Implemented contract

- Versioned, opaque single-runtime handle in `emu68_machine.h`.
- Non-overlapping DIRECT, EXTERNAL, and UNMAPPED regions across the 32-bit guest
  address space, with full-width permission and boundary checks.
- Native JIT access for DIRECT memory and explicit pre-fault dispatch for every
  external effective-address, stack, MOVEM, alternate-FC, bitfield, FPU, paired,
  wide, instruction-fetch, exception-stack, and vector access path.
- Synchronous callbacks and cooperative pending/completion with stable identity,
  one pending access, program ordering, and exact native continuation without
  replaying the instruction or write.
- Guest vector 2 format-$7 access-error entry and vector 3 format-$2 odd
  instruction-address entry.
- Bounded runs, STOP, one-shot stop requests, persistent guest IPL, reset, MMU
  unmap/remap, and translation invalidation.
- Generic progress callback with cycle/instruction deltas and PC, emitted at
  safe boundaries and before external accesses.
- Deterministic instruction-aware 68k cycle model. It is not a cycle-exact model
  of the MC68040 pipeline, caches, or physical bus.
- PiStorm remains isolated behind its physical platform backend.

## Validation facts

The public API unit suite covers ABI checks, singleton lifecycle, mapping and
classification, synchronous and cooperative descriptors, pending completion,
bounded runs, progress publication, STOP/stop request/IPL, invalidation, reset,
and guest exception-frame construction. The clean patch stack applies and
reverses successfully from pinned submodule commits. Bellatrix builds with the
Emu68 backend, with Emu68 boards enabled or disabled, and with the Musashi
backend selected.

Those facts establish the implemented contract and build integration. They do
not claim cycle-exact MC68040 timing or turn the legacy physical path into part
of the API.
