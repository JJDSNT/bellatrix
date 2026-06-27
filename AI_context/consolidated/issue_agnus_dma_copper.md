# Issue: Agnus, DMA Arbitration e Copper

## Status: CLOSED (2026-06-26)

LOF blocker resolvido no Rigel (`beam.lof = 0` na init, `lof_toggle = 0` para
não-interlace). Workbench e Happy Hand funcionando em hardware confirma que DMA
arbiter, Copper raster-time e bitplanes estão operacionais. Itens menores
restantes (VPOSW write handlers, BPL-DMA-FORCE removal) são refinamentos futuros,
não blockers — abrir issues específicos quando manifestarem.

## Contexto

Agnus é responsável pelo beam (posição raster), controle de DMA, Copper e Blitter.
A implementação passou de um modelo ad-hoc para um DMA arbiter centralizado com
Copper, bitplanes, blitter, disco e sprites como requesters.

## Decisões Arquiteturais

- **Agnus = dono do beam** — `beam.hpos/vpos`, LOF, VSYNC/HSYNC
- **DMA centralizado em `AgnusDMA`** — todos os subsistemas como requesters
- **Copper subordinado ao Agnus** — não é subsystem independente
- **Rendering raster-time** — agnus_step dispara Denise por linha, não por frame batch

## Arquitetura de DMA (Sprint 21 — Final)

### AgnusDMA como árbitro
```c
// Em cada agnus_step():
agnus_dma_step(&s->dma, 1);
// → query: bitplanes, copper, blitter, disco, sprites, audio
// → grant: executa uma unidade por slot
```

### Requesters
| Client | Request Mask | Service |
|--------|-------------|---------|
| Copper | `AGNUS_DMA_REQ_COPPER` | `copper_service_step()` |
| Bitplane | `AGNUS_DMA_REQ_BPL*` | `bitplanes_dma_service_next()` |
| Blitter | `AGNUS_DMA_REQ_BLITTER` | `blitter_dma_service_grant()` |
| Disco | `AGNUS_DMA_REQ_DISK` | `paula_disk_dma_service_grant()` |
| Sprite | `AGNUS_DMA_REQ_SPRITE0..7` | leitura chip RAM → Denise sprite state |

### DMACON ownership
- `AgnusDMA dma` embedded em `AgnusState`
- Writes para `AGNUS_DMACON` → `agnus_dma_write_dmacon()`
- Reads de `AGNUS_DMACONR` → `agnus_dma_read_dmaconr()`
- `agnus_dmacon_current()` para leitores externos

## VPOSR / VHPOSR — Chip ID e LOF

### Sprint 15: Chip ID
`agnus_get_beam()` retorna:
```c
lof   = s->beam.lof ? 0x8000 : 0;
vpos8 = (s->beam.vpos >> 8) & 1;
*vposr_out = lof | (0x20 << 8) | vpos8;   // 0x20 = ECS Fat Agnus 8372A PAL
```
**Antes**: `bits [14:8] = 0` → KS 3.1 tratava como hardware inválido.

### Sprint 26: LOF spin — Bug Crítico Pendente
KS 1.3 em `0xfc5a6c`:
```
btst #$6, $dff002.l   ; testa bit LOF de VHPOSR
bne  $fc5a6c           ; loop enquanto LOF=1
```
Esta barreira espera um **short frame** (LOF=0) antes de popular os bitplane buffers.
No harness, Agnus sempre reportava LOF=1 → loop eterno → tela preta.

**Fix necessário**: Em modo não-interlace, LOF deve ser **0** permanentemente.
Em modo interlace, alterna por frame.
**Status**: bug identificado mas FIX NÃO IMPLEMENTADO até Sprint 26.

## Copper

### Modelo de Execução (Sprint 17 — raster-time)
```
agnus_step():
  copper_step(&s->copper, s)   ← executa 1-2 words por ciclo beam
  bitplanes_step(...)
  if line_ready: denise_render_line(...)
```

**Antes (Sprint 07 — batch VBL)**: `copper_vbl_execute()` rodava toda a copper list
de uma vez no VBL. Problema: Copper podia escrever BPLCON0 enable e disable dentro
do mesmo batch → nenhuma linha com bitplanes visíveis.

**Raster-time (correto)**: Copper WAITs bloqueiam na posição correta do beam.
MOVEs afetam o estado que Agnus captura na linha atual.

### Registros Copper
- `COP1LCH/L` ($DFF080/$DFF082) — pointer lista 1
- `COP2LCH/L` ($DFF084/$DFF086) — pointer lista 2
- `COPJMP1/2` ($DFF088/$DFF08A) — strobes (redirect PC)
- `COPCON` ($DFF02E) — CDANG flag

### Bug Sprint 14: COPJMP sem log
COPJMP2 era executado mas sem log → Copper saltava para `cop2lc=0x000000` (nunca
escrito) → cascata de `skip illegal MOVE ir1=0000`. Corrigido com logs explícitos
em COPJMP1/COPJMP2.

### Bug Sprint 14: COPCON ausente de `agnus_is_copper_reg`
`case AGNUS_COPCON` (0x002E) não estava na lista → `cdang` ignorado.

## Bitplanes

### `bitplanes_dma_allowed()` — Decisão Centralizada (Sprint 24)
Adicionada função compartilhada entre:
- Line latch (`bitplanes_begin_line`)
- Agnus DMA query
- DMA scheduler filter

Relaxamento justificado pelo KS1.3 boot path onde BPLEN=0 mas DMAEN=1 +
`raw_nplanes>0` + bitplane pointers válidos → linha devia ser rendida.

### `BPL-DMA-FORCE` — Fallback de Bring-up
Path de compatibilidade que força bitplane DMA quando `DMAEN=1` mas `BPLEN=0`.
Logado como `[BPL-DMA-FORCE]`. Deve ser removido quando modelo DMA estiver completo.

### `fetch_plane_index` — granularidade de fetch
`BitplaneState` agora tem `fetch_plane_index` para fetch incremental plane-a-plane
por slot DMA.

## Blitter

### Fix: Barrel Shift + Word Continuity (Sprint 21)
**Bug**: copy-mode mascarava A com `BLTAFWM/BLTALWM` e usava valor mascarado como
`aold` para próxima palavra. Corrompía continuidade entre palavras.
**Fix**: usa raw A como `aold`; aplica máscara apenas ao `aval` corrente.

### Fix: Line-mode Address (Sprint 21)
**Bug**: cálculo de offset usava `>> 3` (avança a cada 8 pixels) em vez de `>> 4`
(palavra de 16 bits).
**Fix**: `((offset >> 4) << 1)`.

### API DMA
- `blitter_dma_request_mask()` — expõe demanda ao arbiter
- `blitter_dma_service_grant()` — processa 1 unidade por grant
- `blitter_step()` mantido como wrapper de compatibilidade

## Agnus Beam

### PAL: 313 linhas × 454 ciclos = 142.102 ciclos/frame a 50Hz
- VSYNC: vpos == 0 → disparar VERTB, VBL
- HSYNC: hpos wrap → avança vpos

### Campos em AgnusState
- `beam.hpos`, `beam.vpos` — posição atual
- `beam.lof` — Long Frame (interlace)
- `diwstrt`, `diwstop`, `ddfstrt`, `ddfstop` — Display Window / Data Fetch
- `bpl1pt..bpl6pt` — bitplane pointers (do Copper ou CPU)

### VPOSW / VHPOSW (Sprints 15/16 — pendente)
Write handlers para 0xDFF02A e 0xDFF02C. Mencionados como needed mas não confirmados implementados.

## Estado Atual

### O que Funciona
- DMA arbiter centralizado (DMACON, Copper, bitplanes, blitter, disco, sprites)
- Copper raster-time com WAITs na posição correta
- VPOSR chip ID correto (ECS Fat Agnus 8372A PAL = 0x20)
- Blitter word continuity corrigido
- Blitter line-mode address corrigido
- Sprites como DMA requesters (DeniseSprites state via grants)
- `agnus_dma_step` como orquestrador

### Bug Pendente (Alta Prioridade)
- **LOF bit em VHPOSR sempre 1** — impede KS1.3 de popular buffer de display.
  Fix: `beam.lof = 0` em modo não-interlace PAL normal.
  **Esse é o bloqueador atual da tela de boot do KS1.3.**

### Pendente
- VPOSW / VHPOSW write handlers
- Sprites: composição no `denise_render_line` (DMA grant existe, pixel compositing não)
- BPL-DMA-FORCE removal (após modelo DMA confiável)
- Audio como DMA requester real

## Arquivos Relevantes
- `src/chipset/agnus/agnus.h/.c` — AgnusState, agnus_step, beam, DMA dispatch
- `src/chipset/agnus/dma.h/.c` — AgnusDMA arbiter
- `src/chipset/agnus/copper.h/.c` — Copper raster-time
- `src/chipset/agnus/blitter.h/.c` — Blitter com DMA grants
- `src/chipset/agnus/bitplanes.h/.c` — Bitplane fetch state machine
