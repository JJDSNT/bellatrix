---
id: ISSUE-0001
title: "Emu68 MMIO performance profiling"
status: done
priority: high
type: research
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - emu68
  - performance
  - profiling
  - mmio
related_files:
  - src/cpu/emu68/bellatrix_profile.h
  - src/cpu/emu68/bellatrix_profile.c
  - src/cpu/emu68/bellatrix.c
  - src/cpu/cpu_bridge.c
  - cmake/bellatrix-variant.cmake
  - scripts/build.sh
---

# Issue: Performance Profiling — Emu68 MMIO overhead

## Contexto

Emu68 (JIT m68k→AArch64) deveria ser mais rápido que Musashi (interpretador C), mas na
prática o Emu68 está mais lento. O custo extra vem da integração: todo acesso MMIO no Emu68
passa por um data-abort AArch64 → `bellatrix_bus_access` com save/restore de contexto de
exceção. Musashi chama as funções de leitura/escrita diretamente como chamadas C normais.

## Status: PROFILING FUNCIONANDO — dados coletados do hardware real (Jun 2026)

---

## Infraestrutura implementada

### Arquivos
- `src/cpu/emu68/bellatrix_profile.h` — tipos e macros (BellatrixProfile, BPROF_CONTROL_ADDR=0xDFFF04)
- `src/cpu/emu68/bellatrix_profile.c` — bprof_record, bprof_hot_record, bellatrix_profile_dump/reset
- `src/cpu/emu68/bellatrix.c` — instrumentação em bellatrix_bus_access + avanço single-core
- `src/cpu/cpu_bridge.c` — instrumentação em bellatrix_bridge_cpu_read/write (referência bridge_ref)
- `cmake/bellatrix-variant.cmake` — option(BELLATRIX_PROFILE) + add_compile_definitions
- `scripts/build.sh` — BELLATRIX_PROFILE=1 → -DBELLATRIX_PROFILE=ON

### Build de profiling
```bash
# Emu68 JIT
BELLATRIX_PROFILE=1 BELLATRIX_USBSTACK=1 ./scripts/build.sh clean
# Musashi (referência)
BELLATRIX_PROFILE=1 BELLATRIX_USBSTACK=1 BELLATRIX_CPU_BACKEND=musashi ./scripts/build.sh clean
```

### Ativação em runtime
- Auto-dump a cada 100K acessos (BPROF_AUTODUMP_INTERVAL=100000)
- Escrever 0x01 em 0xDFFF04 → dump imediato
- Escrever 0x02 em 0xDFFF04 → reset contadores

---

## Bugs encontrados e corrigidos durante o sprint

### Bug 1: vectors.c chamava bellatrix_bridge_cpu_access, não bellatrix_bus_access
**Sintoma:** `total_access.calls=0` mas `bridge_ref_read.calls` incrementava no build Emu68.
**Causa:** O fault handler foi modificado em sprint anterior para chamar `bellatrix_bridge_cpu_access`
diretamente, bypassando `bellatrix_bus_access` (que contém profiling, PAL_Runtime_Poll, etc).
**Fix:** `emu68/src/aarch64/vectors.c` → chamadas para `bellatrix_bus_access(far, value, size, BUS_WRITE/READ)`.
Patch 0002 atualizado.

### Bug 2: BELLATRIX_PROFILE via CMAKE_C_FLAGS não propagava
**Causa:** `-DCMAKE_C_FLAGS=` pode ser sobrescrito pelo CMakeLists.txt ou toolchain.
**Fix:** Convertido para `option(BELLATRIX_PROFILE)` + `add_compile_definitions` em `bellatrix-variant.cmake`.

### Bug 3: EMU68_BOARDS_MODE default errado
**Causa:** Default era `boards` mas suporte a boards ainda não implementado.
**Fix:** Default alterado para `legacy` em `build.sh`.

### Bug 4: Build sem tag de identificação
**Fix:** `kprintf("[BELA] build: " __DATE__ " " __TIME__ "\n")` em `bellatrix_init()`.

---

## Resultados do hardware (Emu68 JIT, ~1M acessos, Kickstart 3.1 sem disco)

### Breakdown por bus access (avg ~79 cy = 4.1 µs @ 19.2 MHz CNTFRQ)

| Componente | Ciclos | % | Nota |
|---|---|---|---|
| `disp_read` (machine_read) | ~70 cy | 87% | **bottleneck principal** |
| `poll` (PAL_Runtime_Poll) | ~3 cy | 4% | barato |
| `addr_fix` (normalize) | ~0 cy | 1% | irrelevante |
| `lock_wait` | ~0 cy | 0% | sem contenção single-core |
| `fault_ovhd` (derivado) | ~5 cy | 6% | não instrumentado diretamente |

**Overhead AArch64 data-abort vs Musashi C-call direto: ~34 cy (1.7 µs)**
- `total_access_avg` ≈ 79 cy
- `bridge_ref_read_avg` ≈ 45 cy
- diferença = **34 cy** de overhead por fault

### Hot MMIO (estabilizado após 1M+ acessos)
| Endereço | Hits | Registro |
|---|---|---|
| DFF006 | ~290K | DMACONR — Kickstart polling DMA em loop |
| BFE801 | ~178K | CIA-A keyboard (hot register) |
| DFF004 | ~640 | VPOSR (vertical position) |
| DFF09A | ~210 | INTREQR |
| DFF01C | ~133 | INTENA |

### Referência Musashi (mesmo hardware)
- `bridge_ref_read avg` ≈ 45 cy (2.4 µs) — sem overhead de fault
- `advance_time avg` ≈ 8706 cy (453 µs) — custo de bellatrix_machine_advance por quantum
- `avg_m68k_cycles_per_call` ≈ 460 ciclos por quantum

---

## Observações críticas

### advance_time.calls=0 no Emu68
O caminho Emu68 nao passa pelo publisher generico `bellatrix_bridge_publish_cpu_cycles`.
O contador `v30.d[0]`
(JIT instruction counter) não está sendo incrementado pelo JIT. O chipset avança via outro
mecanismo (provavelmente timer HW em PAL_Runtime_Poll). Isso implica que a relação
M68K-cycles → chipset-cycles está **desacoplada** no Emu68 atual. Investigar.

### DFF006 domina (29% de todos os acessos)
Kickstart em busy-wait por DMA completar. Cache do resultado de DMACONR (se raramente muda)
pode reduzir 29% dos acessos MMIO.

### machine_read é o bottleneck real (87% do tempo)
O overhead do fault (~34 cy) é relevante mas secundário. O custo de `bellatrix_machine_read`
em si (~70 cy) é onde está o problema. Otimizar machine_read tem maior impacto.

---

## Próximos passos (ver ISSUE-0002)

1. **Investigar advance_time=0:** Por que v30.d[0] não é incrementado? O chipset avança via
   timer IRQ? Medir impacto do desacoplamento M68K timing / chipset timing.
2. **Otimizar machine_read para DFF006:** Cache de DMACONR pode eliminar 29% dos acessos.
3. **Comparar Musashi completo:** Rodar profiling Musashi com mesmo kickstart para baseline
   apples-to-apples (advance_time já medido, mas total_access não).
4. **Medir throughput de M68K cycles/s:** Comparar Emu68 vs Musashi em cycles por segundo
   wall-clock para quantificar a diferença total de performance.

---

## Update: integração Emu68 JIT progress + MMIO poll coalescido

Log posterior em single-core mostrou outro perfil: `PAL_Runtime_Poll()` passou a custar
~2K-4K CNT cycles por MMIO (~58-64% de `bellatrix_bus_access`) e `advance_time.calls`
continuou em zero. Isso indica que o runtime estava sendo servido por acesso MMIO, não
por progresso real do JIT.

Correção implementada:
- `bellatrix_emu68_report_jit_progress(insn_count, pc)` em `src/cpu/emu68/bellatrix.c`
  centraliza o delta do contador `v30.d[0]` e chama o avanço direto
  `bellatrix_singlecore_advance_cpu_cycles()`.
- `emu68/src/aarch64/vectors.c` chama esse helper antes de cada MMIO fault.
- Quando `v30.d[0]` ainda não avançou porque o MMIO abortou antes da saída local do
  bloco JIT, o helper usa um fallback conservador de 8 ciclos por fault.
- O helper precisa chamar o avanço single-core local de `bellatrix.c`, nao
  `bellatrix_bridge_publish_cpu_cycles()`: o bridge externo publica ciclos para o
  runtime generico/multicore e nao representa o caminho sincrono usado pelo Emu68
  single-core.
- O `ExecutionLoop.c` não chama C para reportar progresso: isso é inseguro porque pode
  clobberar registradores AArch64 que mantêm estado M68K vivo.
- `bellatrix_bus_access()` não executa mais `PAL_Runtime_Poll()` completo em todo MMIO;
  usa um serviço single-core coalescido. A atualização temporal de registradores continua
  preservada porque `bellatrix_machine_read/write()` já chama `machine_flush_for_bus()`.

Validação local:
```bash
BELLATRIX_PROFILE=1 ./scripts/build.sh
```

Resultado esperado no próximo log de hardware:
- `advance_time.calls > 0`
- `poll` cai fortemente como porcentagem de `total_access`
- `avg_m68k_cycles_per_call` passa a refletir os deltas do JIT
