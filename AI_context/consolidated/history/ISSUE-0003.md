---
id: ISSUE-0003
title: "Public Emu68 bus/MMIO callback API"
status: superseded
priority: medium
type: feature
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - emu68
  - bus
  - api
  - upstream
  - multicore
  - fault-handler
related_files:
  - emu68/src/aarch64/vectors.c
  - src/cpu/emu68/bellatrix.c
  - patches/0002-add-bellatrix-bus-hook.patch
---

# Issue: Public Emu68 bus/MMIO callback API

## Motivação ampla

O fault handler + data abort é o único ponto de intervenção disponível hoje
no Emu68. Isso precisa ser melhor entendido — e possivelmente substituído —
por duas razões independentes:

1. **Performance** (ver ISSUE-0002): o data-abort tem overhead fixo por MMIO
   e impede otimizações como flush seletivo e fast paths.
2. **Controle multicore** (ver ISSUE-0007): com o fault handler, a
   sincronização só ocorre em exceções AArch64 — fora do controle do caller.
   Uma API de callback direta permitiria inserir pontos de sincronização
   previsíveis, aproximando o Emu68 do controle que o Musashi já oferece com
   chamadas C normais.

Enquanto não existe uma API pública, o Musashi é o backend preferencial para
desenvolver e validar o modelo multicore. Esta issue é o caminho para trazer
o Emu68 ao mesmo nível de controle.

## Context

Bellatrix currently integrates Emu68 MMIO by letting the JIT execute a memory
access into an unmapped/protected region, then handling the resulting AArch64
data abort in `emu68/src/aarch64/vectors.c`. The fault handler saves live JIT
state, calls Bellatrix bus code, restores state, and resumes the guest.

This works, but it is an indirect integration contract:

- Bellatrix depends on exception-path details and live AArch64 register
  preservation.
- Every MMIO access pays data-abort entry/exit overhead.
- The Emu68 patch must know about Bellatrix-specific symbols.
- It is harder to reason about timing because a normal load/store becomes an
  out-of-band callback only after faulting.

Recent profiling shows the data-abort overhead itself is not the largest cost
right now (~400 CNT cycles versus thousands in dispatch/advance), so this is
not the first performance fix to implement. It is still a good architecture
target for a mature Emu68 backend.

## Goal

Add a small public Emu68 API that lets an embedding runtime register external
bus/MMIO callbacks for address ranges. Emu68 would call the registered backend
directly for those ranges instead of relying on MMU faults.

The API should be generic enough for other Emu68 hosts, not Bellatrix-only.

## Proposed API shape

Minimal callback registration:

```c
typedef uint32_t (*emu68_bus_read_fn)(void *opaque,
                                      uint32_t addr,
                                      unsigned size);

typedef void (*emu68_bus_write_fn)(void *opaque,
                                   uint32_t addr,
                                   uint32_t value,
                                   unsigned size);

typedef struct Emu68BusCallbacks {
    void               *opaque;
    emu68_bus_read_fn   read;
    emu68_bus_write_fn  write;
} Emu68BusCallbacks;

void emu68_set_bus_callbacks(const Emu68BusCallbacks *callbacks);
```

Range-aware variant:

```c
typedef struct Emu68BusRange {
    uint32_t base;
    uint32_t size;
    uint32_t flags;
    Emu68BusCallbacks callbacks;
} Emu68BusRange;

int emu68_register_bus_range(const Emu68BusRange *range);
```

Useful flags:

```c
EMU68_BUS_READ
EMU68_BUS_WRITE
EMU68_BUS_SIDE_EFFECTS
EMU68_BUS_BIG_ENDIAN
EMU68_BUS_M68K_24BIT
EMU68_BUS_NO_INLINE_CACHE
```

Bellatrix would register:

- `0x00dff000-0x00dfffff` custom chip MMIO;
- `0x00bfe000-0x00bfefff` CIA-A;
- `0x00bfd000-0x00bfdfff` CIA-B;
- autoconfig window `0x00e80000-0x00e8ffff`;
- expansion windows as they are configured.

## JIT behavior

For a memory access whose effective address is known to be in a registered
range, the JIT should emit a direct helper call:

```text
guest load/store
  -> generated helper call
  -> registered host callback
  -> return value / continue
```

For unknown or normal RAM accesses, keep the current fast native memory path.

Important constraints:

- The callback path must preserve Emu68 live state exactly like the current
  fault handler does.
- The JIT must not incorrectly cache side-effecting reads.
- The callback must receive the actual M68K PC or enough context for logging
  and timing diagnostics.
- Access size and byte-lane semantics must match the current Emu68 memory
  model.
- Unsupported sizes should either be split by Emu68 or rejected explicitly.

## Bellatrix adapter shape

Bellatrix should keep Emu68-specific code behind a narrow adapter:

```c
uint32_t bellatrix_emu68_bus_read(void *opaque, uint32_t addr, unsigned size);
void     bellatrix_emu68_bus_write(void *opaque, uint32_t addr, uint32_t value, unsigned size);
void     bellatrix_emu68_set_pc(uint32_t pc);
```

The callbacks should call the same internal policy layer planned for the
current fault path:

- accumulate/report JIT progress;
- apply MMIO flush policy;
- use public Rigel APIs for fast paths where safe;
- fall back to `machine_dispatch_read/write`;
- preserve keyboard, CIA, IRQ and expansion side effects.

This avoids having two separate bus implementations.

## Why this is not the first fix

The latest profiling suggests:

```text
fault_overhead_estimate ~400 CNT cycles
dispatch/read path      ~5K-6K CNT cycles
advance_time            ~5K CNT cycles
```

Replacing data aborts with direct callbacks would remove the ~400-cycle
component, but the backend would still be slower than Musashi if Bellatrix
keeps flushing Rigel and doing full generic dispatch on every hot MMIO.

Recommended order:

1. Coalesce CPU progress and reduce `advance_time.calls`. (ISSUE-0002)
2. Add explicit MMIO flush policy. (ISSUE-0002)
3. Add Bellatrix-side fast paths using Rigel public APIs. (ISSUE-0002)
4. Measure again.
5. If fault overhead remains material, implement the Emu68 public callback API. (this issue)

## Migration plan

Stage 1: keep the current data-abort path, but route it through the same
`bellatrix_emu68_bus_read/write` adapter that a future public API would use.

Stage 2: add public callback types and registration in Emu68 without changing
the JIT. Provide a fallback call site from the existing fault handler.

Stage 3: teach Emu68's memory/JIT layer to recognize registered ranges and
emit direct helper calls.

Stage 4: remove Bellatrix-specific code from `vectors.c`, leaving only generic
Emu68 callback machinery.

Stage 5: benchmark:

- direct callback versus data-abort path for DFF006 polling;
- callback overhead with and without Rigel flush;
- Emu68 versus Musashi boot throughput;
- correctness under IRQ, keyboard, autoconfig and frame rendering.

## Open questions

- Does Emu68's JIT have enough range information at codegen time, or does it
  need a runtime helper for all non-RAM accesses?
- Can registered ranges change dynamically after autoconfig, and if so, how
  should JIT code invalidate cached address decisions?
- Should callbacks operate on 24-bit Amiga addresses only, or full 32-bit M68K
  addresses before normalization?
- How should the API expose the current guest PC without forcing every callback
  to know AArch64 register conventions?
- Can side-effecting MMIO helpers be inlined safely, or should they always be
  no-cache/no-inline-call barriers?

## Success criteria

- Bellatrix no longer needs Bellatrix-specific logic in Emu68 exception vectors
  for ordinary MMIO.
- Direct callback MMIO is measurably cheaper than data-abort MMIO.
- Callback path preserves all existing side effects and logging context.
- The API is generic enough to be acceptable upstream or maintainable as a
  small host-extension patch.
