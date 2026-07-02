---
id: ISSUE-0024
title: "KS20: texto não renderiza — boot screen sem texto; após o boot, requesters e nomes de ícones sem texto"
status: open
priority: high
type: bug
owner: unassigned
created_at: 2026-07-02
updated_at: 2026-07-02
tags:
  - ks20
  - denise
  - rendering
  - text
  - blitter
related_files:
  - src/chipset/denise/denise.c
  - src/chipset/agnus/blitter.c
  - src/chipset/agnus/agnus.c
---

# Sintoma

Com KS2.0 (KS20.rom), o texto não aparece em vários contextos:

1. **Boot screen**: a tela de boot aparece, mas sem o texto.
2. **Pós-boot (Workbench)**: requesters abrem sem o texto; os nomes dos
   ícones no desktop/janelas também não são desenhados.

Gráficos em geral (janelas, bordas, ícones) renderizam — o que falta é
especificamente o texto, o que sugere um caminho comum de desenho de glifos
(blitter em modo texto/font rendering, ou algum modo de bitplane/máscara que
o KS2.0 usa e o KS1.3 não).

# Contexto

- KS1.3/Workbench 1.3 e AROS renderizam texto normalmente — o problema é
  específico do KS2.0.
- Houve rodada de "KS20 rendering fixes" em 2026-06-29 (ver AI_context/log
  dessa data e commit `285dcd2`-adjacente) — verificar se este sintoma é
  regressão dessas mudanças ou gap pré-existente.
- Relacionado mas distinto de [[ISSUE-0023]] (stall de boot do KS20 com ISO):
  este issue é sobre renderização e reproduz sem CD.

# Hipóteses iniciais

- Blitter: minterm/canal (B = fonte de glifos, máscaras FWM/LWM, shift) usado
  pelo `Text()` do graphics.library V37 pode exercitar um caso não coberto.
- Denise: prioridade/planos ou modo (e.g. escrita em plano único via
  blit cookie-cut) que o KS2.0 usa para texto.
- Verificar com btrace (`0x0004` chipset) uma chamada de Text() no boot
  screen e comparar os registradores do blitter contra WinUAE.

# Reprodução

Boot KS20.rom (harness ou hardware) — o boot screen já demonstra o problema,
sem precisar de disco.
