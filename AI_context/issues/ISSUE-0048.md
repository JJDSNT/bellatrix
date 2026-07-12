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

## Primeiro corte comprovado: deadline de slot duplicado

O histograma de Battle Squadron mostrou que 98-99,8% dos passos eram cortados
por `rigel_get_next_deadline()`, com predominância de 1-4 CCK. A origem é o
deadline do slot scheduler de Agnus: ele era exposto como fronteira obrigatória
ao host embora `rigel_step()` já percorra e processe internamente todos os slots
do intervalo.

O experimento em `external/rigel` separa os dois contratos:

- `rigel_get_next_deadline()` continua slot-exato para hosts que arbitram o
  chip bus externamente;
- `rigel_get_next_observable_deadline()` exclui somente o slot interno e mantém
  blitter, linha/beam, VERTB, copper wait, áudio e disco;
- `rigel_get_next_bus_change()` permanece uma barreira independente no
  Bellatrix.

Smoke test local, AROS 68040, 100 frames, headless:

```text
antes (workload Battle): 0,7-1,7 M CCK/s, deadline ~= 99%, 1-4 CCK dominante
depois (smoke AROS):      4,36 M CCK/s no primeiro intervalo de trace
                         deadline=19.216, bus=63.782, mmio=209
                         83.207 steps para 4.364.529 CCK
```

Esse smoke ultrapassa o gate PAL de 3,55 M CCK/s, mas ainda não substitui o A/B
com a mesma ROM/ADF de Battle Squadron. A remoção do deadline redundante também
revelou o próximo limitante: `bus_change` passou a dominar os passos pequenos.

O A/B de Battle Squadron confirmou que a primeira mudança isolada não resolveu
o workload: 1,04-1,28 M CCK/s, com `bus_change` em 96-98% dos 144k-198k passos/s
e tamanhos 1-4 novamente dominantes. A inspeção do consumidor mostrou que o
Bellatrix não consulta `cpu_would_stall` nem aplica `rigel_get_cpu_resume_time()`;
logo, ele cortava em cada troca de dono do chip bus sem usar o resultado para
parar a CPU. O próprio contrato de integração do Rigel permite que hosts que não
modelam contenção ignorem essa fronteira.

Próximo A/B: remover `bus_change` dos schedulers síncrono e Core 2, mantendo
MMIO flush, deadline observável e o target de tempo publicado pela CPU. Só
reintroduzir transições de bus quando a contenção for implementada por inteiro
(estado observado + stall/resume), nunca apenas como fragmentação temporal.

## Baseline contaminado por troca do CPU default

O baseline histórico de aproximadamente 50 FPS (`19e6ad5`, 2026-06-26) usava
Musashi 68000 por default. O commit `b1565e4`, posterior ao baseline, mudou o
default efetivo de `musashi_backend_init()` para 68040 e as mensagens de
`run.sh` para 68040, embora o help ainda declare 68000. Assim, as comparações
recentes de KS1.3 + Battle não reproduziam o perfil histórico.

A/B obrigatório antes de atribuir a regressão ao Rigel:

```text
HARNESS_CPU=68000 HARNESS_CPU_QUANTUM=454
```

Comparar CCK/s, `cpu_run`, `rigel_ms`, `present_ms` e qualidade do áudio com o
mesmo ROM/ADF. Manter 68040 como produto separado, não como substituto silencioso
do baseline 68000.

Resultado: hipótese falsificada. Com 68000 e quantum 454, Battle permaneceu em
1,29-1,59 M CCK/s, `rigel_ms=393-425` e 18-23 frames/s. A troca de CPU default
é uma inconsistência real de produto/documentação, mas não explica a regressão.

## Revalidação limpa do Rigel histórico

O teste anterior de `78c45bf` reutilizou o diretório CMake corrente e pode ter
mantido objetos mais novos do Rigel atual; ele não deve ser usado para absolver
ou condenar o submódulo. Foi criada a branch
`perf/baseline-78-observable` em `78c45bf`, com apenas a API de deadline
observável reaplicada (`8a20582`), seguida de rebuild `--clean-first`. Esse é o
A/B histórico válido com a integração Bellatrix atual.

Resultado: o Rigel histórico também não recuperou realtime e foi pior em
throughput: 0,97-1,13 M CCK/s, `rigel_ms=392-432`, 13-16 frames/s. O áudio foi
perceptualmente um pouco melhor, porém com batimento, coerente com produção
sub-real-time. Logo, os commits `78c45bf..cee4e0d` alteraram o comportamento
temporal/sonoro, mas não introduziram o gargalo principal observado hoje. A
branch temporária do A/B foi removida e o submódulo voltou para
`perf/observable-deadline-20260711` (`037e3bc`).

Não iniciar bisecção dentro do Rigel com base nesse resultado. Primeiro medir o
workspace Bellatrix histórico completo com o mesmo workload e relatório. Se nem
ele reproduzir ~50 FPS, não há commit golden demonstrado para bisectar; nesse
caso o caminho é profiling por domínio da integração atual.

## Causa raiz da "regressão": host contaminado por QEMUs órfãos

Todas as medições de 2026-07-11 foram feitas com o host degradado: dois
`qemu-system-aarch64` órfãos de uma sessão anterior (boot AROS/KS13 headless
com serial para log) consumiam ~340% de CPU cada — ~7 dos 8 cores do
i7-8550U — desde ~19:20, matando o turbo e estrangulando o harness. O load
average estava em ~11 durante os testes.

Após matar os órfãos, o mesmo binário baseline (`19e6ad5`, submódulo rigel
confirmado em `78c45bf`, KS1.3 + battle.adf):

```text
boot/título: 3,5-3,9 M CCK/s, present=50-55/s  (gate PAL atingido)
in-game:     2,6-3,1 M CCK/s, present=37-48/s  (steps sobem p/ 0,8-1,0 M/s)
```

Árvore atual (branch observable-deadline), mesmo workload, host limpo:

```text
boot/título: 3,8-4,3 M CCK/s, present=54-61/s
in-game:     2,8-3,0 M CCK/s, present=39-46/s, bus=0
             mmio_flush domina (~350-390k steps/s de 5-8 CCK)
```

Conclusões:

- Não houve regressão de código nem estado bom perdido sem commit; o baseline
  histórico é reproduzível e a árvore atual é igual ou levemente melhor.
- As comparações de 2026-07-11 feitas sob host contaminado (68000 A/B, rigel
  78c45bf A/B, workspace histórico) devem ser desconsideradas como medida
  absoluta; as relativas na mesma janela seguem indicativas.
- O gap real remanescente é só o in-game de Battle (~80-85% de realtime),
  agora dominado por `mmio_flush` com passos de 5-8 CCK — próximo alvo da
  Fase 3.
- Higiene obrigatória antes de qualquer benchmark: verificar `uptime`/load e
  processos QEMU/harness residuais. A linha `[HARNESS-PERF]` agora termina em
  `load=<loadavg 1min>`, carimbando as condições do host em cada medição.

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
