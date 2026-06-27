---
id: ISSUE-0017
title: "Rigel graphics — EON dual-playfield 6-plane DMA ordering"
status: doing
priority: high
type: bug
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - rigel
  - agnus
  - denise
  - bitplane-dma
  - dual-playfield
  - bplcon1
related_files:
  - external/rigel/src/chipset/agnus/timing/slot_scheduler.c
  - external/rigel/src/chipset/agnus/timing/slot_scheduler.h
  - external/rigel/src/chipset/denise/render/compositor.c
  - external/rigel/tests/test_agnus_domains.c
  - external/rigel/tests/test_denise.c
  - src/disks/eonA.adf
  - src/disks/sota.adf
---

# Resumo

`src/disks/eonA.adf` expõe erro gráfico em modo lores 6 bitplanes / dual
playfield. A imagem inicialmente aparecia horizontalmente fatiada. Após ajustes
no scheduler de bitplane DMA, o frame passou a ser coerente, mas ainda não está
100% correto.

# Problema

No trecho do EON em torno do frame 1722:

```text
BPLCON0=6601 depth=6
BPLCON1 varia: 002d, 00b9, 0046, 00d2
BPLCON2=0044
DIW=5881/00c1
DDF=0030/00d0
```

O render antigo mostrava linhas horizontais desconexas. Depois da correção de
agendamento em grupos lores, o planeta/arte principal aparece, mas com cortes e
deslocamentos residuais.

# Objetivo

Renderizar corretamente o EON no modo 6-plane dual-playfield, sem regressão em:

- `KS20.rom`
- `1943.adf`
- `battle.adf`
- `sota.adf` fica registrado como referência futura de outra categoria de bug,
  sem entrar no critério de aceite desta correção de DMA/scroll do EON.
- testes Rigel de timing, Denise, dualpf e prioridade

# O que foi feito

- Harness SDL aumentado para `768x576`, com janela host desacoplada do tamanho
  do frame emulado.
- `machine_present_frame_from_rigel()` não chama `PAL_Video_Resize()` no build
  `BELLATRIX_HARNESS`.
- `compositor.c`: dual playfield agora usa scroll separado de `BPLCON1`:
  PF1 = nibble baixo, PF2 = nibble alto.
- `slot_scheduler.c/.h`: bitplane slots carregam índice lógico por slot.
- `slot_scheduler.c`: lores bitplane DMA passou a agendar grupos por word:
  `hpos = DDFSTRT + word * 8 + plane`, em vez de um fluxo simples stride-2.
- `slot_scheduler.c`: mudança de profundidade via `BPLCON0` agora invalida a
  tabela de slots de bitplane DMA. Isto corrige o custom boot do `1943.adf`,
  que alterna `1200 -> 4200` antes de `DDFSTRT`.
- Fetch de bitplane usa base da linha + índice lógico para preencher
  `plane_words[plane][word]`, e os ponteiros avançam no fim da linha pelo total
  de words efetivamente usados antes do modulo.
- `test_agnus_domains`: adicionado teste matricial lores para depths 1 a 6,
  `DDFSTRT=0x0030`, `DDFSTOP=0x00d0`, validando slots físicos, índice lógico
  e avanço de ponteiros após uma linha.
- `test_denise`: adicionada cobertura sintética para `BPLCON1` em lores single
  playfield e dual playfield, validando deslocamento independente de PF1
  (nibble baixo) e PF2 (nibble alto).
- O teste antigo de copper/blitter foi ajustado para validar o comportamento
  atual de `copper_exec_move`: MOVE protegido para registrador baixo para o
  copper até VBL e não inicia blitter.

# O que falta fazer

- Confirmar se `bitplane_words_per_plane()` está correto para DDF ranges usados
  por EON e outros ranges ainda não validados. O caso específico do 1943
  (`DDF=0038/00d0`, depth change 1->4 antes do fetch) já tem regressão.
- Revisar semântica de `BPLCON1` em hires e em mudanças por linha; os casos
  lores básicos single/dual já têm cobertura unitária.
- Criar capturas/dumps apenas quando investigando cada categoria específica de
  bug; não tratar `EON`, `1943`, `KS20`, `battle` e `sota` como comparáveis
  visualmente entre si.
- Classificar o bug de `sota.adf` quando a investigação atual de EON/scroll
  estabilizar.

# Decisões tomadas

- O sintoma do EON não é causado pelo presenter SDL ou pelo export viewport.
- O erro primário observado era ordem/temporização de bitplane DMA em lores
  high-depth.
- A correção deve preservar a distinção entre slot físico de DMA e posição
  lógica do word no scanline.

# Critérios de aceite

- [ ] EON frame ~1722 aparece sem cortes horizontais/resíduos evidentes.
- [x] `1943.adf` não regride no trainer.
- [ ] `KS20.rom` mantém wrap/black-bar corrigidos.
- [ ] `battle.adf` não perde nave/sprites por clipping incorreto.
- [x] Testes Rigel focados passam.

# Observações

Comando útil para dump headless:

```sh
rtk env BELLATRIX_RIGEL_TRACE=1 \
  BELLATRIX_RIGEL_DUMP_FRAME=1722 \
  BELLATRIX_RIGEL_DUMP_PPM=/tmp/eon_1722.ppm \
  KICKSTART=src/roms/KS13.rom \
  ADF=src/disks/eonA.adf \
  FRAMES=1725 \
  ./run.sh harness 2>&1 | \
  rtk grep --line-buffered -E '\[(RUN|HARNESS|RIGEL-DUMP|RIGEL-FRAME-VIDEO)\]'
```

# Log de execução

- 2026-06-26: EON adicionado à investigação gráfica.
- 2026-06-26: frame 1722 confirmou imagem fatiada em `bplcon0=6601`.
- 2026-06-26: separado scroll PF1/PF2 de `BPLCON1`.
- 2026-06-26: alterado scheduler para índices lógicos de bitplane por slot.
- 2026-06-26: alterado agendamento lores para grupos por word/plane.
- 2026-06-26: dump `/tmp/eon_1722_lores_groups.png` mostra melhora clara, mas
  ainda não atinge fidelidade completa.
- 2026-06-26: adicionada cobertura em `test_agnus_domains` para o caso EON
  `depth=6`, incluindo layout de slots e avanço de `BPLxPT`; regressão focada
  de timing/Denise/dualpf/prioridade/modos de vídeo passou.
- 2026-06-26: cobertura lores expandida para depths 1 a 6 no mesmo DDF do EON.
- 2026-06-26: `src/disks/sota.adf` registrado como referência futura de outra
  categoria de bug.
- 2026-06-26: adicionada cobertura em `test_denise` para scroll `BPLCON1`
  lores single playfield e dual playfield PF1/PF2.
- 2026-06-26: `1943.adf` frame 350 corrigido: `PIRANHAS` aparece inteiro após
  invalidar a tabela de slots quando `BPLCON0` muda profundidade.
