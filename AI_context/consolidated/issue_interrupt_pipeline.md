# Issue: Pipeline de Interrupções — INTENA/INTREQ/IPL/FIQ

## Status: CLOSED (2026-06-26)

Pipeline CIA→Paula→IPL→JIT funcional. Workbench e Happy Hand em hardware
confirmam VBL 50Hz, DSKBLK, PORTS (CIA-A keyboard) e EXTER todos operacionais.
`cia_interrupt_sync_irq_line()` implementa level-sensitive raise/clear. Risco
de sincronização cross-core CIA→INTREQ não manifestou como bug observável.
Histórico de bugs e decisões arquiteturais preservado como referência.

## Contexto

O pipeline de interrupções do Amiga envolve: CIA-A/B → Paula (INTREQ/INTENA) →
IPL M68K. No Bellatrix bare-metal, o VBL era gerado via ARM Generic Timer FIQ.
Vários bugs impediram que interrupções chegassem ao JIT.

## Decisões Arquiteturais

- **Paula é dona de INTREQ/INTENA** — CIAs e Agnus notificam Paula via
  `paula_irq_raise/clear`, nunca modificam diretamente.
- **CIA notifica Paula via attach** — `cia_attach_paula()` + `paula_irq_raise()`
  no `cia_raise_icr()`.
- **IPL derivado por Paula** — `paula_compute_ipl()` usando `INTREQ & INTENA & MASTER`.
- **VBL a 50Hz** — ARM Generic Timer (CNTV) no Core 0 (single-core) ou via
  `agnus_step()` pelo counter de beam (raster-time).
- **`__m68k_state`** — ponteiro global de Emu68 para acessar `M68KState.INT.IPL`.

## Componentes e Responsabilidades

| Componente | Responsabilidade |
|------------|-----------------|
| CIA-A | Gera PORTS (bit 3) → `paula_irq_raise(PAULA_INT_PORTS)` |
| CIA-B | Gera EXTER (bit 13) → `paula_irq_raise(PAULA_INT_EXTER)` |
| Agnus | Gera VERTB (bit 5), DSKBLK (bit 1), BLIT (bit 6) → via Paula |
| Paula | Consolida INTREQ & INTENA → `paula_compute_ipl()` → `bellatrix_machine_sync_ipl()` |
| PAL_IPL_Set | `__m68k_state->INT.IPL = level; __m68k_state->INT.ARM = level; DMB` |
| ExecutionLoop | Label 9: checa `INT.ARM`, compara com `INT.IPL`, injeta exceção |

## Pipeline Completo (Sprint 29 — Final)

### Interrupções CIA → CPU (multicore):
```
Core 3: cia_step() → timer underflow → CIA ICR set
  (próximo ciclo de Core 2)
Core 2: paula_interrupt_update() → lê CIA ICR → INTREQ set
Core 2: bellatrix_machine_sync_ipl() → paula_compute_ipl() → PAL_IPL_Set()
  ↓
Core 0: JIT ExecutionLoop label 9 → INT.ARM > 0 → exceção M68K
```

### VBL (Core 1 / Agnus beam):
```
Core 1: agnus_step() → beam vpos == 312 → agnus_intreq_set(PAULA_INT_VERTB)
     → paula_irq_raise(PAULA_INT_VERTB) → INTREQ bit 5 set
Core 2: paula_compute_ipl() → nível 3 se INTENA[VERTB|MASTER] set
Core 2: PAL_IPL_Set(3) → M68KState.INT.IPL=3, ARM=3
Core 0: JIT → exceção nível 3
```

## Bugs Históricos

### Sprint 05: `INT.ARM = 1` em vez do nível IPL
**Problema**: `PAL_IPL_Set` setava `INT.ARM = 1` sempre. Label 9 do ExecutionLoop
compara `w10 (INT.ARM) cmp w1 (INT.IPL)` e seleciona o maior. Com `ARM=1` e `IPL=3`,
o sistema nunca promovia nível 3.
**Fix**: `ctx->INT.ARM = ipl_level` (valor correto do nível).

### Sprint 08: `TPIDRRO_EL0` null durante init
**Problema**: `PAL_ChipsetTimer_Init()` capturava o ponteiro M68K via `TPIDRRO_EL0`
durante `bellatrix_init()`, que roda antes de `M68K_StartEmu()`. Registro não
setado ainda → `ctx = NULL` → `PAL_IPL_Set` retornava sem fazer nada.
**Fix**: usar `__m68k_state` (global setado antes de `MainLoop()`).
Ver `issue_emu68_jit_integration.md`.

### Sprint 10: UART flood bloqueando sistema
**Problema**: kprintfs de alta frequência (cia_tod_increment, agnus_compute_ipl,
cia_raise_icr — dezenas/milhares por segundo) saturavam UART 115200 baud.
**Fix**: todos os kprintfs de alta frequência removidos. Mantidos apenas logs de
baixa frequência (init, transições explícitas).

### Sprint 14: CIA-B TOD clocked incorretamente
**Problema**: `CIA_B_TOD_TICKS_PER_INCREMENT` = 454 (HSync). CIA-B TOD é clocked
pelo sinal /INDEX do floppy drive; sem drive, não deve incrementar.
**Fix**: valor corrigido para 0 (incremento só via /INDEX do floppy).

### Sprint 16: UART TBE interrupt espúrio
**Problema**: `uart_raise_irq(u, UART_INTREQ_TBE)` sendo chamado em instant-TX
mode, injetando bit 0 em INTREQ mesmo com SERDATR sempre reportando TBE=1.
Causava spurious IPL=1 durante boot.
**Fix**: `uart_raise_irq` comentado em instant-TX mode.

### Sprint 16: `irq_line_level` para PORTS level-sensitive
**Contexto**: INTREQ bit PORTS (CIA-A) é level-sensitive enquanto outros são
edge-latched. Campo `irq_line_level` adicionado a `Paula`.
**Status Sprint 16**: infrastructure adicionada, não conectada em `paula_compute_ipl`.
**Status atual**: conectada via `paula_irq_raise/clear` — CIA-A ICR deve manter PORTS
ativo enquanto qualquer interrupt CIA-A estiver pendente.

## `PAULA_INT_DSKSYN` — Bug de Bit (Sprint 14)

Bit 12 (DSKSYN) não tinha nível mapeado em `paula_compute_ipl`. Adicionado ao nível 6
junto com EXTER. Sem este fix, DSKSYN nunca gerava IPL.

## Execução FIQ (Sprints 05 — substituído pelo raster-time)

O handler FIQ original (ARM Generic Timer a 50Hz) foi substituído pelo modelo
raster-time em que `agnus_step()` detecta fim de frame e dispara VERTB. O timer FIQ
ainda pode ser relevante para single-core (legacy) mas no modelo multicore o VBL
vem da progressão de beam em Core 1.

## Estado Atual

### O que Funciona
- Pipeline completo CIA → Paula → IPL → JIT
- VERTB a 50Hz via raster-time (agnus_step)
- DSKBLK via paula_disk
- Paula como dona autoritativa de INTREQ/INTENA
- `paula_compute_ipl` com todos os bits mapeados por nível

### Pendente / Risco
- **Sincronização cross-core CIA→INTREQ**: Core 3 (CIA step) e Core 2 (Paula update)
  são domínios independentes. A propagação CIA ICR → INTREQ depende de ordenação
  temporal não garantida. `RuntimeMailbox` ou evento atômico ainda não implementados.
- **`irq_line_level` para EXTER**: nível do slot de expansão (CIA-B) analogamente
  ao PORTS. Verificar se está corretamente level-sensitive.

## Arquivos Relevantes
- `src/chipset/paula/paula.h/.c` — dona de INTREQ/INTENA, `paula_compute_ipl`
- `src/chipset/paula/paula_interrupt.h/.c` — engine de IRQ
- `src/chipset/cia/cia.c` — `cia_raise_icr` → `paula_irq_raise`
- `src/chipset/agnus/agnus.c` — `agnus_intreq_set` → `paula_irq_raise`
- `src/host/raspi3/pal_ipl.c` — `PAL_IPL_Set` via `__m68k_state`
- `src/core/machine.c` — `bellatrix_machine_sync_ipl`
- `src/runtime/core_audio.c` — Core 2: `paula_interrupt_update` + `bellatrix_machine_sync_ipl`
