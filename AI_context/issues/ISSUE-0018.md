---
id: ISSUE-0018
title: "Rigel graphics — SOTA demo reference bug"
status: fixed
priority: medium
type: research
owner: unassigned
created_at: 2026-06-26
updated_at: 2026-06-27
tags:
  - rigel
  - graphics
  - demo
  - blitter
related_files:
  - src/disks/sota.adf
  - sota.jpg
---

# Resumo

`src/disks/sota.adf` foi adicionado como referência de bug gráfico. O sintoma de
linhas horizontais "vazando" foi confirmado como erro de line-mode do blitter.

# Estado Atual

Não associar este caso automaticamente aos problemas de EON, KS20, 1943 ou
Battle Squadron. Este caso ficou classificado como blitter line-mode + fill.

Screenshot de referência: `sota.jpg`. Sintoma observado pelo usuário: linhas
horizontais "vazando".

Reprodução headless confirmada no frame 987:

```text
src/roms/KS13.rom + src/disks/sota.adf
BPLCON0=2200 depth=2 lores single playfield
BPLCON1=0020
BPLCON2=0024
DIW=1c71/3ed1
DDF=0030/00d8
DMACON=07ff
export=465x256 vis=113..465/28..284
```

O dump PPM contém apenas duas cores, então o vazamento é dado de playfield
composto, não artefato de SDL, escalonamento RGB ou janela do harness.

Experimentos descartados:

- Forçar `BPLCON1` high/low nibble igual no single-playfield não alterou o
  frame 987.
- Usar ponteiro vivo de `BPLxPT` a cada fetch mudou muito a imagem e ficou
  menos parecido com `sota.jpg`; a investigação voltou ao modelo atual de base
  de linha congelada, com avanço dos ponteiros no fim da linha.
- Sonda filtrada de `copper_write` + `bpl_fetch` no frame 987 não mostrou
  copper writes na região visível; os fetches estavam lendo conteúdo já escrito
  em chip RAM.

Diagnóstico confirmado:

- Chip-write watch em `0x05afd0..0x05b040` mostrou que blits de line-mode
  gravavam bits de borda (`BLTCON0/1` como `da4a/0043`, entre outros) que eram
  depois expandidos por fill descendente/exclusivo (`09f0/0012`).
- A implementação anterior desenhava line-mode com uma aproximação de
  Bresenham por octante. A referência WinUAE (`referencias/winuae/blitter.cpp`)
  usa `BLTADAT/BLTAFWM` deslocado por `BLTCON0[15:12]`, rotação de `BLTBDAT`,
  sinal inicial `BLTCON1[6]`, modo single-dot `BLTCON1[1]` e avanço X/Y
  comandado diretamente pelos bits de `BLTCON1`.
- A correção removeu as linhas vazadas, confirmado visualmente pelo usuário.

# Próximo Passo

- Manter SOTA como referência de regressão visual para blitter line-mode.
- Expandir testes unitários de line-mode com casos de sinal inicial,
  single-dot e octantes adicionais derivados da semântica WinUAE.
- Não usar SOTA para justificar mudanças em Copper/bitplane ou presenter sem
  uma nova evidência independente.

# Log

- 2026-06-26: registrado como demo de referência para investigação futura.
- 2026-06-26: `sota.jpg` registrado como screenshot do sintoma de linhas
  horizontais vazando.
- 2026-06-27: reprodução headless no frame 987; bug classificado como provável
  timing Copper/bitplane na mesma scanline. Experimento de ponteiro vivo foi
  descartado e revertido ao modelo de base congelada por linha.
- 2026-06-27: hipótese Copper/bitplane descartada. Watch de chip RAM confirmou
  bits gerados por blitter line-mode antes do fill. Line-mode foi alinhado à
  semântica WinUAE; usuário confirmou remoção das linhas vazadas.
