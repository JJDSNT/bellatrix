---
id: ISSUE-0048
title: "Performance end-to-end: recuperar realtime no harness e provar ganhos multicore/Emu68"
status: doing
priority: critical
type: performance
owner: agent
created_at: 2026-07-11
updated_at: 2026-07-11
tags:
  - performance
  - harness
  - rigel
  - multicore
  - emu68
  - musashi
  - audio
related_files:
  - tools/harness/main.c
  - src/machine/machine_rigel_step.c
  - src/runtime/core_chipset.c
  - src/cpu/emu68/bellatrix.c
  - external/rigel
---

# Objetivo

Transformar a arquitetura já implementada em ganho mensurável: recuperar
realtime no harness, depois provar onde Musashi/Emu68 e single/multicore ganham
ou perdem. Não usar FPS do OSD como fonte canônica.

Gate PAL primário:

```text
emulated_cck / wall_second ~= 3.546.895
emulated_frames / wall_second ~= 50
```

O áudio só pode ser avaliado como qualidade quando a produção sustentada estiver
próxima de realtime. O caminho direto histórico permanece referência perceptual;
unified/DRC é experimento e não será promovido enquanto for inferior.

# Baseline observado em 2026-07-11

KS1.3 + Battle Squadron no harness atual:

```text
0,7-1,7 M CCK/s
15-20 frames/s
120k-250k rigel_step/s
rigel_step: 390-500 ms/s
presenter: 180-290 ms/s
audio host: 1-15 ms/s
```

Muitos intervalos avançam em média apenas 5-10 CCK por chamada. Desligar 3 de
cada 4 apresentações reduz `present_ms`, mas não recupera realtime. Rigel limpo
`cee4e0d`, Rigel histórico `78c45bf` com Bellatrix atual e binário histórico
completo `19e6ad5` foram testados; o histórico melhora frames, mas não recupera
o áudio lembrado. Não existe hoje um baseline golden completo reproduzível.

# Hipóteses priorizadas

1. Fragmentação externa/interna: deadlines, bus changes e MMIO flush geram
   centenas de milhares de chamadas pequenas ao Rigel.
2. Presenter: conversão, escala e cópia pixel a pixel consomem até ~30% do
   segundo, independentemente do custo do chipset.
3. Custo por CCK dentro de Agnus/Rigel aumentou em relação às medições antigas.
4. Multicore não acelera enquanto Core 2 for o caminho crítico ou o protocolo
   publicar/rendezvous em granularidade pequena.
5. Emu68 acelera apenas a fração CPU; o ganho total é limitado por Rigel,
   presenter e sincronização (Amdahl).

# Plano incremental

## Fase 0 — baseline limpo e repetível

- Preservar/retirar experimentos de áudio não validados do caminho default.
- Fixar ROM/ADF, CPU, resolução e flags de trace.
- Emitir uma linha canônica por segundo com CCK/s, frames/s e realtime ratio.
- Medir sem instrumentação invasiva e registrar host/build/configuração.

## Fase 1 — histograma de steps e causa do corte

Para cada `rigel_step`, registrar de forma agregada:

```text
reason = target | deadline | bus_change | mmio_flush | frame | other
size buckets = 1, 2-4, 5-8, 9-32, 33-128, >128 CCK
```

Também registrar p50/p90/p99 e CCK total por razão. Nenhum log por step.

## Fase 2 — separar chipset, composição e apresentação

A/B independentes:

- Rigel completo sem presenter;
- avanço Agnus/Paula/CIA sem composição Denise;
- composição sem cópia/escala SDL;
- presenter atual versus formato nativo/dirty lines.

Medir `agnus`, `paula`, `cia`, `denise_compose`, `convert`, `scale`, `copy` e
`SDL present`. Otimizar somente o domínio dominante comprovado.

## Fase 3 — recuperar realtime no harness

Reduzir fronteiras redundantes preservando sincronização observável de MMIO.
Gate: >=95% de realtime sustentado no workload e nenhum desvio visual/boot nos
testes focados. Só então reconstruir e validar áudio suave.

## Fase 4 — matriz bare metal normalizada

Executar o mesmo relatório CCK/s em:

1. Musashi single-core;
2. Musashi multicore;
3. Emu68 single-core;
4. Emu68 multicore.

Comparar CPU útil, Rigel, presenter, waits/locks, backlog e Host Reactor. O
multicore só é sucesso se aumentar CCK/s ou reduzir jitter sem regressão.

## Fase 5 — arquitetura de deadline/rendezvous

Com os dados anteriores, agrupar publicação CPU→Core2 e substituir ping-pong
por janelas/epochs somente onde houver ganho comprovado. Manter Core 0 como
control plane, Core 1 CPU, Core 2 owner exclusivo do Rigel e Core 3 reservado.

# Condições de parada

- regressão visual, boot ou IRQ;
- ganho somente no OSD sem ganho em CCK/s;
- áudio mascarando execução sub-real-time;
- otimização que mistura mudança semântica e performance sem A/B isolado.

# Relações

- `ISSUE-0009`: regressão histórica de áudio/performance do harness;
- `ISSUE-0007`: performance e protocolo multicore;
- `ISSUE-0038/0039`: liveness e API pública Emu68;
- `issue_core0_arbiter_scheduler.md`: arquitetura futura de epochs/deadlines.
