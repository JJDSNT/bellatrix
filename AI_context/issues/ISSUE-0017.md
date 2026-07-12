---
id: ISSUE-0017
title: "Rigel graphics — EON dual-playfield 6-plane DMA ordering"
status: doing
priority: high
type: bug
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-29
tags:
  - rigel
  - agnus
  - denise
  - bitplane-dma
  - sprites
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
- 2026-06-27: frame 2619 reclassificado em duas categorias distintas:
  - o retângulo vertical branco desaparece quando o overlay de sprites é
    desabilitado temporariamente no compositor; logo é um problema de sprite
    overlay/DMA/priority, não de bitplane export;
  - as linhas horizontais continuam sem sprites, então permanecem no território
    de playfield/blitter/composição.
- O trace focado do frame 2619 mostrou sprites `1..5` desenhando `pix=3` em
  blocos de 16 px, com `vstart=124`, `vstop=256` e `hstart` em
  `272,288,304,320,336`. Isto forma exatamente o retângulo branco observado.
- O BPL5 exibido nesse frame é alimentado por um blit legítimo:
  `BLTCON0/1=0d0c/0000`, `dspan=009040..00a504`, `dpt=009040`,
  `apt=00b0c0`, `bpt=00d240`, `size=12x190`. O blit cobre a área usada como
  plano alto, mas não explica o retângulo quando sprites estão ativos.
- Corrigida semântica de dual playfield: PF1-vs-PF2 usa `BPLCON2` bit 6
  (`PF2PRI`); os campos `PF1P/PF2P` são prioridade contra sprites. Cobertura
  atualizada em `test_dualpf`. Esta correção é correta, mas não altera o frame
  2619 do EON.
- 2026-06-29: latch de `BPLCON0/1/2` no momento do primeiro slot DMA de bitplane
  de cada scanline (`line_bplcon0/1/2` + `line_bplcon_valid` em
  `rigel_denise_output_state_t`). O compositor passa a usar os valores travados
  em vez dos registradores ao vivo, corrigindo frames em que o copper altera
  BPLCON depois de iniciado o DMA da linha.
- 2026-06-29: `bitplane_line_depth` separado de `sched->depth`. O avanço de
  `BPLxPT` e a aplicação de `BPL1MOD/BPL2MOD` no fim da linha usam o depth que
  estava ativo durante o DMA daquela linha. Evita avanço incorreto de ponteiro
  quando o copper muda `BPLCON0` mid-frame após o DMA já ter ocorrido.
- 2026-06-29: clipping DDF (`x_start`/`x_stop`) nos loops de sprites e HAM:
  pixels fora da janela DDF ativa não são mais escritos no scanline buffer.
- 2026-06-29: `clear_scanline_to_border()` — linhas que não tiveram nenhum DMA
  de bitplane agora recebem a cor de borda (`COLOR00`) em vez de preservar
  conteúdo stale do frame anterior.
- 2026-06-29: `denise_sprites_reset()` chamado no início do VBL, garantindo que
  o estado de sprites (armed/vstart/vstop) começa limpo em cada campo.
- 2026-06-29: offset da janela visível corrigido para `-128` pixels lores (era
  `-32`); `rigel_get_frame()` aplica altura mínima para sequências de DIWHIGH
  malformadas ou de curta duração.
- 2026-06-29: commits em `external/rigel`: `357c4ec` (CIA /DSKCHG) e `67e82ab`
  (agnus/denise rendering). Nenhuma regressão observada em 1943 nem em EON.

# O que falta fazer

- **Battle Squadron** é o próximo alvo de rendering (2026-06-29). Nave do
  jogador (sprite 0) desaparece quando o fundo começa a scrollar; copper list
  de scroll provavelmente sobrescreve `SPRxPT`. Ver seção de diagnóstico em
  ISSUE-0008.
- Confirmar se `bitplane_words_per_plane()` está correto para DDF ranges usados
  por EON e outros ranges ainda não validados. O caso específico do 1943
  (`DDF=0038/00d0`, depth change 1->4 antes do fetch) já tem regressão.
- Revisar semântica de `BPLCON1` em hires e em mudanças por linha; os casos
  lores básicos single/dual já têm cobertura unitária.
- Criar capturas/dumps apenas quando investigando cada categoria específica de
  bug; não tratar `EON`, `1943`, `KS20`, `battle` e `sota` como comparáveis
  visualmente entre si.
- ~~Investigar sprite DMA/estado de sprite no EON frame 2619~~ — resolvido como
  efeito colateral do fix ECS blitter (ver log 2026-06-28).

# Decisões tomadas

- O sintoma do EON não é causado pelo presenter SDL ou pelo export viewport.
- O erro primário observado era ordem/temporização de bitplane DMA em lores
  high-depth.
- A correção deve preservar a distinção entre slot físico de DMA e posição
  lógica do word no scanline.
- O frame 2619 não deve ser tratado como um único bug: o retângulo branco é
  sprite overlay; as linhas horizontais são independentes e persistem sem
  sprites.
- A numeração de frames precisa ser qualificada por origem. O `frame=...` do
  título SDL/`FRAMES=...` é contador do harness; dumps via
  `BELLATRIX_RIGEL_DUMP_FRAME` usam `rigel_get_frame().frame_count` vindo da
  Denise; logs `[RIGEL-FRAME-VIDEO] N=...` usam contador do trace Rigel. Esses
  contadores não têm obrigação de coincidir porque resetam/publicam em pontos
  diferentes. Para correlacionar uma captura, registrar harness frame e
  Denise/Rigel frame na mesma linha de trace.

# Critérios de aceite

- [ ] EON frame ~1722 aparece sem cortes horizontais/resíduos evidentes.
- [x] `1943.adf` não regride no trainer (verificado 2026-06-29).
- [x] `KS20.rom` melhorado (bplcon latch + depth por linha + DDF clipping, 2026-06-29); wrap/black-bar corrigidos desde sessão anterior.
- [ ] `battle.adf` não perde nave/sprites por clipping incorreto. **Próximo alvo.**
- [x] Testes Rigel focados passam.

# Observações

Comando útil para dump headless:

```sh
rtk env BELLATRIX_RIGEL_TRACE=1 \
  BELLATRIX_RIGEL_DUMP_FRAME=2619 \
  BELLATRIX_RIGEL_DUMP_PPM=/tmp/eon_2619.ppm \
  KICKSTART=src/roms/KS13.rom \
  ADF=src/disks/eonA.adf \
  FRAMES=2622 \
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
- 2026-06-27: EON frame 2619 analisado com dump e testes temporários:
  retângulo vertical branco isolado para sprites `1..5`; linhas horizontais
  persistem sem sprites.
- 2026-06-27: `dualpf_priority_resolve()` e compositor ajustados para usar
  `BPLCON2.PF2PRI` na disputa PF1/PF2; testes `test_dualpf`, `test_denise` e
  `test_priority` passaram.
- 2026-06-27: registrada discrepância esperada entre contadores de frame do
  harness, machine/presenter, Denise/Rigel e trace; próximas análises devem
  especificar a origem do contador ou logar os contadores lado a lado.
- 2026-06-28: retângulo branco (sprites `1..5`, frame ~2400 no contador do
  harness) desapareceu após fix ECS blitter (`$DFF05C`/`$DFF05E` em Rigel).
  Com o blitter nunca disparando, a ROM não completava a inicialização gráfica
  e os sprite registers ficavam com estado espúrio. Com o blitter funcional
  (1348 triggers/run), o estado de sprites é corretamente zerado/inicializado
  pela ROM. A categoria "sprite overlay" do frame 2619 está resolvida.
- 2026-06-29: latch de BPLCON por linha (`line_bplcon0/1/2`), `bitplane_line_depth`
  por linha, clipping DDF, border fill em linhas sem DMA, `denise_sprites_reset`
  no VBL, offset de janela visível corrigido para -128. Commits `357c4ec` e
  `67e82ab` no rigel; bump de submodule em bellatrix. KS20 melhorado; 1943 e EON
  sem regressão. Battle Squadron definido como próximo alvo de rendering.

## Sessão 2026-07-06 — Battle Squadron e eonA

- `eonA.adf` branco não era regressão gráfica: o demo exige
  `HARNESS_CPU=68000`; o default 68040 permanece branco.
- No gameplay de Battle, a corrupção já está na chip RAM, não no
  compositor/viewport. Tile sheet e padrões isolados do blitter estão íntegros.
- As colunas pretas expõem tiles legítimos de starfield porque passadas de
  terreno do seam ficam parciais; o jogo continua usando trackloader próprio.
- Foram observados 14.743 writes de BLTSIZE com blitter ocupado. O experimento
  de flush do blit anterior e preservação de `INTREQ.BLIT` é plausível e foi
  preservado, mas não curou o sintoma.
- A corrupção também ocorre com CPU 68000 e com/sem os WIPs de prioridade,
  seed e flush. Próximo diagnóstico é descobrir qual estado guest decide a
  quantidade parcial de colunas (VPOS/VHPOS, VBL, DSKBLK ou relação de IRQ).

Repro histórico:

```sh
HARNESS_MOUSE_LMB_SCRIPT="250:10,1100:10" \
  BELLATRIX_RIGEL_DUMP_FRAME=2000 \
  KICKSTART=src/roms/KS13.rom ADF=src/disks/battle.adf \
  FRAMES=2020 ./run.sh harness
```
