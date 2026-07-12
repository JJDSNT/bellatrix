---
id: ISSUE-0047
title: "Backlog de otimização do Rigel — resultados e experimentos multicore revertidos"
status: superseded
priority: medium
type: research
owner: agent
created_at: 2026-07-11
updated_at: 2026-07-11
tags:
  - rigel
  - performance
  - agnus
  - denise
  - scheduler
  - multicore
related_files:
  - external/rigel/src/core/rigel.c
  - external/rigel/src/chipset/chipset.c
  - external/rigel/src/chipset/agnus/agnus.c
  - external/rigel/src/chipset/agnus/timing/slot_scheduler.c
  - external/rigel/src/chipset/denise/denise.c
  - external/rigel/src/chipset/denise/render/compositor.c
  - src/runtime/core_chipset.c
---

# Backlog de otimização do Rigel

## Propósito

Preservar o conhecimento obtido na campanha experimental de performance de
2026-07-10/11. Todo o código descrito aqui foi revertido porque a validação ficou
restrita a KS13/Battle multicore e outras cargas históricas deixaram de
funcionar. Os resultados continuam úteis como mapa para uma futura campanha,
depois da estabilização de `ISSUE-0046`.

Este documento não autoriza reaplicar o conjunto inteiro. Cada hipótese deve
voltar isoladamente, atrás de flag, com harness + single-core + multicore e a
matriz de loads conhecidos.

## Baseline medido

Configuração:

- Raspberry Pi 3B;
- Musashi 68040;
- multicore: Core 1 CPU, Core 2 Rigel, Core 3 I/O;
- launcher + KS13 + Battle Squadron;
- HDMI áudio ativo;
- USB sem logs e sem MSC durante gameplay.

Primeiro perfil, antes dos fast paths:

```text
13,973 s, 77 frames, 5,51 FPS
5.478.238 CCK avançados
97.827 rigel_step_until, média 55 CCK
backpressure Core 1: 12,804 s
CPU lock wait: 830 ms
critical catch-up: 47 ms
```

Em gameplay, o problema se amplificou:

```text
222,968 s, 710 frames, 3,18 FPS
50.324.861 CCK
13.092.140 steps, média 3 CCK
9.318.517 steps <= 1 CCK
12.721.288 steps <= 8 CCK
backpressure: 184,808 s
audio produzido/consumido: 3% de realtime
9.506.965 underruns
```

Conclusão confirmada: CPU não era o limitante. Core 1 passava a maior parte do
tempo esperando Core 2. Emu68 mais rápido ou outro modelo Musashi apenas
alcançaria o teto de backlog mais cedo.

## Experimento 1 — remover o splitter externo de deadlines

### Problema encontrado

Instrumentação classificou o limitador de cada step:

```text
target:      31.813
deadline: 13.056.073
bus:          4.255
```

99,7% dos steps eram cortados por `rigel_get_next_deadline()`. Ao mesmo tempo,
`rigel_chipset_step()` já chama o slot scheduler interno, que percorre cada CCK.
Isso indicou double scheduling: Bellatrix fragmentava externamente nos mesmos
eventos que Rigel processava internamente.

### Mudança experimental

No Core 2, avançar `rigel_step_until()` diretamente até o target publicado pela
CPU, sem aplicar `rigel_get_next_deadline()`/`rigel_get_next_bus_change()` no
loop externo. O catch-up antes de MMIO crítico foi preservado.

Macro usada:

```c
BELLATRIX_MULTICORE_EXTERNAL_DEADLINE_SPLIT=0
```

### Resultado

```text
3,18 -> 4,56 FPS (+43%)
13.092.140 -> 69.855 steps
média 3 -> 712 CCK/step
```

KS13/Battle continuou funcional no teste observado. Entretanto, passos maiores
seguraram o lock por mais tempo:

```text
chipset_step médio: 1,65 ms
CPU lock wait: 20,3 s
```

### Risco a validar

`rigel_step()` reporta eventos comparando estado antes/depois do bloco. Cruzar
vários eventos num único bloco pode colapsar transições intermediárias de IPL,
frame, Copper ou blitter. Antes de retomar, criar testes para eventos múltiplos
dentro de uma janela e experimentar um chunk máximo (por exemplo 64/128 CCK),
em vez de avançar todo o backlog.

## Experimento 2 — corrigir a progressão de áudio multicore

O primeiro profile mostrou `audio produced=0`. Mesmo antes de o jogo produzir
som, o pipeline de 48 kHz deveria enfileirar silêncio. A causa era assimetria:
`machine_quantum_step()` single-core chamava `bellatrix_audio_output_tick()` e
`hdmi_audio_dma_poll()`, enquanto Core 2 multicore não chamava.

Foi criada a integração:

```c
bellatrix_machine_on_chipset_advanced(cck_cycles)
```

Essa é uma correção funcional, não uma otimização, e foi preservada após o
rollback. Depois dela, produção PCM passou a acompanhar a velocidade emulada:
3–5% de realtime nos runs lentos, confirmando que áudio era vítima do throughput.

## Experimento 3 — profiling interno por domínio

Timers em torno dos blocos de `rigel_chipset_step()` separaram Agnus, Paula e
CIA sem medir cada CCK.

Resultado no frame 718:

```text
Agnus: 119.833 ms (99%)
Paula:      54 ms
CIA:        48 ms
```

Conclusão: Paula, CIA, HDMI, USB e CPU não explicavam o déficit principal.
O hot path era `rigel_agnus_step()` → `agnus_slot_scheduler_step_until()`.

## Experimento 4 — amostragem do slot scheduler

Foi amostrado 1 em cada 1024 CCK, com timestamps ao redor das fases do slot:

```text
prepare/arbitragem: 25%
dispatch_slot:      15%
beam:                7%
Denise/compositor:  47%
tail de linha:       3%
```

A amostragem era útil no baseline, mas depois de reduzir a frequência da Denise
os percentuais ficaram contaminados pelo custo dos próprios `CNTPCT`. Numa
retomada, preferir contadores amostrados por bloco ou PMU, e medir overhead do
profiler com uma imagem controle.

## Experimento 5 — Denise somente na mudança de scanline

### Observação

`rigel_denise_step()` era chamado a cada CCK. Porém
`rigel_denise_compositor_tick()` só chama `compose_line()` quando `vpos/frame`
muda; nos demais CCK repetia comparações e sincronização/cópia de estado de
beam/debug.

### Mudança experimental

No slot scheduler:

```c
if (ctx && beam->hpos == 0)
    rigel_denise_step(...);
```

O modo antigo permaneceu selecionável por:

```c
RIGEL_DENISE_STEP_EVERY_CCK
```

### Resultado

```text
4,43 -> 5,64 FPS (+27%)
Agnus: 120,8 -> 86,7 s (-28%)
tempo até ~frame 710: 158,6 -> 125,9 s
```

KS13/Battle não apresentou mudança visual óbvia. Ainda assim, cargas antigas
regrediram durante a campanha e não foi isolado qual experimento causou isso.

### Risco a validar

Campos como `beam_hpos`, `current_pixel`, estado de debug e qualquer leitura
mid-scanline podem depender da cadência por CCK, mesmo que composição seja por
linha. A solução futura mais segura é separar APIs:

```text
denise_step_beam(cck/run)       — estado temporal barato
denise_compose_line()           — trabalho pesado na fronteira de linha
denise_debug_snapshot()         — somente quando solicitado
```

Não apenas eliminar chamadas.

## Experimento 6 — cache de derivados de DMACON

O prepare relia DMACON e recalculava `copper_active` e `blitter_nasty` em todo
CCK, apesar de writes em DMACON já chamarem
`agnus_slot_scheduler_invalidate()`.

Mudança: atualizar derivados no ponto de invalidação e manter apenas
`blitter_active` dinâmico por CCK.

Resultado pequeno, porém positivo:

```text
5,64 -> 5,77 FPS
Agnus: 86,7 -> 83,8 s
```

Risco: existem alterações internas de DMACON, como restauração de DSKEN durante
DMA de disco. Todo produtor de mudança deve passar pelo mesmo helper antes de
remover a leitura defensiva do hot path.

## Experimento 7 — coalescência de slots CPU/FREE

Foi implementado, mas revertido antes de receber resultado do Pi.

Ideia:

- detectar runs de slots `CPU/FREE` dentro da mesma scanline;
- agrupar somente sem Copper, blitter ou override de disco;
- parar antes de refresh/DMA/bitplane/sprite;
- deixar o último CCK da linha no caminho escalar para preservar modulo, VBL,
  sprite reset e composição.

Macro:

```c
RIGEL_COALESCE_IDLE_SLOTS
```

Riscos: eventos implícitos por CCK, observabilidade do beam, mudanças de estado
no meio do run e interação com bus arbitration. Antes de retomar, modelar uma
API formal `rigel_next_observable_tick()` e testar equivalência step-by-step
versus coalescido em snapshots de estado.

## O que foi preservado no código

Somente `bellatrix_machine_on_chipset_advanced(cck_cycles)`, pois corrige a
ausência de produção PCM/HDMI no caminho multicore. Todos os fast paths,
alterações do submódulo e profiling desta issue foram removidos.

## Plano seguro para retomar

### Fase 0 — pré-condições

- `ISSUE-0046` concluída;
- matriz automatizada com harness, single-core e multicore;
- cargas mínimas: KS13, KS20, KS31, AROS, Battle e loads históricos que
  regrediram;
- screenshots/hashes e checkpoints de boot definidos.

### Fase 1 — profiling sem mudança funcional

- reintroduzir somente profiling por domínio/bloco;
- medir single-core e multicore;
- quantificar overhead com build controle;
- não editar scheduler.

### Fase 2 — separar Denise temporal de composição

- criar contrato explícito para estado mid-line;
- mover composição pesada para line boundary;
- testes de equivalência por scanline, sprites, Copper e scroll;
- um commit isolado e reversível.

### Fase 3 — granularidade Bellatrix↔Rigel

- testar chunk máximo controlado (16/32/64/128 CCK);
- verificar eventos múltiplos e IPL transitório;
- medir throughput e lock hold;
- não remover deadlines sem contrato de eventos agregados.

### Fase 4 — cache e fast-forward interno

- centralizar mutações de DMACON;
- expor `next_observable_tick` formal;
- implementar runs ociosos com comparação diferencial contra stepping escalar.

## Critérios de aceite futuros

- Histórico (não ativo): 34/34 testes continuam passando.
- Histórico (não ativo): Harness KS13/20/31/AROS mantém checkpoints.
- Histórico (não ativo): Single-core não muda funcionalmente nem perde loads conhecidos.
- Histórico (não ativo): Multicore mantém launcher, input, IRQ, Copper, sprites, blitter e disco.
- Histórico (não ativo): Estado escalar e otimizado coincide nos ticks observáveis.
- Histórico (não ativo): Áudio produzido acompanha realtime apenas quando emulação acompanha.
- Histórico (não ativo): Ganho é medido em build release e supera o overhead/risco introduzido.
- Histórico (não ativo): Cada otimização pode ser ligada/desligada independentemente.

## Relações

- `ISSUE-0046`: gate obrigatório de estabilização.
- `ISSUE-0007`: arquitetura do árbitro e histórico da campanha multicore.
- `ISSUE-0045`: arquitetura futura de I/O, ortogonal ao throughput do Rigel.
- `issue_paula_audio_timing.md`: áudio como consumidor de tempo emulado.
