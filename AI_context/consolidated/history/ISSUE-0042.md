---
id: ISSUE-0042
title: "Fases de boot bem definidas (launcher single-core / runtime multicore)"
status: superseded
priority: medium
type: refactor
owner: user
created_at: 2026-07-10
updated_at: 2026-07-10
tags:
  - boot
  - phasing
  - multicore
  - launcher
  - logging
related_files:
  - src/cpu/emu68/bellatrix.c
  - src/host/raspi3/pal_core.c
  - src/runtime/core_io.c
  - src/host/raspi3/console_log.c
  - src/launcher/launcher.c
---

# Resumo

Tornar as fases de boot explícitas e determinadas, com logs que representem
fielmente o que ocorre. Trabalho mais intrusivo, adiado a pedido do usuário.

# Problema

O faseamento atual é implícito e enganoso:

- `PAL_Core_LaunchIO()` (bellatrix.c ~1001) chama `pal_runtime_init_once()`, que
  seta `runtime_ready`, acordando o Core 3 **antes** do `launcher_run()`.
- A linha `[BELA] Initialized (multicore enabled)` (~1047) imprime **depois** do
  launcher, dando a falsa impressão de que o multicore começa depois.
- O console é deferido (ring por-core drenado pelo Core 3); a ORDEM no serial
  não reflete a ordem real dos eventos. Isso induziu diagnóstico errado no
  debug do launcher-USB (2026-07-10).

# Objetivo

Modelo de fases explícito:

- Fase 0 — early boot (Core 0).
- Fase 1 — host services (USB/BT/HDMI-audio) — Core 0.
- Fase 2 — launcher (pré-boot, Core 0 exclusivo dono do USB/BT).
- Fase 3 — runtime (multicore) — só aqui se lança Core 1/2/3.

# O que foi feito

Nada ainda (backlog). Precursor parcial: a fase de enumeração determinística do
launcher (working tree, ver ISSUE do launcher-USB / memória).

# O que falta fazer

1. Enum/estado de fase com marcadores `[PHASE] <nome> t=<ms>` (timestamp real
   via `PAL_Time`, sem confiar na ordem do console deferido).
2. Mover `PAL_Core_LaunchChipset/IO` para **depois** do launcher (antes de
   `core_chipset_init`), tornando o launcher comprovadamente single-core.
3. Launcher drena o próprio console (`console_log_drain()` no loop de pump), já
   que o Core 3 não estará vivo na fase 2.
4. Corrigir `[BELA] Initialized (multicore)` para imprimir no momento real do
   lançamento dos cores.
5. Validar USB/BT/serial em cada fase.

# Decisões tomadas

- Preferir fronteira de fase explícita a gate por-iteração (flag no hot loop).
- Adiado por ser mais intrusivo; o fix imediato do launcher usou espera
  determinística sem reordenar fases.

# Critérios de aceite

- Histórico (não ativo): Fases explícitas com marcadores timestampados.
- Histórico (não ativo): Launcher roda com Core 3 comprovadamente parado (`runtime_ready==0`).
- Histórico (não ativo): Logs em ordem/tempo fiéis (independem da ordem de drenagem).
- Histórico (não ativo): Sem regressão de USB/BT/serial no boot.
