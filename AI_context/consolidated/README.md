# Bellatrix — Histórico Consolidado por Issue

Este diretório contém a consolidação de todos os sprints (01-43) organizados por
tópico funcional. Substitui os arquivos `sprint_NN.md` individuais.

## Índice de Issues

| Arquivo | Tópico | Sprints Fonte | Status |
|---------|--------|--------------|--------|
| [issue_infrastructure_build.md](issue_infrastructure_build.md) | Build system, patches Emu68, estrutura de diretórios | 01-04, 15, 43 | ✅ Estável |
| [issue_rom_overlay.md](issue_rom_overlay.md) | ROM loading via initramfs, overlay MMU | 01-02, 06, 09, 18, 21 | ✅ Funciona (OVL live path fix) |
| [issue_emu68_jit_integration.md](issue_emu68_jit_integration.md) | CACR_IE, v30 save/restore, ExecutionLoop, bridge, Fast RAM | 08, 18, 19, 21 | ✅ Funciona |
| [issue_interrupt_pipeline.md](issue_interrupt_pipeline.md) | INTENA/INTREQ/IPL, FIQ, CIA→Paula chain | 05, 08, 11, 14, 16, 29 | ✅ Funciona (⚠️ cross-core timing) |
| [issue_cia_8520.md](issue_cia_8520.md) | CIA 8520 completo: timers, TOD, ports, serial, ICR | 03, 10, 11, 14, 16, 22 | ✅ Funciona |
| [issue_agnus_dma_copper.md](issue_agnus_dma_copper.md) | Agnus beam, DMA arbiter, Copper raster-time, Blitter | 05, 07, 10, 11, 14, 17, 21, 24 | ⚠️ **LOF bug pendente** |
| [issue_denise_rendering.md](issue_denise_rendering.md) | Denise bitplanes, sprites, rendering raster-time | 07, 10, 11, 17, 21, 23, 24, 26 | ⚠️ **Sprites sem compositing** |
| [issue_paula_serial_floppy.md](issue_paula_serial_floppy.md) | Paula, disk DMA, serial bridge, floppy | 11, 14, 15, 16, 20, 21, 22, 25 | ✅ Funciona (⚠️ DiagROM1.3 loopback) |
| [issue_harness_ks13_boot_screen.md](issue_harness_ks13_boot_screen.md) | Investigação tela de boot KS1.3 — bloqueador LOF | 14-26 | 🔴 **Bloqueado: LOF=1 permanente** |
| [issue_multicore_runtime.md](issue_multicore_runtime.md) | RPi3 multicore: Core 1=GFX, Core 2=Paula, Core 3=IO | 27, 28, 29 | ✅ Funciona (⚠️ cross-core sync) |
| [issue_usb_host_dwc2.md](issue_usb_host_dwc2.md) | CherryUSB + DWC2 host, BCM2837 bring-up | 30-43 | ⚠️ QEMU OK, Pi 3B pendente |
| [issue_bluetooth.md](issue_bluetooth.md) | BCM43430A1 bootstrap | 30 | 🔴 Bloqueado em phase 1 |
| [issue_logging_miniuart.md](issue_logging_miniuart.md) | Console fora do PL011, mini-UART compartilhado com Paula | — | ✅ Resolvido |
| [issue_cdrom_boot.md](issue_cdrom_boot.md) | ATAPI CD-ROM boot: RIPPLE board, lide.device, FindCDFS, AROS CDFS gap | — | 🔴 Bloqueado: sem 'CD01' em FSR |
| [issue_multicore_boundary_logging.md](issue_multicore_boundary_logging.md) | Cross-core boundary logging (CORE1↔CORE2, CORE3↔CORE2), cmake source-list cleanup | — | ✅ Resolvido (⚠️ ver `issue_core_log_vs_rigeltrace.md`) |
| [issue_paula_audio_timing.md](issue_paula_audio_timing.md) | Paula AUD0-3 timing trace (6 eventos, 4 canais) + ring buffer consumer | — | ✅ Resolvido (⚠️ harness audio mostly fixed — ver `issue_paula_audio_cpu_chipset_sync.md`) |

## Ações Imediatas (Próxima Sessão)

### 1. Fix LOF bit — Alta Prioridade
**Arquivo**: `src/chipset/agnus/agnus.c`
**O quê**: `beam.lof = 0` em modo PAL não-interlace. VHPOSR bit 15 deve ser 0.
**Impacto**: desbloqueia bitmap producer do KS1.3 → tela de boot aparece.
Detalhes: `issue_harness_ks13_boot_screen.md` + `issue_agnus_dma_copper.md`.

### 2. USB Pi 3B — Validação de msleep + TX FIFO flush
**Arquivo**: `src/io/usb/usb_hc_bellatrix.c`, `src/io/usb/usb_osal_bellatrix.c`
**O quê**: Flash Sprint 42+43 no hardware e verificar se Pi 3B completa primeira transação EP0.
Detalhes: `issue_usb_host_dwc2.md`.

### 3. CD Boot: Inject 'CD01' into FileSystem.resource
**O quê**: `FindCDFS()` in lide.device searches FSR for DosType 'CD01'. AROS ROM doesn't
provide this (AROS CDFS is a handler, not an FSR entry). Two paths:
- **Option A**: expand board to 128KB, embed BootCDFileSystem in second ROM bank (real hardware path).
- **Option B**: DiagArea ROM injects a minimal FileSysEntry with `fse_DosType='CD01'` into FSR at init time.
**Prerequisite**: ADF/ISO harness mutual-exclusion bug fixed (main.c). media_present works.
Detalhes: `issue_cdrom_boot.md`.

### 4. Cross-core CIA→INTREQ Sync
**O quê**: `RuntimeMailbox` ou evento atômico Core 3→Core 2 para propagação CIA ICR.
**Risco atual**: Interrupções CIA podem ser perdidas ou atrasadas.
Detalhes: `issue_multicore_runtime.md` + `issue_interrupt_pipeline.md`.

## Fases do Projeto

| Fase | Deliverable | Status |
|------|-------------|--------|
| 0 | Build + btrace | ✅ |
| 1 | Chip RAM + ROM | ✅ |
| 2 | CIA 8520 completo | ✅ |
| 3 | INTENA/INTREQ/VBL | ✅ |
| 4 | Copper + Bitplanes | ⚠️ (LOF fix pendente) |
| 5 | Happy Hand | ❌ |
