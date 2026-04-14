// AI_context/sprint_04.md

# Sprint 04 — Reorganização e Build verde

## Status: concluído

---

## Issue 1 — Reorganização da estrutura de arquivos

Estrutura anterior (`src/variants/bellatrix/`) substituída pela estrutura definitiva:

```
src/
  cpu/            # Emu68 integration (bellatrix.c/h — entry point e bus dispatch)
  core/           # btrace.c/h — bus trace in-firmware
  chipset/
    cia/          # cia.c/h — CIA 8520
    agnus/        # (Fase 3+)
    denise/       # (Fase 4+)
    paula/        # (Fase 3+)
  host/           # pal.h + raspi3/*.c — Platform Abstraction Layer
  memory/         # (Fase 1 já implementada em cpu/)
  bus/            # (Fase 5+)
  devices/        # (MVP futuro)
  firmware/       # (configuração de ROM)
tests/
  unit/
  integration/
  traces/
  fixtures/
```

Include paths no CMakeLists.txt:
- `${CMAKE_SOURCE_DIR}/../src` — resolve `#include "core/btrace.h"`, `#include "chipset/cia/cia.h"`
- `${CMAKE_SOURCE_DIR}/../src/cpu` — resolve `#include "bellatrix.h"` em vectors.c
- `${CMAKE_SOURCE_DIR}/../src/host` — resolve `#include "pal.h"` em raspi3/*.c

---

## Issue 2 — Build corrigido

Erro encontrado: `blitwait` declarado `static int blitwait` apenas dentro de `#ifdef PISTORM`.
O change `#if defined(PISTORM) || defined(BELLATRIX)` em `M68K_StartEmu` expôs isso.

Fix: guarda a linha com `#ifdef PISTORM ... #endif` no patch 02.

Resultado: `[100%] Built target Emu68.elf` — Emu68.img de 2.0 MB gerado.

---

## Estado atual dos patches

`patches/0001-add-bellatrix-variant-cmake.patch`:
- `SUPPORTED_VARIANTS` inclui `"bellatrix"`
- Bloco cmake com include_directories e BASE_FILES novos caminhos

`patches/0002-add-bellatrix-bus-hook.patch`:
- `vectors.c`: `#elif defined(BELLATRIX)` + bus hook
- `start.c`: 5 mudanças (HUNK path exclusion, rom_copy guard, initramfs path, bellatrix_init call, M68K_StartEmu fix, blitwait guard)

---

## Próxima sessão: Fase 3 — INTENA/INTREQ/VBL

Arquivos a criar:
- `src/chipset/agnus/agnus.h` / `agnus.c` — INTENA/INTREQ/DMACON + VPOSR/VHPOSR
- `src/host/raspi3/pal_timer.c` — ARM generic timer FIQ para VBL a 50 Hz

Patches a estender (patch 02):
- `vectors.c`: FIQ handler BELLATRIX (ARM timer → INTREQ[VERTB] → INT.ARM)
- `start.c`: label 9 fix (`#if defined(PISTORM) || defined(BELLATRIX)`)
  - `#ifdef PISTORM` → `#if defined(PISTORM) || defined(BELLATRIX)` na linha ~1890
  - `#ifndef PISTORM32` → `#if !defined(PISTORM32) && !defined(BELLATRIX)` na linha ~1904

Critério de sucesso: IPL trace mostra VBL (level 3) periódico a 50 Hz.
