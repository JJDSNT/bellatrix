---
id: ISSUE-0039
title: Emu68 public API phase 1
status: superseded
priority: high
type: feature
owner: agent
created_at: 2026-07-09
updated_at: 2026-07-10
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
  - patches/0003-bellatrix-execution-loop.patch
  - patches/0007-bellatrix-boot-sequence.patch
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
- Com `BELLATRIX_EMU68_API_TRACE=1`, adapter imprime uma unica vez os primeiros
  acessos via dispatcher publico: primeiro read, primeiro write e primeiro
  `sync-required`.
- Com `BELLATRIX_EMU68_API_AUTODUMP=1`, adapter imprime uma linha de stats no
  primeiro `sync-required`, sem depender de ferramenta guest.
- Writes criticos custom/CIA retornam `EMU68_BUS_SYNC_REQUIRED`.
- Adicionadas estatisticas leves (`emu68_stats_t`) para reads/writes/sync/error/
  unhandled/unsupported/invalidate/stop.
- Adicionado controle temporario `0xDFFF08`:
  - `1`: dump stats
  - `2`: reset stats
  - `3`: dump e reset
- `emu68_run_cycles()` implementado como janela cooperativa sobre o
  `MainLoop()` real: `patches/0003-bellatrix-execution-loop.patch` chama
  `emu68_api_dispatch_quantum_progress(...)`, que retorna do loop quando o
  orcamento expira ou quando `emu68_request_stop()` e observado.
- `emu68_step()` usa uma janela minima de 8 ciclos estimados.
- Backend Emu68 conectado ao `CpuBackend`: `reset()` inicializa o contexto via
  `M68K_StartEmu()` e `run()` chama `emu68_run_cycles()`.
- Caminho Bellatrix em `M68K_StartEmu()` usa `M68KState` estatico e retorna
  antes do `MainLoop()` quando o backend e dono da execucao, evitando stack
  lifetime invalido e permitindo execucao por quantum.
- Documentado em `docs/emu68_public_api.md`.
- Consolidado em `AI_context/consolidated/emu68_public_api.md`.

# O que falta fazer

- Formalizar e testar o contrato de IRQ: no Bellatrix, `emu68_set_irq_level()` publica
  somente o nivel virtual em `M68KState.INT.IPL`; `INT.ARM` e PiStorm-only e deve ficar
  zero. Nao ha requisito de IRQ ARM fisica para IRQs guest. IRQs de device do host ficam
  fora do core/JIT Emu68 (polling no Core 3 hoje, arbitro/Core 0 no futuro se necessario).

- Fazer `EMU68_BUS_SYNC_REQUIRED` encerrar a janela ativa de `emu68_run_cycles()`
  como barreira real, nao apenas evento/contador. Writes como `DMACON`, `INTENA`,
  `INTREQ`, `COPJMP`, `BLTSIZE` e CIA devem cortar a janela para Rigel observar
  o efeito no ponto certo.
- Melhorar a precisao temporal de `emu68_run_cycles()`: hoje ciclos sao
  estimados a partir de instrucoes aposentadas (`v30 * 8`), suficiente para uma
  janela cooperativa mas ainda grosseiro para Paula/Copper/Blitter.
- Separar um bootstrap publico limpo de `M68K_StartEmu()` e reduzir dependencia
  do estado global do Emu68. O caminho atual e controlavel pelo `CpuBackend`,
  mas ainda nao e multi-instancia real.
- Definir contrato/range API para custom, CIA, autoconfig, Z2/Z3, overlay/ROM,
  com flags de side effect/no-inline/no-cache; hoje o fault path chama callback
  generico.
- Amarrar invalidacao JIT a writes reais de RAM/codigo/overlay/autoconfig quando
  necessario, e validar com self-modifying code/ROM patch.
- Validar o modelo em multicore: `run_cycles()` + progress publishing + MMIO
  barrier precisam ser provados com Core 1/2 sob contencao.
- Testar em hardware real; QEMU so valida bootstrap/sanity, nao timing real.
- Usar o controle `0xDFFF08` em uma execucao real para confirmar dump/reset via
  guest.
- Validar invalidação com self-modifying code/overlay/ROM patch.
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
- [x] Diagnostico confirma autodump de contadores no primeiro `sync-required`.
- [x] `emu68_run_cycles()` livewired ao `MainLoop()` por janela cooperativa.
- [x] Backend Emu68 dirigido pelo loop generico `CpuBackend.run()`.
- [x] QEMU curto com `aros.rom` entra no boot/JIT via backend Emu68 dirigido por
  `run_cycles()` e termina apenas pelo timeout planejado.
- Histórico (migrado para ISSUE-0051/0052): `EMU68_BUS_SYNC_REQUIRED` encerra janela ativa como barreira real.
- Histórico (migrado para ISSUE-0051/0052): Precisao temporal melhor que estimativa `v30 * 8`.
- Histórico (migrado para ISSUE-0051/0052): Range API publica definida.
- Histórico (migrado para ISSUE-0051/0052): Multicore validado com Emu68 API.
- Histórico (migrado para ISSUE-0051/0052): Hardware real validado.
- Histórico (migrado para ISSUE-0051/0052): Diagnostico confirma dump/reset por `0xDFFF08` em uma execucao real.
- Histórico (migrado para ISSUE-0051/0052): Invalidação validada contra caso real de codigo mutavel.
- Histórico (migrado para ISSUE-0051/0052): IPL 1..7/clear validado sem `INT.ARM` ou IRQ ARM fisica no Bellatrix.

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
- 2026-07-09: logs de primeira ocorrencia movidos para trace opt-in via
  `BELLATRIX_EMU68_API_TRACE=1`; builds normais seguem silenciosos exceto pelo
  log de registro da API e dumps explicitos.
- 2026-07-09: adicionado autodump opt-in via `BELLATRIX_EMU68_API_AUTODUMP=1`
  para validar contadores no primeiro `sync-required` sem ferramenta guest.
- 2026-07-09: QEMU/AROS com autodump observou
  `[EMU68-API] first-sync stats bus_r=1 bus_w=2 sync=1 err=0 unhandled=0
  bad_size=0 stop=0 inv=0`.
- 2026-07-09: `emu68_run_cycles()` passou a armar janela cooperativa no
  `MainLoop()` real via `emu68_api_dispatch_quantum_progress(...)`.
- 2026-07-09: backend Emu68 passou a implementar `CpuBackend.reset/run`; o
  loop generico do Bellatrix agora dirige o Emu68 por `emu68_run_cycles()`.
- 2026-07-09: QEMU curto com `aros.rom` validou boot ate JIT/API usando o novo
  caminho de backend; execucao terminou por timeout planejado.
