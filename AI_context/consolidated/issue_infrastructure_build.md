# Issue: Infraestrutura, Build System e Patches Emu68

## Contexto do Problema

O projeto Bellatrix precisa integrar-se ao Emu68 (JIT M68K->AArch64) sem manter
alteracoes diretas no submodulo. A integracao usa patches pequenos no Emu68 e
mantem a maior parte da cola em `src/` + `cmake/`.

## Decisões Arquiteturais

- **Patches mínimos sobre Emu68** — o submodulo `emu68/` nao e fonte de verdade
  para alteracoes Bellatrix; toda alteracao nele deve existir em `patches/`.
- **`cmake/` como fronteira de integracao** — `cmake/bellatrix-variant.cmake`
  lista fontes, includes, options e defines Bellatrix sem precisar editar o
  patch base do Emu68 a cada nova fonte.
- **Estrutura de fontes em `src/`** — cola Bellatrix/Emu68 vive fora do Emu68 e
  entra no firmware via `cmake/bellatrix-variant.cmake`.
- **`btrace`** — sistema de logging JSON Lines via UART para captura e análise de acessos de bus.
- **`external/musashi`** — submodule para harness de desenvolvimento/teste em Linux (sem bare-metal).

## Estrutura de Diretórios Final

```
bellatrix/
  emu68/                          # git submodule (READ-ONLY)
  external/btstack                # Bluetooth
  external/cherryusb              # USB host stack
  external/musashi                # CPU harness
  src/
    cpu/bellatrix.h/.c            # Entry point Emu68 + bus dispatch
    core/machine.h/.c             # BellatrixMachine — integração
    core/btrace.h/.c              # Bus trace JSON Lines
    bridge/bellatrix_bridge.h/.c  # Camada bridge CPU/machine
    chipset/
      cia/cia*.c/h                # CIA 8520 (modular: timers, ports, TOD, serial, irq)
      agnus/agnus.h/.c            # Agnus: beam, DMA, copper, blitter
      agnus/copper.h/.c           # Copper (subordinado ao Agnus)
      agnus/blitter.h/.c          # Blitter
      agnus/dma.h/.c              # DMA arbitration
      agnus/bitplanes.h/.c        # Bitplane fetch
      denise/denise.h/.c          # Denise (instância explícita)
      denise/sprites.h/.c         # Sprites (Denise)
      paula/paula.h/.c            # Paula: INTREQ/INTENA owner
      paula/paula_disk.h/.c       # Paula disk DMA
      paula/paula_serial.h/.c     # Paula serial (SERDAT/SERDATR)
      paula/paula_audio.h/.c      # Paula audio
      paula/paula_input.h/.c      # Paula input (POTGO/POTGOR)
      paula/paula_interrupt.h/.c  # Paula interrupt engine
      floppy/floppy_drive.h/.c    # Floppy drive (ADF)
    input/
      keyboard.h/.c               # Protocolo teclado Amiga
      controller_port.h/.c        # Joystick/mouse
    host/
      pal.h                       # Platform Abstraction Layer
      raspi3/                     # Backend bare-metal RPi3
      posix/pal_posix.c           # Backend harness Linux
    io/
      serial/uart_host.h/.c       # Bridge serial guest→host
      usb/usb_hc_bellatrix.h/.c   # DWC2 USB host (Bellatrix native)
      usb/usb_osal_bellatrix.c    # CherryUSB OSAL shim
    runtime/
      core_gfx.h/.c               # Core 1: Agnus+Denise
      core_audio.h/.c             # Core 2: Paula
      core_io.h/.c                # Core 3: CIA+UART
  patches/
    0001-add-bellatrix-variant-cmake.patch
    0002-add-bellatrix-bus-hook.patch
    0003-bellatrix-execution-loop.patch
    0004-bellatrix-cherryusb-dwc2-host.patch
  scripts/
    setup.sh                      # aplica patches + verifica prerequisites
    build.sh                      # cmake + make VARIANT=bellatrix
    flash.sh                      # SD card ou TFTP
  tools/
    btrace/btrace.py              # captura serial → JSON Lines
    btrace/analyze.py             # análise de log → relatório
    launcher/                     # TUI launcher para selecionar ROM/ADF
  tests/
    unit/                         # testes unitários CIA, memória, UART
    integration/                  # testes integração overlay, blitter, sprites, audio
```

## Patches Emu68 — Sumário

### `0001-add-bellatrix-variant-cmake.patch`
- `SUPPORTED_VARIANTS` inclui `"bellatrix"` no CMakeLists.txt
- `include_directories`: `${CMAKE_SOURCE_DIR}/../src` + subdirs
- `BASE_FILES`: lista completa de fontes Bellatrix

### `0002-add-bellatrix-bus-hook.patch` (o mais complexo)
- `vectors.c`: `#elif defined(BELLATRIX)` — hook de bus (`bellatrix_bridge_cpu_access`)
- `vectors.c`: save/restore de `x18` (M68K PC) e `v30` (JIT instruction counter) ao redor do bus hook — **crítico** para evitar corrupção do counter JIT
- `start.c`: `#if !defined(PISTORM) && !defined(BELLATRIX)` para excluir BELLATRIX do path HUNK/ELF
- `start.c`: guard `rom_copy` / `ps_read_32` com `#ifdef PISTORM`
- `start.c`: `__m68k.CACR = BE32(CACR_IE)` — ativa JIT cached mode para BELLATRIX
- `start.c`: `bellatrix_init()` chamado antes de `M68K_StartEmu()`
- `start.c`: secondary_boot para Core 1/2/3 (multicore)

### `0003-bellatrix-execution-loop.patch`
- `ExecutionLoop.c`: fronteiras seguras que reportam instruções/ciclos ao runtime
  genérico da machine API; não chama código Bellatrix diretamente

### `0004-bellatrix-cherryusb-dwc2-host.patch`
- Patches no CherryUSB: endianness LE, hub descriptor parsing, polling mode, DWC2 MMIO LE accessors

### `0025`–`0035`: machine API do Emu68 — RETIRADA

> **Retirada (2026-07-15), fontes removidos (2026-07-20).** A premissa central
> desta API — que MMIO normal precisava parar de usar Data Abort — é *errada*,
> não apenas superada: o fault handler sempre foi só um roteador. Ver
> `docs/fault_handler.md` para a correção e ISSUE-0058 para a linha de base
> conservadora, que mantém o caminho fault-driven nativo do Emu68.
>
> Os patches `0025`–`0035` foram removidos e arquivados com os diffs completos
> em `AI_context/archive/emu68-public-machine-api-2026-07.md`. Os fontes
> correspondentes (`src/cpu/emu68/emu68_machine*`, `emu68_backend.c`,
> `mainloop_window.S`, `tests/unit/test_emu68_machine.c`) ficaram órfãos —
> fora de todo `CMakeLists`/`.cmake` — e foram deletados em 2026-07-20.
> `docs/emu68_public_api.md` é mantido porque a questão *separada* de
> sincronização do relógio do chipset (ISSUE-0058 §3.1.1) segue aberta.

Descrição histórica do que os patches faziam:

- adicionam retorno/continuação exata do loop, `mmu_unmap`, classificação
  explícita de todos os caminhos de acesso e contagem determinística de ciclos
- acessos EXTERNAL chegam ao contrato público antes de qualquer load/store host
- o antigo patch `0021` e o wrapper baseado em fault foram removidos

O contrato e o backend ficavam no repositório principal (todos deletados):

- ~~`src/cpu/emu68/emu68_machine.h/.c`~~
- ~~`src/cpu/emu68/emu68_machine_emit.c`~~
- ~~`src/cpu/emu68/emu68_machine_platform.c`~~
- ~~`src/cpu/emu68/emu68_backend.c`~~

O adaptador vivo hoje é `src/cpu/emu68/emu68_native_backend.c` +
`emu68_direct_region.c` + `vectors.inc`, registrados em
`cmake/bellatrix-variant.cmake`.

## Build Commands

```bash
./scripts/setup.sh                    # aplica patches (idempotente: reverse-check)
./scripts/build.sh                    # build normal
./scripts/build.sh clean              # clean rebuild
BELLATRIX_USBSTACK=1 ./scripts/build.sh  # build com USB host stack
./run.sh qemu                         # smoke test no QEMU
./run.sh harness                      # harness Linux com Musashi
```

## Problemas Históricos Resolvidos

### Sprint 04: `blitwait` scope error
`static int blitwait` declarado só dentro de `#ifdef PISTORM`. O bloco `#if defined(PISTORM) || defined(BELLATRIX)` em `M68K_StartEmu` expôs referência inválida. Fix: guard com `#ifdef PISTORM`.

### Sprint 15: Patch 0001 desatualizado (assume-unchanged)
`emu68/CMakeLists.txt` marcado como `assume-unchanged` → `git apply --reverse --check` falhava. Fix: `git update-index --no-assume-unchanged` antes de regenerar patch.

### Sprint 43: Patch 0004 divergente de cherryusb local
Build.sh temporariamente relaxado para aceitar cherryusb com mudanças locais mais novas que o patch. Sprint 43 restaura invariante: patch 0004 é source of truth, `git apply --check` deve passar em subtree limpa.

### 2026-06-15: Workaround provisório para release `v0.0.0`
Durante a primeira tentativa de release via GitHub Actions, o build chegou ao link e falhou por dois símbolos ausentes:

- `bellatrix_emu68_boards_reset`
- `g_lide_rom_data` / `g_lide_rom_size`

Workaround aplicado:

- `patches/0002-add-bellatrix-bus-hook.patch` injeta um `bellatrix_emu68_boards_reset()` no-op no caminho `#elif defined(BELLATRIX)` de `emu68/src/aarch64/vectors.c`.
- `cmake/bellatrix-variant.cmake` adiciona `src/machine/expansions/lide_cdrom/lide_rom_stub.c` quando `external/lide.device/lide.rom` não existe no runner.
- `lide_rom_stub.c` define `g_lide_rom_data` e `g_lide_rom_size` apenas para destravar o link sem embutir o ROM do lide.device.

Isto é provisório. O caminho correto é:

- regenerar o patch 0002 para manter a integração Bellatrix/Emu68 completa e coerente com `BELLATRIX_ENABLE_EMU68_BOARDS`;
- garantir que o pipeline de release construa ou forneça `external/lide.device/lide.rom`;
- remover `lide_rom_stub.c` quando o ROM real estiver sempre disponível no build de release.

## O que Funciona
- Build verde em todos os cenários documentados
- `setup.sh` idempotente (detecta patches já aplicados)
- Harness Linux (Musashi) e bare-metal (Emu68) compilam do mesmo `src/`
- CMake include paths resolvem todos os `#include` sem prefixos especiais

## O que Ainda Precisa Atenção
- Patch 0004 (CherryUSB) pode precisar ser regenerado quando mudanças USB forem feitas
- `scripts/build.sh clean` é necessário após mudanças em patches

## Modelo de Memória — Constantes Centralizadas (Sprint 23)

`src/core/memory/memory.h` define as constantes canônicas:

```c
#define BELLATRIX_CHIP_RAM_BASE   0x000000
#define BELLATRIX_CHIP_RAM_SIZE   0x080000   // 512KB (diagnosis) ou 0x200000 (2MB normal)
#define BELLATRIX_CHIP_RAM_END    (BELLATRIX_CHIP_RAM_BASE + BELLATRIX_CHIP_RAM_SIZE)
#define BELLATRIX_CHIP_RAM_MASK   (BELLATRIX_CHIP_RAM_SIZE - 1)
#define BELLATRIX_CHIP_BOOT_SIZE  0x080000   // janela de overlay
#define BELLATRIX_CHIP_BOOT_END   (BELLATRIX_CHIP_RAM_BASE + BELLATRIX_CHIP_BOOT_SIZE)
```

Helpers disponíveis:
```c
bool bellatrix_chip_addr_contains(uint32_t addr);
bool bellatrix_chip_addr_contains_range(uint32_t addr, uint32_t size);
```

**Nota**: 512KB foi usado temporariamente para acelerar o boot no harness (menos
tempo no loop de clear de RAM do KS). O valor correto para bare-metal é 2MB (`0x200000`).
Todos os consumidores (Copper, Agnus, bus, musashi_backend, testes) devem usar as
constantes centrais, nunca hardcode de `0x1FFFFF` ou `0x200000`.

## Arquivos Críticos para Manter em Sincronia
- `emu68/CMakeLists.txt` ↔ `patches/0001-...`
- `emu68/src/aarch64/vectors.c` ↔ `patches/0002-...`
- `emu68/src/aarch64/start.c` ↔ `patches/0002-...`
- `emu68/src/ExecutionLoop.c` ↔ `patches/0003-...`
  (`patches/0025-...` retirado — ver seção `0025`–`0035` acima)
- ~~emissores `emu68/src/M68k_*.c` ↔ `patches/0027-...` a `0035-...`~~ (retirados)
- ~~`src/cpu/emu68/emu68_machine*` e `emu68_backend.c`~~ (deletados 2026-07-20)
- `src/cpu/emu68/vectors.inc` ↔ `patches/0002-...`
  (incluído por `emu68/src/aarch64/vectors.c`, não é TU separada)
- `external/cherryusb/` ↔ `patches/0004-...`
