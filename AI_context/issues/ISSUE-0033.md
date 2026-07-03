---
id: ISSUE-0033
title: "Harness: RTG (gfx board) para distros que não usam Denise — ArosOne desktop"
status: open
priority: low
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - harness
  - rtg
  - denise
  - aros
  - arosone
related_files:
  - tools/harness/musashi_backend.c
  - src/chipset/denise/denise.c
  - AI_context/consolidated/memory_model.md
  - AI_context/issues/ISSUE-0031.md
---

# Contexto

ArosOne-68K boota no harness (68040 + Z2 fast RAM, ISSUE-0031) mas o
desktop nunca aparece: a distro é configurada para tela RTG (perfil
WinUAE usa uaegfx com 512MB VRAM). O harness só renderiza via Denise.
O Emu68 tem RTG próprio no hardware; o harness não tem nada.

# Objetivos (em ordem de custo)

1. **Curto prazo, sem RTG**: forçar screenmode nativo PAL na distro
   editando prefs/startup direto no HDF (tools/hdf write) e validar que o
   desktop ArosOne aparece via Denise. Documentar a receita.
2. **Médio prazo**: avaliar um board RTG mínimo no harness — candidatos:
   - uaegfx/P96 API (o que AROS m68k já tem driver pronto)
   - framebuffer simples estilo Emu68 (reusar o driver que o Emu68 expõe?)
   Depende de Z3 ([[ISSUE-0032]]) para VRAM decente; um RTG Z2 ficaria
   limitado a ~8MB de janela.

# Critério de pronto (fase 2)

Desktop de uma distro RTG-only (ArosOne) renderizado pelo harness em
resolução > PAL, com screenshot pelo pipeline existente
(HARNESS_SCREENSHOT_FRAMES).

# Notas

- Não distorcer o modelo do chipset: RTG é um board de expansão, não
  parte da Denise (princípio "Denise é instância explícita").
- Ver memory_model.md para a discussão de presets (evitar 512MB VRAM
  estilo UAE; 16-32MB bastam).
