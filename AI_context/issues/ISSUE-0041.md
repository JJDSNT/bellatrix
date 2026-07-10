---
id: ISSUE-0041
title: "Emu68 API: barreira real para SYNC_REQUIRED"
status: doing
priority: high
type: bug
owner: agent
created_at: 2026-07-10
updated_at: 2026-07-10
tags:
  - emu68
  - api
  - jit
  - synchronization
related_files:
  - src/cpu/emu68/emu68_api.h
  - src/cpu/emu68/emu68_api_adapter.c
  - patches/0003-bellatrix-execution-loop.patch
  - emu68/src/ExecutionLoop.c
---

# Objetivo

Fazer um acesso de bus que retorna `EMU68_BUS_SYNC_REQUIRED` encerrar a janela ativa de
`emu68_run_cycles()` com `EMU68_STOP_SYNC_REQUIRED`, no primeiro boundary seguro do
`MainLoop` apos o retorno do fault handler.

# Invariantes

- O acesso MMIO termina antes de a janela retornar.
- Nao se faz longjmp/return atraves do fault handler.
- O JIT retorna somente pelo `MainLoop`, com contexto pinado restaurado.
- O boundary nao depende do gate normal de 64 instrucoes/dispatches.
- `detail` identifica o endereco que pediu sincronizacao.
- Fora de uma janela ativa, `SYNC_REQUIRED` continua sendo apenas evento/contador.
- Nenhuma IRQ ARM fisica participa deste mecanismo.

# Plano

1. Registrar `sync_pending` no adapter quando o callback de bus pedir sincronizacao.
2. Expor ao patch do `MainLoop` uma consulta interna de baixo custo.
3. Forcar o hook de progresso no primeiro dispatch seguinte ao fault.
4. Retornar `EMU68_STOP_SYNC_REQUIRED`, preservando ciclos, PC e endereco.
5. Atualizar patch aplicado, documentacao e verificacao do setup.
6. Validar build, QEMU curto e contadores/motivo de parada.

# Criterios de aceite

- [x] Build Emu68 passa.
- [x] `scripts/setup.sh --verify` passa.
- [x] Write critico retorna `EMU68_STOP_SYNC_REQUIRED` na janela ativa.
- [x] A janela seguinte continua a partir do PC correto.
- [ ] Boot KS13/ADF e AROS nao regridem.
- [ ] Multicore Core 1/Core 2 respeita o mesmo boundary.

# Log de execucao

- 2026-07-10: `sync_pending` implementado no adapter; o patch do `MainLoop` ignora o
  gate normal e chama o hook no primeiro dispatch seguro apos o fault.
- 2026-07-10: build minimo passou com launcher ativo e USB/BT/HDMI/multicore desligados.
- 2026-07-10: smoke QEMU de 90 s com KS13 + `wb13.adf` registrou API, inseriu DF0,
  entrou no JIT e trocou overlay sem reset/crash imediato. Nao chegou a hand screen no
  limite TCG; boot por disco permanece pendente de uma execucao mais longa e comparacao
  com o baseline anterior.
- 2026-07-10: a primeira tentativa revelou que `MainLoop()` corrompia o retorno C:
  upstream o tratava como non-returning e usava x19-x29/q8-q15 para estado guest. O
  wrapper Bellatrix-owned `mainloop_window.S` passou a preservar o banco callee-saved
  host ao redor de cada janela.
- 2026-07-10: DiagROM com trace provou retorno limpo: `reason=1`
  (`EMU68_STOP_SYNC_REQUIRED`), PC valido, 8 ciclos estimados, seguido de progresso
  normal pelos testes de ROM, overlay e chip RAM. Multiplos pedidos antes do boundary
  preservam o primeiro endereco como causa em `detail`.
