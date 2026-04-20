// AI_context/sprint_11.md

# Sprint 11 — Refactoring estrutural: arquitectura canónica

## Status: concluído — build verde

---

## Objetivo

Migrar o codebase de `src/` para o modelo arquitectural canónico definido em
`docs/roadmap.md` e `docs/timing_and_architetura.md`:

* chipset é dono do tempo observável
* machine integra; não é scheduler soberano
* Paula é dona de INTREQ/INTENA
* DMA pertence ao Agnus
* Denise é instância explícita, não singleton
* CIA e Agnus notificam Paula via `paula_irq_raise`
* `bellatrix.c` consome a machine nova, sem glue paralelo

---

## Mudanças implementadas

### `cia.h`
* Adicionado `CIA_ID` enum (`CIA_PORT_A`, `CIA_PORT_B`)
* `CIA_State` mantido; adicionado `typedef CIA_State CIA`
* Novos campos no struct: `irq_level`, `paula_irq_bit`, `struct Paula *paula`
* Assinatura alterada: `cia_init(CIA*, CIA_ID)`
* Novas funções: `cia_reset`, `cia_compute_ipl`, `cia_attach_paula`

### `cia.c`
* `cia_init` configura `irq_level` (2/6) e `paula_irq_bit` (PORTS/EXTER)
* `cia_raise_icr` notifica Paula via `paula_irq_raise` quando ICR dispara
* `cia_read_reg` (ICR read) faz `paula_irq_clear` após ACK do CPU
* Adicionados: `cia_reset`, `cia_compute_ipl`, `cia_attach_paula`

### `agnus.h`
* Removidos `intena`, `intreq` (movidos para Paula)
* Removidos `INT_*` e `agnus_compute_ipl` (movidos para Paula)
* Adicionado `typedef AgnusState Agnus`
* Novos campos: `struct Denise *denise`, `struct Paula *paula`
* Novas funções: `agnus_reset`, `agnus_attach_denise/paula`,
  `agnus_handles_read/write`, `agnus_read/write`
* `agnus_intreq_set/clear` mantidas — agora fazem forward para Paula

### `agnus.c`
* `agnus_intreq_set/clear` → delegam para `paula_irq_raise/clear`
* `agnus_step` VBL → `agnus_intreq_set(s, PAULA_INT_VERTB)` + `denise_render_frame(s->denise, s)`
* `agnus_write_reg` → Denise via `denise_write_reg(s->denise, ...)`;
  INTENA/INTREQ via `paula_write(s->paula, ...)`
* `agnus_read_reg` → removidos INTENAR/INTREQR (Paula os trata)
* Adicionados: `agnus_reset`, attach functions, `agnus_handles_read/write`, `agnus_read/write`

### `denise.h` + `denise.c`
* Convertido de singleton estático para struct `Denise` instanciável
* Campos: `bplcon0/1/2`, `bpl1mod`, `bpl2mod`, `palette[32]`, `agnus*`
* API nova: `denise_init(Denise*)`, `denise_reset`, `denise_step`,
  `denise_attach_agnus`, `denise_handles_read/write`, `denise_read/write`
* `denise_write_reg(Denise*, reg, value)` — registador de baixo nível (usado por Agnus/Copper)
* `denise_render_frame(Denise*, const AgnusState*)` — assinatura actualizada

### `paula.h` + `paula.c`
* Paula nova e correcta: dona de INTREQ/INTENA
* `paula_irq_raise/clear` — interface para CIA e Agnus
* `paula_compute_ipl` — calcula IPL com base em INTREQ & INTENA
* `paula_step` — no-op (audio DMA reservado para fase futura)
* `paula_handles_read/write`, `paula_read/write` — protocolo de barramento
* `paula_attach_agnus/cia_a/cia_b` — wiring (stubs; CIA/Agnus notificam via raise)
* `PAULA_INT_MASTER` adicionado ao enum

### `machine.c`
* `bellatrix_machine_init` usa `cia_init(cia, CIA_PORT_A/B)` e chama todos os `attach_*`
* `machine_compute_ipl` usa `paula_compute_ipl` + `cia_compute_ipl` (cia_a e cia_b)
* `machine_step_components` chama `denise_step`
* `bellatrix_machine_advance` substitui `machine_step` como API pública
* Limpeza: `machine_sync_before_bus` simplificada (branches duplicados removidos)

### `bellatrix.c`
* Removido double-init: `denise_init()`, `agnus_init`, `cia_init` extras
* `machine_step(m, ...)` → `bellatrix_machine_advance((uint32_t)m68k_cycles)`
* `bellatrix_cpu_step` → `bellatrix_machine_advance`
* `update_ipl()` → `bellatrix_machine_sync_ipl()` (CIA já notifica Paula via attach)
* Denise write no bus handler retido mas CIA-A OVL override mantido

### `blitter.c`
* `INT_BLIT` → `PAULA_INT_BLIT` (inclui `chipset/paula/paula.h`)

### `emu68/CMakeLists.txt`
* Adicionados `src/chipset/paula/paula.c` e `src/chipset/agnus/dma.c`

### `CLAUDE.md`
* Actualizado: estrutura de directórios, princípios arquitectónicos,
  referências de documentação (roadmap.md, timing_and_architetura.md)

---

## Critério de aderência verificado

Módulo a módulo, segundo `docs/roadmap.md`:

| Critério                             | Estado |
|--------------------------------------|--------|
| CIA usa tipo explícito `CIA`         | ✅     |
| Agnus usa tipo `Agnus`               | ✅     |
| Denise instância explícita           | ✅     |
| Paula dona de INTREQ/INTENA          | ✅     |
| DMA pertence ao Agnus                | ✅     |
| Copper subordinado ao Agnus          | ✅     |
| `bellatrix.c` sem glue paralelo      | ✅     |
| Sem singleton implícito              | ✅     |
| Build verde                          | ✅     |

---

## Build

`[100%] Built target Emu68.elf` — build verde sem warnings adicionais.

---

## Próxima sessão

1. Flash e boot real — verificar `[VBL] frame=1` no serial
2. Se VBL não aparecer: verificar se `PAL_Runtime_Poll` está a ser chamado
3. "Pau de Cego": pintar framebuffer sólido em `bellatrix_init` para confirmar
   que VC4 está a receber o que escrevemos
4. Paula: adicionar stubs para ADKCON, POTGO, SERPER (geram ruído btrace)
5. Avaliar se `cia_compute_ipl` no `machine_compute_ipl` continua necessário
   após o caminho CIA→Paula via `cia_attach_paula` estar activo
