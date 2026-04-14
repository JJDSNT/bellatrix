// AI_context/sprint_01.md

# Sprint 01 — Fase 0: Mapeamento + Base de Integração

## Status: concluído

---

## Issue 1 — Mapeamento do Emu68 (Etapa 1)

### O que foi encontrado

**Fluxo de bus access:**
```
data abort → SYSHandler [vectors.c:2162]
  → SYSPageFault{Write,Read}Handler [vectors.c:1067 / 1513]
    → SYSWriteValToAddr / SYSReadValFromAddr [vectors.c:326 / 484]
      └── #ifdef PISTORM → ps_write_*/ps_read_* (GPIO)
      └── #elif BELLATRIX → bellatrix_bus_access() ← nosso hook
      └── #else → acesso direto à memória física
```

**IPL:** `M68KState.INT.IPL` + `.INT.ARM`, acessível via `TPIDRRO_EL0`.
Escrita + DMB → JIT lê no próximo checkpoint.

**ROM overlay:** detectado em `SYSWriteValToAddr` na escrita em `CIAAPRA`.

**Chip RAM (Fase 1):** mapeamento direto MMU `0x000000–0x1FFFFF` → buffer ARM estático.

---

## Issue 2 — Submodule + Patches (Etapa 2)

### O que foi feito

- `emu68/` adicionado como git submodule (`github.com/michalsc/Emu68`)
- Dois patches gerados em `patches/`:
  - `0001-add-bellatrix-variant-cmake.patch` — SUPPORTED_VARIANTS + bloco bellatrix em CMakeLists.txt
  - `0002-add-bellatrix-bus-hook.patch` — `#elif defined(BELLATRIX)` em vectors.c + `bellatrix_init()` em start.c

**Mudança em start.c:** `#ifndef PISTORM` → `#if !defined(PISTORM) && !defined(BELLATRIX)`
para que o Bellatrix tome o path de ROM (initramfs_loc ou SD card), não o path de HUNK/ELF.

---

## Issue 3 — Código Bellatrix Fase 0 (Etapa 2 + 3)

### Arquivos criados

```
src/variants/bellatrix/
  bellatrix.h / bellatrix.c     # entry point + bus dispatch stub
  chipset/
    btrace.h / btrace.c         # bus trace completo com ring buffer e watchdog
  platform/
    pal.h                       # interface pública da PAL
    raspi3/
      pal_debug.c               # wraps Emu68's kprintf
      pal_ipl.c                 # escreve em M68KState.INT via TPIDRRO_EL0
      pal_core.c                # stubs (timer, video, core dedicado — Fase 3+)
```

**btrace:** loga JSON Lines via `PAL_Debug_Print`. Ring buffer de 64 entradas para
post-mortem. Watchdog de 250 VBLs. Filtro via 0xDFFF00.

**bellatrix_bus_access:** Fase 0 = stub (retorna 0, loga tudo como unimpl).

---

## Issue 4 — Scripts e Ferramentas

### Arquivos criados

```
scripts/
  setup.sh   # aplica patches + verifica prerequisites
  build.sh   # cmake + make VARIANT=bellatrix
  flash.sh   # SD card ou TFTP

tools/btrace/
  btrace.py  # captura serial → JSON Lines (pyserial)
  analyze.py # análise de log → relatório de registradores não implementados
```

---

## Critério de conclusão da Fase 0

- [ ] `scripts/build.sh` compila sem erros
- [ ] Imagem roda no RPi 3 e entra no JIT loop
- [ ] UART mostra primeiros eventos JSON de bus trace
- [ ] `analyze.py --report boot.jsonl` gera relatório válido

Os primeiros três itens requerem hardware (RPi 3 + SD card + serial).

---

## Próxima sessão: Fase 1

**Objetivos:**
1. Chip RAM: `mmu_map(0x000000, 0x000000, 0x200000, ...)` para buffer ARM estático (2 MB)
   - Arquivo: `src/variants/bellatrix/bellatrix.c` ou novo `chipset/ram.c`
   - Hook: chamado em `bellatrix_init()` antes de `M68K_StartEmu()`
2. ROM load: usar `initramfs_loc` para carregar Kickstart do SD card
   - Verificar caminho em `start.c` linha ~1354 (`else if (initramfs_loc != NULL)`)
   - Confirmar que patches não quebram esse caminho para BELLATRIX
3. Validar que JIT executa as primeiras instruções do Kickstart

**Observação sobre patches vs start.c:**
O path de ROM loading em start.c usa `initramfs_loc` — isso carrega um arquivo do SD card
quando a variante não é PiStorm (hardware ROM). Para Bellatrix, o Kickstart.rom deve estar
na raiz do SD card como `kernel8.img` não — precisa verificar como o bootloader passa o
initramfs. Ver `docs/bellatrix_arquitetura.docx` seção 10.2 para detalhes.
