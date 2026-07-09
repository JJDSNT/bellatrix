---
id: ISSUE-0039
title: Emu68 public API phase 1
status: doing
priority: high
type: feature
owner: agent
created_at: 2026-07-09
updated_at: 2026-07-09
tags:
  - emu68
  - api
  - jit
  - integration
related_files:
  - src/cpu/emu68/emu68_api.h
  - src/cpu/emu68/emu68_api_adapter.c
  - src/cpu/emu68/bellatrix.c
  - cmake/bellatrix-variant.cmake
  - patches/0021-emu68-public-bus-dispatch.patch
  - docs/emu68_public_api.md
  - AI_context/consolidated/emu68_public_api.md
---

# Resumo

Criar a primeira fase da API publica Bellatrix/Emu68, preservando o live path
atual e mantendo o submodulo Emu68 alterado apenas por patches.

# Problema

A integracao Emu68 atual funciona por hooks diretos em `vectors.c`,
`ExecutionLoop.c` e estado global. Isso e pragmatico para boot, mas deixa a
fronteira publica indefinida e dificulta evoluir para execucao por janela,
sync boundary e diagnosticos consistentes.

# Objetivo

Introduzir uma API pequena e honesta para:

- bus externo;
- IRQ;
- estado minimo;
- invalidacao de codigo;
- eventos;
- estatisticas leves de uso;
- reserva explicita para execucao por janela futura.

# O que foi feito

- Criado `src/cpu/emu68/emu68_api.h`.
- Criado `src/cpu/emu68/emu68_api_adapter.c`.
- Registrado o adapter em `cmake/bellatrix-variant.cmake`.
- Criado `patches/0021-emu68-public-bus-dispatch.patch`.
- `vectors.c` passa por `emu68_api_dispatch_bus_access(...)` antes do fallback
  `bellatrix_bus_access(...)`.
- `src/cpu/emu68/bellatrix.c` registra callbacks de bus na API.
- Boot imprime `[EMU68-API] v1 bus registered` quando a API cria o singleton e
  registra os callbacks.
- Adapter imprime uma unica vez os primeiros acessos via dispatcher publico:
  primeiro read, primeiro write e primeiro `sync-required`.
- Writes criticos custom/CIA retornam `EMU68_BUS_SYNC_REQUIRED`.
- Adicionadas estatisticas leves (`emu68_stats_t`) para reads/writes/sync/error/
  unhandled/unsupported/invalidate/stop.
- Adicionado controle temporario `0xDFFF08`:
  - `1`: dump stats
  - `2`: reset stats
  - `3`: dump e reset
- Documentado em `docs/emu68_public_api.md`.
- Consolidado em `AI_context/consolidated/emu68_public_api.md`.

# O que falta fazer

- Usar o controle `0xDFFF08` em uma execucao real para confirmar contadores.
- Validar invalidação com self-modifying code/overlay/ROM patch.
- Projetar `run_cycles()` real.
- Fazer `EMU68_BUS_SYNC_REQUIRED` parar uma janela quando `run_cycles()` existir.
- Avaliar integração gradual com `CpuBackend`.
- Adiar HLE ate lifecycle/run ficarem claros.

# Decisões tomadas

- API/adapter vivem no lado Bellatrix, não no submodulo Emu68.
- O patch `0002` continua sendo o bus hook original.
- O patch `0021` e apenas a camada publica por cima do hook de `0002`.
- `temp_aemu68_api.md` e rascunho temporario e nao deve ser tratado como doc.
- Documento formal fica em `docs/emu68_public_api.md`.

# Critérios de aceite

- [x] Build Emu68/Bellatrix passa.
- [x] `scripts/setup.sh --verify` passa.
- [x] API publica compila fora do submodulo.
- [x] Patch Emu68 novo e minimo.
- [x] AI context registra o que, por que e para que.
- [x] Stats podem ser dumpados/resetados por controle explicito.
- [x] QEMU sem launcher chega ao JIT com a API registrada no boot.
- [x] QEMU com `src/roms/aros.rom` confirma read/write/sync pelo dispatcher.
- [ ] Diagnostico confirma dump de contadores por `0xDFFF08` em uma execucao real.
- [ ] Invalidação validada contra caso real de codigo mutavel.

# Observações

`wip/emu68-liveness` tambem toca parte da historia de liveness/diagnostico
relacionada aos patches 0019/0020, mas nao foi integrado automaticamente aqui.
Esta issue foca a API publica incremental.

# Log de execução

- 2026-07-09: API publica inicial criada fora do submodulo.
- 2026-07-09: `0021` criado para dispatch publico no fault path.
- 2026-07-09: docs e AI context atualizados.
- 2026-07-09: estatisticas leves adicionadas ao adapter.
- 2026-07-09: controle `0xDFFF08` adicionado para dump/reset dos stats.
- 2026-07-09: `BELLATRIX_LAUNCHER=0` validado no QEMU; log
  `[EMU68-API] v1 bus registered` observado antes do JIT.
- 2026-07-09: QEMU com `src/roms/aros.rom` observou
  `[EMU68-API] first bus write`, `[EMU68-API] first bus read` e
  `[EMU68-API] first sync-required`; AROS chegou ao serial de resident modules,
  ROMInfo 1MiB e autoconfig Z2 Fast RAM.
