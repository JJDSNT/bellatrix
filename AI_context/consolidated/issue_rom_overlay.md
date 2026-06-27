# Issue: ROM Loading e Overlay Management

## Status: CLOSED (2026-06-26)

Boot completo com Happy Hand e Workbench em hardware valida ROM loading,
overlay MMU remap e OVL trigger via CIA-A PRA. Histórico de bugs preservado
como referência.

## Contexto do Problema

O Kickstart ROM precisa ser visível em `0x000000` no boot (overlay ativo) e depois
mapeado para `0xF80000` quando a CIA-A PRA bit 0 (OVL) for limpo pelo Kickstart.
Múltiplos bugs impediam este processo em diferentes fases.

## Decisões Arquiteturais

- **ROM via initramfs** — o bootloader RPi passa o Kickstart.rom via `initramfs`
  (`config.txt`: `kernel=kernel8.img` + `initramfs kickstart.rom followkernel`).
  `start.c` processa `initramfs_loc` no path não-PISTORM.
- **Overlay = MMU remap** — diferente do PiStorm (que usa GPIO), Bellatrix usa a MMU
  AArch64 para tornar `0x000000-0x07FFFF` visível como ROM ou chip RAM.
- **OVL trigger** — escrita em `0xBFE001` (CIA-A PRA, bit 0) detectada no write hook
  de `vectors.c` → chama `bellatrix_sync_overlay_from_ciaa()`.
- **Janela de overlay = 512K** — não apenas 4K (erro histórico). Corresponde ao
  padrão do Emu68 base (`addr < 0x80000`).

## Histórico de Bugs

### Sprint 02: ROM path errado em start.c
**Problema**: BELLATRIX caía no path HUNK/ELF (`M68K_StartEmu` lia PC de addr=0
sem ler reset vectors da ROM).
**Fix**: `#if !defined(PISTORM) && !defined(BELLATRIX)` exclui BELLATRIX do path
HUNK/ELF. BELLATRIX passa pelo mesmo `#else` (ROM loading) que o path padrão.

### Sprint 06: Overlay apenas 4K
**Problema**: `mmu_map(0xf80000, 0x000000, 4096, ...)` — apenas os primeiros 4K
eram shadow da ROM.
**Fix Sprint 21**: `mmu_map(0x000000, 0xE00000, 0x80000, ...)` — janela completa de
512K. Corresponde ao padrão do Emu68 base.

### Sprint 09: ROM não carregada (initramfs ignorado)
**Problema**: `#elif defined(BELLATRIX)` em `start.c` pulava o bloco de processamento
do initramfs e ia direto para `bellatrix_init()` sem carregar o ROM.
**Fix**: Remover `#elif BELLATRIX` separado. BELLATRIX cai no `#else` junto com
o path padrão. Blocos PISTORM-only guardados com `#ifdef PISTORM`.

### Sprint 09: Chip RAM não mapeada após refactor
**Sintomas no log**:
```
[BUS] unmapped write: ffffeffc = 00000000   ← stack push sem chip RAM mapeada
[BUS] unmapped read:  ffffe203               ← ROM alias 32-bit
[BUS] unmapped read:  84000058               ← vetor exceção
```
**Causa**: `bellatrix.c` havia sido refatorado e perdeu o `mmu_map` para chip RAM.
**Fix**: restaurar `mmu_map(0x0, 0x0, 0x200000, ...)` + AF=0 trap mappings para CIA/custom.

### Sprint 18: CACR_IE não setado → overlay não funciona em cached mode
**Problema**: Sem `__m68k.CACR = BE32(CACR_IE)`, JIT rodava em uncached mode e
`bellatrix_machine_advance` nunca era chamado via ExecutionLoop.
**Sintoma**: `exec_pc=00000000` sempre, chipset nunca avançava.
**Fix**: Ver `issue_emu68_jit_integration.md`.

### Sprint 21: OVL logic fora do live path
**Problema crítico descoberto**: `bellatrix_bus_access()` tinha o código de overlay,
mas o live Emu68 path NÃO chama essa função. O path real é:
`vectors.c → bellatrix_bridge_cpu_access() → bellatrix_machine_write/read()`.
Portanto, toda a lógica OVL em `bellatrix_bus_access()` era letra morta.

**Fix**: 
1. Nova função `bellatrix_sync_overlay_from_ciaa()` em `src/cpu/bellatrix.c`
2. Chamada de `vectors.c` após write em `0xBFE001`:
   ```c
   if (addr == 0x00BFE001)
       bellatrix_sync_overlay_from_ciaa();
   ```
3. `bellatrix_sync_overlay_from_ciaa()` lê o estado CIA-A PRA da machine e remapeia:
   - OVL=1: `mmu_map(0x000000, 0xE00000, 0x80000, READ_ONLY)` → ROM visível em 0
   - OVL=0: restore chip RAM + reaplica debug traps (page zero, JMP table page)

## Sequência de Boot Esperada (Correta)

```
1. boot loader RPi carrega kernel8.img + kickstart.rom como initramfs
2. start.c: processa initramfs_loc → copia ROM para 0xF80000 ARM
3. bellatrix_init():
   - mmu_map(0x0,   0x0,       0x200000, R/W)   ← chip RAM 2MB
   - mmu_map(0x0,   0xE00000,  0x80000,  RO)    ← overlay ativo: ROM em 0
   - mmu_map(0xF80000, ..., ROM_SIZE, RO)         ← ROM em endereço real
   - AF=0 traps para CIA/custom (0x200000-0xDFFFFF, 0xF00000-0xF7FFFF)
4. M68K_StartEmu():
   - lê [0x000000] = ISP (via overlay → ROM)
   - lê [0x000004] = PC (via overlay → ROM)
   - JIT começa execução do Kickstart em 0xFC0000+
5. Kickstart escreve CIA-A PRA (0xBFE001) com OVL=0
   → vectors.c detecta → bellatrix_sync_overlay_from_ciaa()
   → mmu_map(0x000000, 0x0, 0x200000, R/W) ← chip RAM em 0
6. Kickstart usa chip RAM normalmente
```

## Diagnóstico Útil

O Bellatrix imprime no boot:
```
[BELA] rom_mapped=1
[BELA] ROM @ 0xf80000: xx xx xx xx ...
[BELA] Reset vectors: ISP=0x00001000 PC=0x00FC00D2
[BELA] Overlay check virt[0:3]: xx xx xx xx   (deve igualar ROM bytes)
```
E ao toggle overlay:
```
[OVL-TRIG] ciaa_pra=0x00 overlay=0 pc=0x00FC00D2
```

## DiagROM — OVL FAILED

Sprint 20 observou `Checking if OVL works: FAILED` com DiagROM 2.0. Após o fix
do Sprint 21 (OVL no live path + janela 512K), este teste deveria passar.
**Status**: pendente de validação em hardware real após Sprint 21.

## O que Funciona
- ROM loading via initramfs em bare-metal
- Overlay toggle detectado via write hook em `vectors.c`
- Mapeamento MMU de 512K para overlay
- Diagnósticos de init impressos no UART

## O que Precisa Validação
- DiagROM 2.0 `OVL works: PASSED` após Sprint 21 (não confirmado em hardware)
- Comportamento de reset fidelity (harness usa pulse_reset via bus; bare-metal usa
  vetores pré-computados de `bellatrix_reset_isp/pc`) — diferença potencial

## Arquivos Relevantes
- `src/cpu/bellatrix.c` — `bellatrix_init()`, `bellatrix_sync_overlay_from_ciaa()`
- `src/cpu/bellatrix.h` — declarações públicas
- `emu68/src/aarch64/vectors.c` — live write hook + OVL trigger
- `emu68/src/aarch64/start.c` — initramfs + path de ROM loading
- `patches/0002-add-bellatrix-bus-hook.patch`

---

### Sprint 44: Extended ROM window — AROS 1MB ROM no backend Musashi bare-metal

**Problema**: No build bare-metal com Musashi como CPU backend (`bellatrix-musashi`),
AROS reportava "512KiB ROM detected" em vez de "1MiB ROM detected".
O build harness (Linux) e o build Emu68 JIT reportavam 1MB correctamente.

**Causa raiz**: `src/cpu/musashi_backend.c` → `musashi_read()` não tinha handler
para o intervalo `0xe00000–0xe7ffff` (janela de ROM estendida AROS — módulos
nvdisk.library, expansion.library, trackdisk.device, etc.). Leituras nesse intervalo
caíam para `bellatrix_bridge_cpu_read()` e retornavam `0xffffffff`. AROS não
conseguia ler os headers da ROM estendida e concluía que era uma ROM de 512KB.

Por que o Emu68 JIT funciona: `start.c` chama `mmu_map(0xe00000, …, AF=1)` para
o primeiro bloco de 512KB da ROM, tornando-o diretamente acessível sem fault.
O path Musashi passa por software callbacks, não pela MMU, e dependia de código
explícito que estava ausente.

**Fix implementado**:

1. `src/core/memory/memory.h` — novos campos em `BellatrixMemory`:
   ```c
   #define BELLATRIX_EXT_ROM_BASE  0x00E00000u
   #define BELLATRIX_EXT_ROM_SIZE  0x00080000u
   #define BELLATRIX_EXT_ROM_END   0x00E7FFFFu

   const uint8_t *rom_ext;
   size_t         rom_ext_size;
   ```
   + declaração de `bellatrix_memory_attach_ext_rom()`.

2. `src/core/memory/memory.c` — inicializa campos a 0 em `bellatrix_memory_init()`;
   implementa `bellatrix_memory_attach_ext_rom()`.

3. `src/cpu/musashi_backend.c` — handler de leitura adicionado em `musashi_read()`:
   ```c
   if (mem->rom_ext &&
       addr >= BELLATRIX_EXT_ROM_BASE &&
       addr <= BELLATRIX_EXT_ROM_END) {
       const uint8_t *p = mem->rom_ext + (addr - BELLATRIX_EXT_ROM_BASE);
       if (size == 1u) return p[0];
       if (size == 2u) return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
       return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
              ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
   }
   ```
   + write protection adicionada em `musashi_write()` para o mesmo intervalo.

4. `src/cpu/bellatrix.c` — detecção automática de ROM 1MB em `bellatrix_init()`:
   compara primeiros 8 bytes em `ROM_EXT_KVIRT` (`0xffffff9000e00000`) vs
   `ROM_KVIRT` (`0xffffff9000f80000`); se diferentes, chama
   `bellatrix_memory_attach_ext_rom()` com o ponteiro da primeira metade.

**Status**: Build `bellatrix-musashi` compila verde. AROS agora reporta
"1MiB ROM detected" com ambas as regiões (`0xe00000–0xe7ffff` +
`0xf80000–0xffffff`). AROS ainda para em `trackdisk.device` — bug separado.

**Pendente**:
- `floppy_drive.c:180`: `d->disk_changed = floppy_has_media(d) ? 0 : 1` deve ser
  `d->disk_changed = 0` — no hardware real, STEP sempre limpa o latch `/DSKCHG`
  independentemente de haver mídia.
- Investigar por que AROS para em `trackdisk.device` no bare-metal Musashi mas
  passa no harness (próxima sessão — ver análise abaixo).

### Análise: por que trackdisk.device falha no bare-metal Musashi mas passa no harness

O código CIA e floppy é **idêntico** nas duas builds — ambas usam `src/chipset/`.
A diferença não está na lógica, está no ambiente:

**Candidato 1 — CIA-B PRB timing**
O harness tem loop de frames fixo (ciclos sintéticos). O bare-metal Musashi acumula
ticks reais do E-clock via `cia_tick_acc / 10`. Se o quânto de ciclos entregue ao
Musashi for muito grande, os timers CIA podem "pular" eventos que o trackdisk espera.

**Candidato 2 — Overlay com AROS 1MB**
`apply_overlay_map(1)` mapeia `0xe00000` em `0x000000`. O primeiro opcode da janela
ext é `JMP 0xF80002`. Se o Musashi acessar `0x000000` num momento inesperado (antes
ou depois do toggle de overlay), pode executar código errado. Emu68 JIT tem acesso
direto via MMU e não passa por esse path.

**Candidato 3 — Autoconfig fast RAM ausente**
AROS trackdisk pode depender de ter RAM autoconfig configurada antes de inicializar.
No harness, a fast RAM estática (`g_fast_ram`) já está disponível desde o início.
No bare-metal Musashi, o autoconfig está presente (`BELLATRIX_HARNESS` não definido
no bare-metal), mas a sequência de configuração pode ser diferente.

**Como diagnosticar na próxima sessão**:
1. Capturar log serial do ponto exato de parada no bare-metal — linha imediatamente
   antes de travar identifica qual candidato investigar primeiro.
2. Se o log mostrar CIA timeout → Candidato 1.
3. Se o log mostrar acesso inválido ou PC inesperado → Candidato 2.
4. Se o log mostrar falha de alocação de memória → Candidato 3.
