---
id: ISSUE-0046
title: "Estabilização da matriz de builds e regressões antes de novas arquiteturas"
status: doing
priority: critical
type: infra
owner: agent
created_at: 2026-07-11
updated_at: 2026-07-11
tags:
  - harness
  - singlecore
  - multicore
  - regression
  - stabilization
related_files:
  - tools/harness/CMakeLists.txt
  - src/machine/machine_rigel_step.c
  - scripts/build.sh
  - run.sh
---

# Estabilização da matriz de builds e regressões

## Objetivo

Restaurar uma baseline reproduzível antes de novos trabalhos em bare metal,
multicore ou Emu68. Nenhuma otimização de Rigel ou mudança de scheduler entra
sem validar harness, single-core e multicore.

## Decisão de 2026-07-11

A campanha exploratória de performance de ISSUE-0007 avançou rápido demais:
produziu medidas úteis, mas mudanças de scheduler/Rigel foram testadas apenas no
multicore KS13/Battle e invalidaram cargas previamente conhecidas. Todas as
otimizações experimentais e profiling interno do Rigel foram removidos. O
splitter original por deadline/bus foi restaurado. Foram preservadas somente
correções independentes:

- ownership/serialização USB do launcher/runtime;
- progressão do produtor PCM/HDMI após avanço do Rigel multicore;
- fallbacks seriais do harness para não depender de `core_io.c` bare-metal.

Na frente bare-metal escolhida em seguida, a topologia ativa passa a ser Core 0
supervisor+I/O, Core 1 CPU, Core 2 Rigel e Core 3 reservado. Essa mudança passa
pelos mesmos gates desta issue; o primeiro corte preserva as APIs e remove
somente o executor concorrente de I/O.

## Falha do harness e correção

`machine_rigel_step.c` referencia `core_io_serial_enqueue_tx/dequeue_rx`, mas o
harness POSIX não liga o runtime Core 3. Adicionados fallbacks fracos que retornam
fila vazia/indisponível; as definições fortes de `core_io.c` continuam vencendo
no Pi.

## Evidências atuais

- `cmake --build out/harness-rigel -j4`: passou;
- `ctest --test-dir out/harness-rigel --output-on-failure`: 34/34 passaram,
  incluindo KS13, KS20, KS31 e AROS;
- Musashi single-core + launcher + USB + HDMI: build passou;
- Musashi multicore + launcher + USB + HDMI: build passou;
- `git diff --check`: passa.

Após o primeiro corte Core0=supervisor+I/O, a mesma matriz local foi repetida:
harness e 34/34 testes passaram; Musashi 68040 single-core e multicore com
launcher, USB e HDMI compilaram. Permanece pendente a validação no Pi.

## Gates obrigatórios

- [x] Harness liga.
- [x] 34 testes do harness/Rigel passam.
- [x] Bare-metal Musashi single-core compila.
- [x] Bare-metal Musashi multicore compila.
- [ ] Bare-metal Emu68 single-core compila e boota carga conhecida.
- [ ] Bare-metal Emu68 multicore compila; execução só após backend estável.
- [ ] Pi: smoke single-core com KS13 e carga histórica.
- [x] Pi: smoke multicore com launcher + KS13.
- [ ] Documentar conjunto mínimo de ROM/ADF e resultados esperados.
- [ ] Automatizar a matriz num comando de CI local sem lançar GUI/QEMU.

## Política daqui em diante

Mudanças de arquitetura ficam atrás desta matriz. Performance do Rigel foi
documentada, não descartada, mas não será atacada enquanto os builds e loads de
referência não forem novamente reproduzíveis. Depois da estabilização, escolher
uma frente por vez: arquitetura bare-metal multicore ou contrato/integração
Emu68.
