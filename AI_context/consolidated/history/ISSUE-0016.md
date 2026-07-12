---
id: ISSUE-0016
title: "1943 trainer — display shifted horizontally after BPLCON0 depth split"
status: fixed
priority: medium
type: bug
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - denise
  - agnus
  - viewport
  - bplcon1
  - bplcon0
  - horizontal-scroll
  - sprites
  - bitplane-dma
related_files:
  - external/rigel/src/chipset/denise/video/display_window.c
  - external/rigel/src/chipset/denise/render/compositor.c
  - external/rigel/src/chipset/agnus/timing/slot_scheduler.c
  - external/rigel/tests/test_agnus_domains.c
  - external/rigel/src/core/rigel_denise_api.c
  - src/disks/1943.adf
---

# Issue: 1943 trainer — display shifted horizontally

## Sintoma

Tela do trainer "PIRANHAS" (aparece logo no início, ~frame 736) exibe o logo
deslocado para a esquerda. O correto seria:

```
[peixe-sprite] PIRANHAS [peixe-sprite]
```

O que aparece: logo deslocado à esquerda, peixe da direita cortado/invisível,
peixe da esquerda fora do viewport ou também cortado.

Screenshot de referência: `1943.jpg` (frame=736). O usuário confirmou que,
no caminho `KS13.rom + 1943.adf`, frames 350/360 já reproduzem o problema.

Frame 50 é cedo demais nesse caminho: ainda está em estado inicial de boot
(`BPLCON0=0200`, `DMACON=0000`, `DIW/DDF=0000`).

## Hipóteses

1. **BPLCON1 não aplicado corretamente** — o scroll horizontal dos bitplanes
   (`PF1H`/`PF2H`) desloca o conteúdo mas o viewport de exportação não
   acompanha, fazendo o conteúdo aparecer na posição errada.
2. **Viewport start calculado errado** — `visible_x_start` está sendo calculado
   sem levar em conta o scroll, fazendo o frame exportado começar no pixel
   errado.
3. **HSTART dos sprites correto mas viewport deslocado** — os peixes (sprites
   de hardware) estão no HSTART certo no espaço Amiga, mas o frame exportado
   não cobre esse range.
4. **BPLCON0 muda profundidade antes do DDFSTRT e a tabela de DMA não é
   reconstruída** — confirmado como causa principal. O trainer alterna
   profundidades dentro do frame:

```text
0200 -> 1200 depth=1  beam ~073
1200 -> 4200 depth=4  beam ~096, antes do DDFSTRT=0038
4200 -> 1200 depth=1  beam ~138
1200 -> 2600 depth=2  beam ~152
2600 -> 0200 depth=0  beam ~255/256
```

Antes da correção, `agnus_slot_scheduler_set_depth()` atualizava `depth`, mas
não marcava `table_dirty`. Assim, linhas que passavam de 1 para 4 bitplanes
antes do fetch continuavam usando a tabela de slots montada para 1 bitplane.
O resultado visual era uma rotação/deslocamento horizontal: o `P` de
`PIRANHAS` aparecia separado na borda direita.

## Correção

- `external/rigel/src/chipset/agnus/timing/slot_scheduler.c`:
  `agnus_slot_scheduler_set_depth()` agora marca `table_dirty` quando o campo
  BPU de `BPLCON0` muda.
- `external/rigel/tests/test_agnus_domains.c`: adicionado caso sintético para
  `DDFSTRT=0038`, `DDFSTOP=00d0`, `BPLCON0 1200 -> 4200`, validando que a
  tabela passa de 20 slots (1 bitplane) para 80 slots (4 bitplanes) e remapeia
  os primeiros slots como planos 0..3.
- A investigação também adicionou cobertura de `BPLCON1` em `test_denise`
  para single/dual playfield. Essa cobertura é válida, mas não era a causa
  principal do 1943.

## Validação Visual

Dump usado:

```sh
rtk env BELLATRIX_RIGEL_TRACE=1 \
  BELLATRIX_RIGEL_DUMP_FRAME=350 \
  BELLATRIX_RIGEL_DUMP_PPM=/tmp/1943_350_depthdirty.ppm \
  KICKSTART=src/roms/KS13.rom \
  ADF=src/disks/1943.adf \
  FRAMES=360 \
  ./run.sh harness
rtk convert /tmp/1943_350_depthdirty.ppm /tmp/1943_350_depthdirty.png
```

Resultado: frame 350 agora mostra `PIRANHAS` completo, centralizado, com os
sprites laterais visíveis. O frame exportado permanece `447x215`, com
`DIW=2981/38c0`, `DDF=0038/00d0`, `BPLCON1=0020`.

## Relação com outros bugs

Mesmo território da fix de viewport de WB1.3 (KS20 e ISSUE-0008). A fix
anterior adicionou 32px de borda antes de `visible_x_start` — pode estar
interagindo mal com jogos que usam BPLCON1 para centrar conteúdo.

Após esta correção, o 1943 deixa de apontar para viewport como causa principal.
Ainda vale manter KS20, Battle Squadron e SOTA como categorias separadas:
viewport/export, sprites/scroll e vazamento horizontal de linhas,
respectivamente.

## Log de execução

- 2026-06-26: issue registrada. Investigação iniciada com harness + trace.
- 2026-06-26: frame 50 descartado para este caminho; frame 350/360 reproduz.
- 2026-06-26: confirmado que `BPLCON1=0020` não era suficiente para explicar
  o `P` deslocado; adicionada cobertura unitária de scroll por nibble.
- 2026-06-26: corrigida invalidação da tabela de bitplane DMA quando `BPLCON0`
  muda profundidade; frame 350 passa a exibir `PIRANHAS` corretamente.
- 2026-06-26: regressão focada passou:
  `test_timing`, `test_agnus_domains`, `test_denise`, `test_video_modes`,
  `test_priority`, `test_dualpf`.
