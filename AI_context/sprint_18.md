# Sprint 18 — CACR_IE fix + DiagROM Emu68 debug

## What we did

### 1. Harness link fix (from sprint 17 continuation)

`g_bellatrix_fault_pc` and `g_bellatrix_exec_pc` were defined in `src/cpu/bellatrix.c`
(AArch64-only build). Moved both definitions to `src/core/machine.c` (compiled by both
builds). Added `extern` declarations to `src/cpu/bellatrix.h`. Both builds now link.

### 2. CACR_IE fix — enables JIT cached mode for BELLATRIX

**Problem**: `exec_pc=00000000` always in btrace logs from the Raspberry Pi.

**Root cause found**:
- The BELLATRIX M68K_StartEmu block did NOT set `__m68k.CACR`.
- With CACR=0, `cacr & CACR_IE = 0` in ExecutionLoop.c line 339 → JIT runs in **uncached mode**.
- The BELLATRIX block in ExecutionLoop.c reads v30 instruction counter and sets
  `g_bellatrix_exec_pc` when `bela_delta > 0`. In uncached mode this SHOULD work in
  theory (M68K_SaveContext/LoadContext save/restore v30 around C calls), but the JIT
  cache fast path (`LastPC == PC → CallARMCode()` without save/restore) is unavailable,
  creating extra overhead and potentially missing some delta accumulation paths.
- More importantly: with `CACR_IE` missing, the chipset never advances properly via the
  instruction counter path, and the `enable_cache` bootarg (used to enable CACR in the
  standard Emu68 path) is NOT parsed in the BELLATRIX boot path.

**Critical discovery**: `EMU68_HOST_BIG_ENDIAN=1` in `emu68/include/config.h`.
This means Emu68 runs the AArch64 in big-endian mode (SCTLR.EE=1, SCTLR.E0E=1).
Therefore `BE32(x) = x` (identity, defined in support.h line 42). So:
- `BE32(CACR_IE) = CACR_IE = 0x00008000`
- `__m68k.CACR = 0x00008000` → `cacr & CACR_IE = 0x00008000` → cached mode! ✓

**Fix applied**:
- Added `__m68k.CACR = BE32(CACR_IE);` to BELLATRIX block in
  `emu68/src/aarch64/start.c` (after `__m68k.FPCR = 0;`).
- Updated `patches/0002-add-bellatrix-bus-hook.patch` to include this line.
- Build green after change.

## State of DiagROM in Emu68 (before CACR_IE fix)

From btrace log captured before this session:
- `fault_pc=00f800d2` always → CPU executes from ROM at 0xF800D2 (DiagROM Begin:)
- `exec_pc=00000000` always → chipset never advances (JIT instruction counter path)
- `intena=0x0000`, `dmacon=0x0000` → no chipset writes seen in VBL snapshots
- VBL fires at ~50Hz (Agnus beam working)
- DiagROM first CIA writes (CIA-A PRA=0 to disable overlay) ARE being logged

## What to test next after flashing the CACR_IE fix

1. `exec_pc` should now show a non-zero M68K PC (somewhere in the ROM or during DiagROM
   execution). If it does, JIT cached mode is working and `bellatrix_machine_advance` runs.

2. If `exec_pc` is still 0: v30 instruction counter is not accumulating. Possible causes:
   - `EMU68_INSN_COUNTER` not actually emitting vadd_2d for the ROM code path
   - JIT blocks for ROM are never completing (faulting before EMIT_LocalExit)
   - Something in the fault path resets v30 before EMIT_LocalExit

3. Check whether `intena` and `dmacon` advance after the fix (should see DiagROM writes).

## DiagROM execution flow (from analysis)

DiagROM entry point: 0xF800D2 (Begin:).
- Clears D0-D7, A0-A6, sets A7=0x400
- Writes CIA-B (0xBFD000): DDRB=0xFF, PRB=0x7F, etc.
- Writes CIA-A (0xBFE001): OVL=0 (disables ROM overlay at 0x000000)
- Writes custom chip registers: DMACON, INTENA, etc.
- Calls DumpSerial (outputs "DiagROM 1.2..." over serial)
- After DumpSerial: writes INTENA=$C000 (enable + master enable)

DumpSerial behavior:
- Checks CIA-A PRA bit 6 (LMB): with `ext_pra=0xFF`, bit 6=1 → does NOT exit early ✓
- Checks SERDATR bit 13 (TSRE): `uart_read_serdatr` returns 0x2000 → bit 13=1 → OK ✓
- Checks POTGOR bit 14: `paula_read_potgor` returns 0xFFFF → bit 14=1 → skips .aaa loop ✓

## CIA address decode (corrected)

CIA-A: odd bytes in 0xBFE000–0xBFEFFF (A0=1):
  `(addr & 1) && (addr >= 0xBFE001 && addr <= 0xBFEF01)`
CIA-B: even bytes (0xBFD000–0xBFDF00) OR even bytes (0xBFE000–0xBFEF00):
  Not (addr & 1) and in those ranges.

## Files modified in this sprint

- `emu68/src/aarch64/start.c`: +`__m68k.CACR = BE32(CACR_IE);` in BELLATRIX init block
- `patches/0002-add-bellatrix-bus-hook.patch`: updated to include CACR_IE line
- `src/core/machine.c`: defines `g_bellatrix_fault_pc` and `g_bellatrix_exec_pc`
  (moved from bellatrix.c); added `case 4:` to machine_dispatch_write fallthrough
- `src/cpu/bellatrix.h`: extern declarations for both pc globals
- `src/chipset/cia/cia.c`: removed DDRA/PRA masking, ext_pra=0xFF at reset
- `src/chipset/agnus/bitplanes.c`, `src/chipset/denise/denise.c`:
  replaced getenv/strtol with `PAL_Diag_GetEnvInt`
- `src/host/pal.h`: added `PAL_Diag_GetEnvInt` / `PAL_Diag_GetEnvBool` declarations
- `src/host/posix/pal_posix.c`: implemented both using getenv/strtol
- `src/host/raspi3/pal_debug.c`: stub implementations for bare metal
- `patches/0003-bellatrix-execution-loop.patch`: BELLATRIX block in MainLoop reading
  v30 and calling `bellatrix_machine_advance`; IPL delivery via INT.IPL

## Key references

- `emu68/include/config.h` line 33: `#define EMU68_HOST_BIG_ENDIAN 1`
- `emu68/include/support.h` line 42: `static inline uint32_t BE32(uint32_t x) { return x; }`
  (big-endian mode, no swap needed)
- `emu68/include/M68k.h` line 155: `#define CACR_IE 0x00008000`
- `emu68/src/ExecutionLoop.c` line 339: `if (likely(cacr & CACR_IE))` — cached vs uncached
- `emu68/src/aarch64/start.c` line 1528: `M68K_LoadContext` loads v30 from `ctx->INSN_COUNT`
- `emu68/src/M68k_Translator.c` `EMIT_LocalExit`: emits `vadd_2d(30,30,0)` per JIT block exit
