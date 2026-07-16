---
id: ISSUE-0062
title: "Host reactor (Core 3) sem heartbeat periódico após o rebaseline de topologia"
status: open
priority: low
type: observability
owner: agent
created_at: 2026-07-16
updated_at: 2026-07-16
tags: [multicore, reactor, logging, observability, topology]
related_files:
  - src/host/raspi3/pal_core.c
  - AI_context/consolidated/multicore_topology.md
  - AI_context/issues/ISSUE-0058.md
---

# Contexto

Na topologia legada (`Core0=Supervisor/IO Core1=CPU Core2=Chipset
Core3=Reserved`, comentário ainda presente em `scripts/build.sh`), Core 0
imprimia um heartbeat periódico `[CORE0-SUP] beat=... cpu_target=...
chipset=... backlog=... frames=... pc=...` (introduzido em `1473b24`,
"multicore: bound CPU↔chipset backlog + Core 0 supervisor") a cada ~2s,
junto com `[CORE0-IO] calls=... budget_miss=...`.

O rebaseline de topologia (`7b4f7c9`, 2026-07-15, "rebaseline: keep native
Emu68 on core 0") moveu a CPU para o Core 0 e o papel de host reactor para
o Core 3 (topologia atual documentada em
`AI_context/consolidated/multicore_topology.md`). `CORE0-SUP`/`CORE0-IO`
não existem mais em lugar nenhum de `src/` — confirmado por
`git log -S"CORE0-SUP"`, que só aparece em `1473b24` (introdução) e
`8aa0bed` (menção incidental num commit WIP desta sessão).

O `host_reactor_loop()` novo (`src/host/raspi3/pal_core.c:405`) só imprime
**uma vez**, no start: `[HOST] Core %u reactor event stream: %u Hz`. Não há
heartbeat periódico equivalente ao antigo `[CORE0-SUP]`.

# Por que importa

Durante a investigação do ISSUE-0061 (regressão de boot emu68), o Jaime
notou a ausência do heartbeat comparando com o que lembrava de sessões
anteriores. A ausência **não é bug nem regressão desta sessão** — é
consequência direta do rebaseline de topologia de três dias atrás, que
trocou o papel do reactor de core sem portar o heartbeat periódico junto.
Sem ele, fica mais difícil confirmar de relance que o host reactor (Core 3:
USB, BT, miniUART, apresentação, timeline) está vivo e avançando durante um
boot, especialmente numa investigação de regressão como a do ISSUE-0061.

# Proposta (não implementada)

Adicionar ao loop principal de `host_reactor_loop()` um print periódico
(gate por tempo ou por contagem de iterações, seguindo o padrão do antigo
`[CORE0-SUP]`) reportando pelo menos: contagem de `bellatrix_runtime_io_step`
chamadas, último `now` (cntpct), e talvez o mesmo `frames`/`insn` que
`[EMU68-LIVE]` já reporta do lado CPU, para dar visão comparável dos dois
lados sem precisar cruzar dois mecanismos de log diferentes.

Prioridade baixa — não bloqueia nada, é puramente observabilidade.
