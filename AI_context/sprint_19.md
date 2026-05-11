# Sprint 19 — v30 regression fix + boot diagnostic logging

## Root cause of regression (AROS/DiagROM no CIA writes after CACR_IE fix)

### Symptom

After sprint 18 added `__m68k.CACR = BE32(CACR_IE)` (enabling JIT cached mode),
the system appeared to freeze: `[VEC-W]` vector-table writes appeared in the log
but no `[CIAA-W]`, `[CIAB-W]`, `[INTENA-W]`, or `[DMACON-W]` ever followed.
M68K execution stopped after a few vector writes.

### Root cause: v30 (JIT instruction counter) corrupted by kprintf in bus fault handlers

**v30 (V30.d[0])** is the JIT instruction counter. `EMIT_LocalExit` in
`M68K_Translator.c` emits `vadd_2d(30,30,0)` at every JIT block exit to
accumulate the instruction count. The BELLATRIX ExecutionLoop block reads v30
each while-loop iteration to compute `bela_delta` and call
`bellatrix_machine_advance(bela_delta * 8)`.

**v30 is a caller-saved AArch64 SIMD register** (V16–V31 are caller-saved per
the AArch64 ABI). Every `kprintf` call is free to clobber it.

`SYSWriteValToAddr` / `SYSReadValFromAddr` (the Emu68 fault handlers) already
saved/restored **x18** (M68K PC) around `bellatrix_bus_access`, but did NOT
save/restore v30. Every bus fault that invoked `kprintf` (e.g., `[VEC-W]`,
`[CIAA-W]`, `[BUS...]`) corrupted v30.

**Without CACR_IE** (uncached mode): `EMIT_LocalExit` was never reached in the
hot path; v30 never changed; `bela_delta = 0` always; `bellatrix_machine_advance`
was never called from ExecutionLoop. Safe.

**With CACR_IE** (cached mode, sprint 18 fix): JIT blocks compile and exit via
`EMIT_LocalExit` → vadd_2d emitted → v30 accumulates. But the very next bus
fault zeroes v30 (kprintf in bellatrix_bus_access clobbers it). On the next
while-loop iteration:

    bela_delta = (uint32_t)(v30_after_kprintf_clobber - bela_insn_prev)
               = (uint32_t)(0 - N_accumulated)
               ≈ 2^32 - N  (huge!)

`bellatrix_machine_advance(2^32 * 8)` → `machine_step_components(m, ~2^32)` →
`agnus_step` loops for billions of ticks. CPU never returns to M68K execution.
System freezes after vector writes (the last bus accesses before the freeze).

### Fix

Added v30 save/restore in both `SYSWriteValToAddr` and `SYSReadValFromAddr` in
`emu68/src/aarch64/vectors.c`:

```c
uint64_t _x18_save, _v30_save;
asm volatile("mov %0, x18"     : "=r"(_x18_save));
asm volatile("mov %0, v30.d[0]": "=r"(_v30_save));
g_bellatrix_fault_pc = (uint32_t)_x18_save;
bellatrix_bus_access(...);
asm volatile("mov v30.d[0], %0" :: "r"(_v30_save));
asm volatile("mov x18, %0"      :: "r"(_x18_save));
```

Patch `0002-add-bellatrix-bus-hook.patch` regenerated to include this fix.

## Diagnostic logging added

To capture future boot regressions early:

- **`[BUS000]–[BUS119]`** in `bellatrix_bus_access`: logs every bus access for the
  first 120 calls unconditionally. Shows exact order of M68K bus faults at boot.
- **`[INTENA-W]`** in `machine_dispatch_write`: direct kprintf when INTENA (DFF09A)
  is written, showing fault_pc and resulting intena value.
- (DMACON already had `[DMACON-W]` in agnus.c)

These complement the existing `[VEC-W]`, `[CIAA-W]`, `[CIAB-W]`, `[OVL-TRIG]`
kprintfs already in place.

## Files modified

- `emu68/src/aarch64/vectors.c`: v30 save/restore in SYSWriteValToAddr +
  SYSReadValFromAddr (BELLATRIX section)
- `patches/0002-add-bellatrix-bus-hook.patch`: regenerated with v30 fix
- `src/cpu/bellatrix.c`: [BUS000–119] first-N-accesses log in bellatrix_bus_access
- `src/core/machine.c`: [INTENA-W] kprintf in machine_dispatch_write

Build verified green. Patches detected as already applied by setup.sh.

## Next steps

1. Flash new binary and boot
2. Confirm CIA writes now reach machine_dispatch_write (expect `[CIAA-W]` and
   `[CIAB-W]` in serial output for DiagROM boot)
3. Confirm `exec_pc` is now non-zero (CACR_IE enables JIT cached mode, instruction
   counter now accumulates correctly)
4. Once boot is stable, remove or reduce the `[BUS000–119]` noise (or make it
   conditional on a compile-time flag)
5. Resume DiagROM boot progress diagnosis (CIA-A OVL toggle, INTENA/DMACON setup)

## Emu68 integration follow-up: bridge + memory unification

### Summary

Work continued on the Bellatrix/Emu68 integration with the explicit goal of:

- keeping the harness working
- minimizing Emu68 changes
- treating Emu68 RAM as the authoritative backing on the real target

### What was implemented

1. **Bridge layer introduced**

Added a shared CPU/machine bridge in:

- `src/bridge/bellatrix_bridge.h`
- `src/bridge/bellatrix_bridge.c`

This centralizes:

- CPU bus read/write access
- address normalization
- CPU progress -> machine time advancement
- CPU IRQ/IPL synchronization

`emu68` hooks were updated to use the bridge:

- `emu68/src/aarch64/vectors.c`
- `emu68/src/ExecutionLoop.c`

The harness was kept on the same logical contract.

2. **Autoconfig Z2 groundwork added**

Implemented a Bellatrix-side Z2 Autoconfig MVP in:

- `src/core/memory/autoconfig.c`
- `src/core/memory/autoconfig.h`

Important details:

- disabled by default so the harness does not change behavior
- enabled explicitly from the Emu68 Bellatrix target
- aligned with AROS `ReadExpansionByte` / `WriteExpansionByte` nibble-wide
  physical layout
- assign logic updated for the Z2 path (`0xe80048`)

Also forced the Emu68 autoconfig window (`0x00e80000..0x00e8ffff`) to fault
through the Bellatrix hook instead of being silently satisfied by the global
identity RAM map.

3. **Critical Fast RAM integration bug found and fixed**

The main AROS failure was not Autoconfig yet. The critical bug was a mismatch
between:

- what the Emu68 ICache/JIT fetch path was reading
- what Bellatrix thought existed in Fast RAM

Trace result:

- Emu68 `ICACHE` fetch at `0x002e0014` saw `0xffff`
- Bellatrix Fast RAM read at the same logical address saw `0x0000`

Root cause:

- Bellatrix was already pointing `memory.fast_ram` at Emu68's real RAM backing
- but `fast_ram.c` indexed it incorrectly using the absolute 68k address mask
- for example, `0x002e0014` was treated like offset `0x2e0014`
- the correct offset inside Z2 Fast RAM is `0x002e0014 - 0x00200000 = 0x0e0014`

Fix:

- `src/core/memory/fast_ram.c`

`fast_addr()` now subtracts `BELLATRIX_FAST_RAM_BASE` before masking:

```c
return (addr - BELLATRIX_FAST_RAM_BASE) & m->fast_ram_mask;
```

This matches the intended design: **Bellatrix consumes Emu68 memory as the
source of truth**, not the other way around.

### Current status

- Harness still passes: `ctest --test-dir out/harness --output-on-failure`
- Emu68 Bellatrix image rebuild succeeded: `cmake --build emu68/build-bellatrix-codex -j4`
- `./run.sh qemu` was **not** run in this session

### Next steps

1. Boot the new `Emu68.img` on target and confirm whether AROS still jumps to
   `0x002e0014`
2. If it still fails, compare the new Bellatrix Fast RAM reads with the Emu68
   ICache trace to confirm both now see the same contents
3. Once memory coherence is confirmed, return to the earliest remaining boot
   divergence before AROS prints its first visible messages

## Emu68 follow-up: Fast RAM aliasing and current boot state

### Additional findings

Further target testing showed that the first Fast RAM integration attempt was
still observing the wrong Emu68 RAM view.

Observed sequence:

- Emu68 `ICACHE` initially saw `0xffff` in the `0x002e0014` area
- Bellatrix Fast RAM reads still saw `0x0000`
- fixing the Fast RAM offset math in `src/core/memory/fast_ram.c` was correct,
  but not sufficient

The remaining issue was the RAM alias used by Bellatrix on the real Emu68
target.

### Changes made

1. **Fast RAM offset fix retained**

Bellatrix Fast RAM accessors now subtract `BELLATRIX_FAST_RAM_BASE` before
masking, so logical address `0x002e0014` maps to offset `0x000e0014` inside
the Fast RAM block.

2. **Bellatrix Fast RAM switched to Emu68 low RAM alias**

In `src/cpu/bellatrix.c`, Bellatrix Fast RAM now points to the low
identity-mapped Emu68 alias (`0x00200000`) instead of the high
`0xffffff9000200000` alias.

3. **Fast RAM no longer trap-mapped**

The Emu68 Bellatrix setup now maps `0x00200000..0x00bfffff` as normal RAM
again (`MMU_ACCESS` restored). This avoids recursive faults and matches the
intended model:

- RAM remains owned by Emu68
- Bellatrix observes the same RAM
- hooks remain reserved for MMIO / overlay / autoconfig

### Result after these changes

The failure mode changed substantially:

- Emu68 `ICACHE` no longer sees `0xffff`
- it now sees `0x0000` across the `0x002e0000` window
- the previous `opcode ffff at 002e0012 not implemented` crash disappeared
- execution now continues in Fast RAM with advancing PCs such as:
  - `0x002ed00c`
  - `0x002fe40c`
  - `0x0030f80c`

This is important because it means:

- RAM coherence improved
- the old invalid-opcode symptom is gone
- but the CPU is now executing **zero-filled Fast RAM**, which still indicates
  a wrong control-flow transition earlier in boot

### Current diagnosis

The active problem is no longer “Bellatrix and Emu68 disagree on RAM contents”.

The active problem is:

- ROM code transitions into Fast RAM too early or with a bad target
- the destination area is still zero-filled
- zero is a valid 68k opcode, so execution continues into meaningless RAM
  instead of immediately faulting

The suspicious state remains the same:

- transition from ROM into `0x002e....`
- temporary structures/registers around `0x0003f800..0x00040000`

### Validation

- `ctest --test-dir out/harness --output-on-failure`: passed (`6/6`)
- `cmake --build emu68/build-bellatrix-codex -j4`: passed
- `./run.sh qemu`: not run in this session

### Updated next steps

1. Instrument the ROM -> Fast RAM transition more directly
2. Trace the data structure or jump source around `0x0003f800..0x00040000`
3. Identify what computes or loads the first `0x002e....` execution target
4. Only after that return to Autoconfig/Z2, which appears to be later than the
   current failure point
