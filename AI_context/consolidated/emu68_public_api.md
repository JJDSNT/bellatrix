# Emu68 Public Machine API — Working Context

> **DIREÇÃO DE PRODUTO SUPERADA em 2026-07-15 por ISSUE-0058.** Este arquivo
> preserva pesquisa, correções de JIT, testes e resultados A/B úteis. Não usar
> sua premissa fault-independent como arquitetura obrigatória. O baseline atual
> é Emu68 no Core 0 com fault handler nativo enquanto o contrato original de
> startup, IRQ e vectors é auditado.

The normative, high-level contract is
[`docs/emu68_public_api.md`](../../docs/emu68_public_api.md). This file records
implementation and validation state; those details do not belong in the public
document.

## Required architecture

The normal machine-profile flow is:

```text
translated 68k access
    -> normalize the complete 32-bit guest access
    -> classify its full width and permission
       -> DIRECT: native AArch64 load/store
       -> EXTERNAL: explicit callback or cooperative pending access
       -> UNMAPPED/disallowed: guest 68k access error
```

The public ABI is opaque, versioned and host-neutral. Emu68 owns 68k execution,
architectural state, guest exceptions, STOP, IPL semantics, JIT classification
and exact continuation. The host owns topology, external devices and
scheduling. Normal EXTERNAL traffic must not reach the host through Data Abort
or a wrapper around `SYSPageFault*`/`SYS*ValFromAddr()`.

## Current implementation state

Lifecycle, region descriptors, page classification, synchronous and
cooperative access descriptors, bounded runs, progress, reset, STOP, guest IPL,
invalidation, exception construction and a native explicit bridge are present.
The unit suite passes and the image builds.

The runtime implementation is not validated. KS1.3 does not boot through the
explicit bridge, while an A/B configuration using the same classifier and the
old faulting access does boot. ISSUE-0057 must remain `in_progress` until the
real Emu68/QEMU regression passes.

## Facts established by the runtime audit

- Topology must be installed on Core 0 before workers start.
- Cache invalidation must not run before JIT state exists.
- Page classification rebuilds only changed ranges.
- A catch-all external 4 GiB range is invalid; uncovered space is UNMAPPED.
- Full-width page-boundary classification is required.
- The old fault path can still boot KS1.3 under the current surrounding
  scheduler/topology, isolating the regression to explicit bridge execution.
- The first observed bus divergence is an expected `INTENA=0x7fff` write
  becoming `0x0000`, followed by unrelated values.
- Static objdump of the 832-byte native bridge matches its assembly source.
- The unresolved defect concerns runtime-emitted AArch64 state/continuation,
  not evidence of compiler alteration of the static assembly.
- `x0`-`x3` are outside the general allocator, but neither blindly clobbering
  nor blindly restoring them defines the TU contract.
- `x4`-`x11` can all be live in complex effective-address generation.
- `x12`, pinned guest GPRs, `x18`, `x30`, `v28` and `v30` have known special
  roles documented by earlier Emu68 integration issues and must be handled
  according to the real TU entry/exit convention.
- Overlay/remap invalidation and continuation safety must be verified together.

## Validation rules

The harness is a behavioral oracle only. It cannot validate the API. A valid
result requires a real Emu68 guest under QEMU or hardware and an observable
guest milestone.

The KS1.3 regression captures the framebuffer and rejects flat grey, yellow
fatal and otherwise blank output. Frame counters alone are not success. For
AROS, the first accepted milestone is serial output produced by the emulated
Amiga guest; early host-side serial text does not count. DiagROM may be used to
isolate CPU/register behavior but does not replace the KS1.3 regression.

Current results:

- machine API unit suite: PASS;
- multicore Emu68 cross build: PASS;
- static native-bridge disassembly: PASS;
- KS1.3 explicit API boot: FAIL;
- AROS guest serial milestone: not reached.

Do not state that the public API is complete, fault-independent in practice, or
ready for dependent implementation until those runtime facts change.
