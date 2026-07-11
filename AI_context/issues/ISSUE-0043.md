---
id: ISSUE-0043
title: "JIT/ExecutionLoop acoplado no caminho do build Musashi"
status: backlog
priority: medium
type: refactor
owner: user
created_at: 2026-07-10
updated_at: 2026-07-10
tags:
  - jit
  - musashi
  - emu68
  - architecture
related_files:
  - emu68/src/ExecutionLoop.c
  - src/cpu/emu68/mainloop_window.S
  - src/cpu/emu68/emu68_api_adapter.c
  - emu68/src/aarch64/vectors.c
---

# Resumo

Desacoplar a infraestrutura do JIT do caminho do interpretador Musashi. Mesmo em
`BELLATRIX_CPU_BACKEND=musashi` aparecem logs/caminhos do JIT/Emu68. Trabalho
arquitetural, para o futuro (o JIT é meio acoplado com a infraestrutura).

# Problema

- No build Musashi multicore (2026-07-10) o serial mostra `[JIT:SYS] Exception
  with vector 0200 ...` — o dumper de exceção AArch64 do Emu68 está no caminho
  ativo (ex.: `ELR=0x64204c6962726172`, ASCII "d Librar", PC saltou para dentro
  de uma string).
- `ExecutionLoop.c`, `mainloop_window.S` e o adapter da API Emu68 são
  compilados/linkados no build Musashi. Handler de bus/exceção e vetores AArch64
  são compartilhados (patches em `vectors.c`).

# Objetivo

Clareza e isolamento: saber o que do JIT é realmente exercido no caminho Musashi
e não atribuir ao JIT o que é infraestrutura compartilhada.

# O que foi feito

Nada ainda (backlog). Apenas observação registrada.

# O que falta fazer

1. Mapear quais símbolos do caminho JIT são exercidos no build Musashi vs apenas
   linkados.
2. Isolar o dumper de exceção ARM sob rótulo neutro (não `[JIT:SYS]`) quando o
   backend é interpretador.
3. Avaliar guardas de compilação para excluir ExecutionLoop/mainloop_window do
   build Musashi puro, ou documentar por que precisam existir.

# Decisões tomadas

- Adiado para o futuro; o JIT está acoplado com bus/IPL/exceção e separar é
  esforço arquitetural.

# Critérios de aceite

- [ ] Mapeamento do que do JIT é usado no caminho Musashi.
- [ ] Logs de exceção não atribuídos ao JIT quando irrelevante.
- [ ] Decisão documentada: desacoplar vs manter (com justificativa).
