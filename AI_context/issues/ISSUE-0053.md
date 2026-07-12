---
id: ISSUE-0053
title: "Rigel throughput optimization — isolated low-priority campaign"
status: backlog
priority: low
type: performance
owner: agent
created_at: 2026-07-12
updated_at: 2026-07-12
tags: [rigel, chipset, performance]
related_files:
  - external/rigel
  - src/runtime/core_chipset.c
  - src/machine/machine_rigel_step.c
  - AI_context/consolidated/rigel_performance_research.md
---

# Escopo exclusivo

Esta é a **única issue** para otimização de throughput interno do Rigel e para a
hipótese de aproximar o chipset de 100% do wall clock. Ela tem baixa prioridade:
o objetivo primário do Bellatrix é computação rápida e estável em KS3.1/AROS,
multicore e 68040+/Emu68.

Correções funcionais de áudio, vídeo, Copper, blitter ou compatibilidade podem
permanecer em suas issues próprias. Elas não devem carregar metas de throughput
do Rigel.

# Gate de início

Não iniciar esta campanha enquanto uma medição não mostrar que o custo interno
do Rigel limita um workload prioritário. Antes de alterar o chipset, separar:

- tempo exclusivo dentro do Rigel;
- integração, contatos, locks e wakeups;
- composição e apresentação;
- Agnus, Denise, Paula, CIA, Copper e blitter;
- chamadas por frame e CCK virtuais por chamada.

# Backlog isolado

- [ ] A/B deadline externo normal versus coarse, sem promoção automática.
- [ ] Medir por domínio e identificar os hot paths internos dominantes.
- [ ] Avaliar scheduler orientado a eventos somente se clocks vazios dominarem.
- [ ] Avaliar Copper fast/fallback, blitter timed-functional e renderização por
  scanline somente quando o respectivo domínio dominar o perfil.
- [ ] Exigir feature flag, equivalência funcional e rollback para cada mudança.

# Referência

Algoritmos, emuladores de referência e hipóteses históricas estão em
`AI_context/consolidated/rigel_performance_research.md`. Esse consolidado não é
uma fila adicional.
