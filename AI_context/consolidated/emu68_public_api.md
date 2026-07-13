# Emu68 Public Machine API — Authoritative Status

## Status warning

**The public Emu68 machine API is not implemented.**

The files currently named `emu68_api.h` and `emu68_api_adapter.c` do not satisfy
the public API contract. They are a Bellatrix-owned experimental adapter around
the existing Emu68 singleton and page-fault bus path.

The following messages are not evidence that the public API is ready:

```text
[EMU68-API] v1 bus registered
[EMU68-API] first bus read ...
[EMU68-API] first bus write ...
[EMU68-API] first sync-required ...
```

They prove only that the experimental adapter was registered and invoked after
a Data Abort. They do not prove explicit JIT bus dispatch, host ownership of the
ARM exception infrastructure, or independence from fault-as-bus.

The normative contract is
[`docs/emu68_public_api.md`](../../docs/emu68_public_api.md). Implementation work
is tracked by [`ISSUE-0057`](../issues/ISSUE-0057.md). If this context conflicts
with an older issue or historical note, this file and the normative document
take precedence.

## Current implementation

The current Bellatrix Emu68 bus flow is:

```text
JIT emits native LDR/STR
    -> unmapped/protected guest address
    -> ARM Data Abort
    -> SYSPageFaultReadHandler()/SYSPageFaultWriteHandler()
    -> SYSReadValFromAddr()/SYSWriteValToAddr()
    -> emu68_api_dispatch_bus_access()
    -> Bellatrix callback
    -> bellatrix_bus_access() fallback when unhandled
```

Relevant current files are:

- `src/cpu/emu68/emu68_api.h`;
- `src/cpu/emu68/emu68_api_adapter.c`;
- `src/cpu/emu68/bellatrix.c`;
- `patches/0002-add-bellatrix-bus-hook.patch`;
- `patches/0021-emu68-public-bus-dispatch.patch`;
- patched `emu68/src/aarch64/vectors.c` and `ExecutionLoop.c`.

What exists today:

- an opaque-looking handle over one global Emu68 runtime;
- 8/16/32-bit callbacks reached from `vectors.c` after Data Abort;
- estimated bounded windows around the patched `MainLoop()`;
- guest IPL publication, STOP handling, stop request, partial state access,
  invalidation wrappers, events and diagnostic counters;
- successful builds and AROS boot progress through that experimental path.

What does not exist today:

- an upstream-neutral public header and runtime in the main repository, with
  Emu68 patches limited to unavoidable JIT integration hooks;
- a direct/external/unmapped guest-region registry;
- JIT classification before host memory is touched;
- an explicit external-access helper emitted by the JIT;
- a cooperative pending-access get/complete protocol;
- modeled 68k cycle accounting required by the run contract;
- a Bellatrix Emu68 profile in which normal guest bus traffic is independent of
  Data Abort and `SYS*ValFromAddr()`;
- a public API whose operation has been validated independently of the legacy
  fault dispatcher.

Therefore the current adapter must never be described simply as “the public
Emu68 API.” Use “experimental Bellatrix fault-path adapter” when referring to
the existing implementation.

## Required architecture

The public API is implemented at the Emu68 JIT memory-emission boundary, not in
`vectors.c` and not as another layer around
`emu68_api_dispatch_bus_access()`.

The required access split is:

```text
DIRECT region
    -> existing native AArch64 load/store

EXTERNAL region
    -> explicit synchronous callback
       or cooperative EMU68_STOP_EXTERNAL_ACCESS

UNMAPPED address
    -> defined 68k bus-error/unmapped semantics
```

The JIT must normalize the computed address to 32 bits before classification so
the aliases created by `mirror_page()` retain identical guest semantics.

The MMU remains responsible for direct RAM/ROM mappings, JIT mappings,
protection, aliases and cache attributes. Only its accidental second role as an
external-region detector through faults is removed from the machine profile.

Physical ARM IRQ/FIQ and 68k IPL remain distinct:

```text
physical ARM IRQ/FIQ
    -> Bellatrix host/platform

guest 68k IPL
    -> emu68_machine_set_ipl()
    -> Emu68 architectural interrupt delivery
```

The API cannot expose ARM vector ownership, GPIO, PiStorm transaction slots,
host core numbers, or JIT-private state. PiStorm remains behind its own platform
backend.

## Completion condition

The public API may be called implemented only when all behavior defined in
`docs/emu68_public_api.md` exists and is validated. At minimum:

- direct RAM/ROM still use native JIT loads/stores;
- every external access reaches the explicit API before any host access;
- synchronous and cooperative modes follow the documented ordering rules;
- cooperative reads and writes use the documented pending-access lifetime;
- unmapped accesses use defined 68k semantics;
- no normal Bellatrix transaction reaches a page-fault or
  `SYS*ValFromAddr()` handler;
- guest IPL, STOP, reset, stop request, bounded run and invalidation work through
  the public contract;
- the API neither acquires nor depends on physical ARM interrupt or exception
  ownership; transferring legacy physical infrastructure to Bellatrix is a
  separate implementation and is not an API acceptance gate;
- PiStorm continues to work behind its platform backend;
- tests and traces demonstrate zero fault-originated normal bus transactions.

Until every condition holds, the API status remains **not implemented**, even
if the current adapter builds, boots, logs callbacks, or reaches an AROS screen.

The API must also respect CPU-backend ownership. Historical ISSUE-0043 was
archived without implementing its separation: `src/cpu/emu68/bellatrix.c`
still selects and initializes Musashi and owns the generic `CpuBackend` loop.
New API code must not deepen that coupling. Generic backend selection and
execution belong in `src/cpu/`; only the Emu68 backend adapter belongs in
`src/cpu/emu68/`.
