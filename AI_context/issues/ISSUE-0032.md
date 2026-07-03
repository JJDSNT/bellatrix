---
id: ISSUE-0032
title: "Harness: Zorro III fast RAM (32-bit) via Super Buster — cenário Z2+Z3"
status: open
priority: medium
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - harness
  - zorro3
  - superbuster
  - memory
  - musashi
related_files:
  - tools/harness/musashi_backend.c
  - src/machine/bus/superbuster/superbuster.c
  - src/machine/bus/zorro3/zorro3.c
  - src/machine/machine_rigel_bus.c
  - AI_context/consolidated/memory_model.md
---

# Contexto

O harness hoje só tem chip 2MB + slow 1.5MB + Z2 fast 8MB (ISSUE-0031).
A infra Z3 **já existe em parte**: `src/machine/bus/superbuster/` e
`src/machine/bus/zorro3/` (registro de boards, config read/write
despachado em machine_rigel_bus.c) — o Super Buster foi introduzido
justamente para permitir os dois cenários (Z2 e Z3) na mesma máquina.

# Bloqueio principal

`tools/harness/musashi_backend.c` mascara TODO acesso do CPU com
`addr &= 0x00FFFFFF` (barramento 24-bit). Z3 vive em >= 0x10000000, então
nenhum acesso chega ao espaço Z3. Musashi em 68020+ endereça 32-bit
normalmente.

# Objetivo

- Em CPU 68020+: não mascarar para 24-bit; rotear >16MB para o caminho
  Z3 (config space 0xFF000000, janelas de board)
- Board de fast RAM Z3 (ex.: 128MB) análoga à Z2, com a MESMA regra do
  autoconfig: janela silenciosa até base atribuída (ver
  memory_model.md / lição da ISSUE-0031)
- `HARNESS_Z3RAM=<MB>` para habilitar (default off até estabilizar)
- Validar: KS3.1/AROS enxergam a RAM na memlist; cenário misto Z2+Z3 ok;
  em 68000 nada muda (24-bit)

# Notas

- Cuidado com os pontos do backend que assumem 24-bit (normalize_addr,
  tabelas de watch, btrace)
- Preset alvo (memory_model.md): AROS "moderno" = chip 2MB + Z3 128MB
  (+ RTG, ver [[ISSUE-0033]])
