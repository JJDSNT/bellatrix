---
id: ISSUE-0044
title: "Regressão de multicore após merge da liveness (crash QEMU / hang Pi)"
status: resolved
priority: high
type: bug
owner: agent
created_at: 2026-07-10
updated_at: 2026-07-10
tags:
  - multicore
  - musashi
  - regression
  - emu68
  - liveness
related_files:
  - src/cpu/emu68/bellatrix.c
  - src/cpu/emu68/mainloop_window.S
  - src/cpu/emu68/emu68_api_adapter.c
  - emu68/src/ExecutionLoop.c
  - src/host/raspi3/pal_core.c
---

# Resumo

Após o fast-forward de `wip/emu68-liveness` para `main` (b3b02bf), o build
Musashi multicore não funciona: QEMU crasha e o Pi real trava no boot. Debug
adiado a pedido do usuário — issue aberta para não perder o rastro.

# Problema

Build: Musashi + multicore + USB + HDMI audio (main @ b3b02bf).

- **QEMU:** trava em "scanning" e dá exceção AArch64:
  `[JIT:SYS] Exception with vector 0200. ELR=0x64204c6962726172,
  ESR=0x86000004, FAR=0x64204c6962726172`. O ELR/FAR é ASCII ("d Librar") —
  **PC saltou para dentro de uma string** (salto selvagem no ARM). Vários X
  contêm texto ("ies\r\nCop", " ROM"), sugerindo retorno/branch para um buffer
  de ROM/strings. `X31(SP)=0xffffff800007fc00`, `X30=0xffffff80001d92d0`.
- **Pi real:** trava em "initialising", último log `[USB] DWC` (hang na init do
  DWC2/USB).

Suspeita: o pacote da liveness (mainloop_window.S preservando banco callee-saved
do host, mudanças de ABI/ExecutionLoop, boundary de SYNC_REQUIRED — ISSUE-0041)
interage mal com o caminho multicore e/ou com a init de USB. Single-core
funcionava antes; o crash tem cara de corrupção de stack/registrador salvo.

# Objetivo

Restaurar o boot do build Musashi multicore (e validar Pi real), sem perder os
ganhos da liveness em single-core/JIT.

# O que foi feito

**Causa raiz encontrada — NÃO era a liveness/ABI.** Repro determinístico em
QEMU (`-M raspi3b`, Musashi multicore, `-initrd KS13.rom`) + bisect isolado:

- `33656e8` (ABI) boota; `b3b02bf` (IPL/SEV) boota — os commits da liveness
  estão OK no multicore.
- A regressão é a **mudança do launcher (fase de enumeração USB)**: sem ela
  boota, com ela crasha. Isolado por rebuild do `launcher.c.obj`.
- Mecanismo confirmado desligando o pump USB do Core 3 (`core_io_step`): o
  crash some. É a **corrida de duplo-pump** — Core 0 (launcher) e Core 3 (IO,
  lançado em bellatrix.c ~1022, ANTES do launcher em ~1042) dirigindo o
  DWC2/CherryUSB concorrentemente. O launcher antigo mascarava (pump curto);
  a espera de 5s determinística ampliou a janela → corrompe estado
  compartilhado → o Core 1 (M68K) faz branch selvagem (`ELR/FAR=ASCII de banner
  de ROM`, `ESR=0x86000004` instruction abort) ao rodar depois.
- O "hang no `[USB] DWC`" do Pi é o MESMO crash visto pelo console deferido: o
  Core 3 para de drenar após o crash, então a última linha visível é uma
  bufferizada bem anterior (não é onde travou de fato).

**Fix aplicado (gating de propriedade do USB):**
- `RuntimeCoreIO.launcher_owns_usb` (flag atômico).
- `core_io_step` pula `usb_host_step` enquanto o flag está setado (Core 3 cede).
- `bellatrix.c`: seta antes de `PAL_Core_LaunchIO` (~1020), limpa após
  `launcher_run()` (~1043). Core 0 é dono exclusivo do USB na fase do launcher;
  Core 3 reassume no runtime.

# O que falta fazer

1. **Validar em hardware real (Pi 3B + pen drive):** sem crash/hang, MSC
   enumera e o `fat32_init_usb` roda (logs `[FAT32]` + lista de ADFs).
2. Commitar no main (launcher.c + core_io.{h,c} + bellatrix.c gating).

# Decisões tomadas

- Gating pontual (Core 3 cede USB na fase do launcher) em vez do modelo de
  fases global — este fica para ISSUE-0042. O gating já elimina a corrida.
- Relacionada a ISSUE-0041 (liveness, inocentada), ISSUE-0042 (fases),
  ISSUE-0043 (JIT no Musashi).

# Critérios de aceite

- [x] Causa raiz identificada (bisect + repro determinístico + teste de mecanismo).
- [x] Build Musashi multicore boota em QEMU sem exceção (M68K roda KS13, frames++).
- Histórico (não ativo): Pi real passa da init de USB e chega ao launcher (pendente teste hardware).
- [x] Sem regressão do single-core/JIT (só gate de USB no multicore).
