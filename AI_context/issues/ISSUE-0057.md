---
id: ISSUE-0057
title: "Implement the fault-independent Emu68 public machine API"
status: in_progress
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
  - src/cpu/emu68/emu68_machine_bridge.S
  - src/cpu/emu68/emu68_machine_platform.c
  - src/cpu/emu68/emu68_backend.c
  - emu68/src/ExecutionLoop.c
  - emu68/src/M68k_EA.c
---

# Current result

The public types, lifecycle, topology, synchronous/cooperative descriptors,
bounded execution, reset, stop, IPL, invalidation, JIT classifier and native
bridge exist. Unit tests pass, but the required KS1.3 runtime regression does
not. The API is therefore not complete and must not be described as validated
or ready for dependent work.

The harness remains an oracle for machine behavior. It is not validation of the
Emu68 API because it does not execute the Emu68 JIT or its explicit bridge.

# Runtime evidence

The old Bellatrix fault path, with the same API lifecycle, topology and
scheduler temporarily retained around it, reached the KS1.3 boot display at
frame 500:

```text
frame=640x480 colors=4 dominant=0.9744 rgb=(248,252,248) PASS
```

The explicit bridge did not. A finite bus trace gives the first decisive
divergence:

```text
old fault path: W dff09a[2]=00007fff
explicit path:  W dff09a[2]=00000000
                W dff09a[2]=0000f2d4
                R dff09a[2]=00000000
```

The classifier itself was isolated by making its EXTERNAL branch execute the
old native/faulting access. That configuration booted, so the failure is in the
explicit emitted-call/continuation contract rather than page classification.

# Generated-code audit

`aarch64-linux-gnu-objdump` confirms that
`emu68_machine_native_bridge` is assembled as written: its 832-byte frame saves
and restores the intended GPR, SIMD, status and TPIDR state. There is no current
evidence that the C compiler or assembler changed the static bridge semantics.

The defect is in the AArch64 sequence generated at runtime by
`emu68_machine_emit.c`, or in its assumptions about the live JIT state. The
audit found and corrected an earlier source-ordering error in which direct and
slow operations were emitted on the wrong branches. It also found that fixed
scratch operands can alias address/value operands. Dynamic scratch selection
avoids that direct alias, but KS1.3 still diverges.

Further experiments established constraints rather than a final fix:

- classification instructions modify `NZCV`; preserving it alone did not
  restore the old behavior;
- treating `x0`-`x3` as persistent state and restoring them is also wrong for
  the TU calling convention;
- requesting additional allocator registers can exhaust `x4`-`x11` in complex
  effective-address emitters;
- using `x30` as a general classifier register corrupted translated
  continuation;
- overlay/remap invalidation must not leave the currently executing translated
  continuation pointing at invalid code.

Temporary diagnostic variants that made those experiments fail were reverted;
they are not the intended API design.

# Validation state

- `tests/unit/run_emu68_machine.sh`: PASS.
- Cross build of the multicore Bellatrix/Emu68 image: PASS.
- Static bridge disassembly: matches the assembly source.
- KS1.3 explicit-API framebuffer regression: FAIL.
- AROS guest serial milestone: not reached.
- DiagROM showed guest activity but is not an API success criterion.

Completion requires the explicit path to preserve the actual Emu68 JIT/TU
contract and pass a real QEMU guest regression without falling back to Data
Abort dispatch. Produced frames, harness execution, or unit tests alone are not
sufficient.
