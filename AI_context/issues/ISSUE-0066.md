---
id: ISSUE-0066
title: Remover as registries legacy zorro2/zorro3 (consolidar no board_registry)
status: done
priority: medium
type: refactor
owner: agent
created_at: 2026-07-18
updated_at: 2026-07-18
tags:
  - boards
  - legacy-removal
  - zorro
blockers:
  -
related_files:
  - src/machine/bus/zorro2/zorro2_bus.c
  - src/machine/bus/zorro3/zorro3.c
  - src/machine/bus/zorro_autoconfig.c
  - src/machine/memory/memory_map.c
  - src/machine/machine_rigel.c
  - src/machine/machine_rigel_bus.c
  - cmake/bellatrix-variant.cmake
  - tools/harness/CMakeLists.txt
---

# Resumo

Depois que lide (EXTERNAL Z2) e RTG (EXTERNAL Z3) passaram a ser servidos pelo
`board_registry` + `expansion.c` bus_ops, as registries legacy de AutoConfig
Zorro II/III (`zorro2_bus.c`, `zorro3.c`) ficaram sem nenhum consumidor de board
no bus vivo. Esta issue remove esse código morto e consolida no board_registry.

**Escopo (confirmado com Jaime):** deletar as registries legacy. **NÃO** deletar o
RTG — VideoCore.card (o equivalente alvo no Emu68) só existe no Raspberry, então o
RTG será evoluído em sessão específica; ele já está migrado para board_registry Z3
e permanece.

# Problema

O bus vivo já foi desacoplado do legacy (commit 366720a / merge 4f2f071), mas os
arquivos legacy continuam compilados e entrelaçados em vários pontos:

- `zorro_autoconfig.c` — provê o predicado da janela AutoConfig `$E80000`
  (`in_window`, ainda necessário) MAS suas funções de read/write (mortas, zero
  callers) dependem de `zorro2/zorro3_has_pending_board()`.
- `memory_map.c` — bloco `BELLATRIX_ROUTE_Z2_AUTOCONFIG` (harness OU modo legacy)
  roteia Z2 via `zorro2_bus.c`. Redundante: `machine_dispatch` já trata Z2 antes de
  chegar no `memory_map` (fallback), nos DOIS backends (emu68 via `cpu_bridge`,
  musashi via `musashi_backend`, ambos → `bellatrix_machine_read/write` →
  `machine_dispatch`).
- `machine_rigel.c` — chama `zorro2/zorro3_init()` e `zorro2/zorro3_reset()`.
- `superbuster.c` — mantém fallback `zorro3_in_board_window` (só o teste usa).
- Includes soltos em `bellatrix.c`, `memory.c`, `machine_rigel_bus.c`, `main.c`.
- `lide_cdrom.c` usa `BELLATRIX_ZORRO2_WIN_128KB` de `zorro2_bus.h` (único uso vivo
  desse header) — precisa relocar o macro.
- Testes: `test_zorro_autoconfig.c` exercita a registry legacy (assunto deletado;
  cobertura equivalente em `test_board_registry`); `test_superbuster_z3.c` tem um
  caso legacy zorro3 além do caso board_registry novo.
- CMake: `zorro2_bus.c` aparece 2x no produto (BASE_FILES linha 205 + branch legacy
  linha 289 = duplicado); presente em vários alvos do harness.

# Objetivo

`board_registry` como única autoridade de AutoConfig e de janela EXTERNAL; nenhuma
referência viva às registries legacy zorro2/zorro3. Sem regressão no harness nem no
produto (ambos os modos de build).

# O que falta fazer

Superfície de deleção mapeada (fazer nesta ordem, validando incrementalmente):

1. **Relocar `BELLATRIX_ZORRO2_WIN_*`** para `autoconfig.h` (perto dos `AC_SIZE_*`);
   `lide_cdrom.c` passa a incluir `autoconfig.h` (já inclui) e larga `zorro2_bus.h`.
2. **`zorro_autoconfig.c/.h`** → reduzir a só `in_window` (usa o inline
   `bellatrix_z2_config_addr_contains` de `memory.h`, NÃO precisa de zorro2_bus).
   Remover read/write mortos + includes zorro2/3.
3. **`superbuster.c`** → remover o fallback `zorro3_in_board_window` + include.
4. **`memory_map.c/.h`** → remover o bloco `BELLATRIX_ROUTE_Z2_AUTOCONFIG` inteiro
   (define + todos os `#if`), o include `zorro2_bus.h`, e os enums `MEM_REGION_Z2`
   / `MEM_REGION_Z2_BOARD`.
5. **`machine_rigel.c`** → remover chamadas `zorro2/zorro3_init/reset` + includes.
6. **`machine_rigel_bus.c`, `bellatrix.c`, `memory.c`, `main.c`** → remover includes
   zorro2/3; corrigir comentário obsoleto em `bellatrix.c` ("→ memory_map →
   zorro2_bus.c").
7. **Testes** → deletar `test_zorro_autoconfig.c` (assunto removido); em
   `test_superbuster_z3.c` remover o caso legacy zorro3, manter o caso board_registry
   + gate NBSTAB.
8. **Deletar** `zorro2/zorro2_bus.{c,h}` e `zorro3/zorro3.{c,h}`.
9. **CMake** → remover as refs de `zorro2_bus.c`/`zorro3.c` no produto (variant:
   linhas 205, 206, 289 — inclui a duplicata) e no harness (BELLATRIX_SOURCES + alvos
   de teste + o alvo `test_zorro_autoconfig`). Manter a flag
   `BELLATRIX_ENABLE_EMU68_BOARDS` e `BELLATRIX_LEGACY_Z2_RAM_MB` (escolhem boards
   nativas do Emu68 vs Fast RAM do board_registry — decisão legítima, NÃO é a
   registry legacy). O Fast RAM do modo legacy já vem do board_registry
   (`z2_fast_ram.c`, incondicional desde o passo 1).

# Decisões tomadas

- board_registry `struct ExpansionBoard` não tem read/write por acesso; a janela
  EXTERNAL continua servida por `expansion.c` bus_ops (mantido — ver topo de
  `expansion.h`). Esta issue NÃO remove `expansion.c`.
- DIRECT vs EXTERNAL distinguidos por `map != NULL` vs `map == NULL`, sem estender a
  struct do Emu68. Ver [[bellatrix-board-registry-mechanism]].
- RTG permanece (VideoCore.card é Pi-only; evolução em sessão dedicada).
- O legacy "real" restante (fora desta issue) = memory-map hardcoded da TUI do
  `run.sh`.

# Critérios de aceite

- [x] `zorro2_bus.{c,h}` e `zorro3.{c,h}` deletados; nenhum include remanescente.
- [x] `test_zorro_autoconfig` removido; `test_superbuster_z3` só com o caso
      board_registry; suíte do harness verde.
- [x] Compilação do produto OK nos dois modos (`BELLATRIX_ENABLE_EMU68_BOARDS` ON e
      OFF).
- [x] lide enumera + carrega lide.device + janela ATA dispara; RTG registra como Z3.

# Observações

Ambos os backends roteiam por `machine_dispatch`, então o board_registry é
autoridade também no produto. Validação de runtime do produto (QEMU/Pi) fica para a
sessão de estabilização; aqui a barra é compilar os dois modos + harness verde.

# Log de execução

- 2026-07-18: superfície mapeada (esta issue). Contexto do desacoplamento em
  ISSUE-0060 (fases plugins / RTG→Z3 / decouple, merge 4f2f071).
- 2026-07-18 (executado, branch `legacy-registry-deletion`): deleção concluída.
  Deletados `zorro2_bus.{c,h}`, `zorro3.{c,h}`, `test_zorro_autoconfig.c`.
  `zorro_autoconfig.c` reduzido a só `in_window` (usa o inline de `memory.h`);
  `superbuster.c` sem fallback zorro3; `memory_map.c` sem o bloco Z2 (+ enums
  MEM_REGION_Z2/Z2_BOARD removidos); `machine_rigel.c` sem init/reset zorro2/3;
  includes removidos de `machine_rigel_bus.c`, `bellatrix.c`, `memory.c`,
  `main.c`. `WIN_128KB` relocado para `autoconfig.h`. `test_superbuster_z3`
  reescrito só com o caso board_registry (+ NBSTAB + shutup); `test_memory_map`
  sem as asserções da rota Z2 legacy. Labels de backend em `memory.c` atualizados
  ("board_registry"). Scripts de integração (smoke/boot_adf/no_autoconfig)
  atualizados para as strings novas. **Validação:** harness 16/16 (built) verde;
  lide carrega lide.device; RTG registra Z3; **produto compila+linka nos DOIS
  modos** (musashi, BELLATRIX_EMU68_BOARDS_MODE=legacy E boards). Sweep confirmou
  zero refs remanescentes a `bellatrix_zorro2_/zorro3_` ou aos headers.
- 2026-07-18 (esclarecimento dos "dois modos", decisão do Jaime): `BELLATRIX_
  ENABLE_EMU68_BOARDS` NÃO é mais "legacy registry vs moderno" — a registry foi
  deletada. Agora ON = compila as boards NATIVAS do Emu68 (z2ram/sdcard/68040/
  devicetree, seção `.boards`, walker `vectors.c`); OFF (default do build.sh) =
  não compila essas, Fast RAM vem do `z2_fast_ram.c` (board_registry). Em AMBOS
  os modos as boards do board_registry (lide/RTG/fast_ram) existem e dirigem o
  AutoConfig via `machine_dispatch`. **Flag NÃO renomeada** (Jaime: só documentar).
  Persistem 2 modos porque 2 walkers/seções coexistem (Emu68 `.boards.z2` vs
  `bellatrix_boards`) — unificá-los é o alvo (§5/§6 de docs/expansions_and_boards.md,
  [[bellatrix-memmap-emu68-convergence]]), dependente de estabilizar boards-mode
  (sdcard no QEMU). Documentado em docs/expansions_and_boards.md §4 (Build modes).
