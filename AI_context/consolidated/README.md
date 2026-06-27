# AI_context/consolidated

## Regra de promoção

Nenhuma issue pode ser movida para consolidated sem:

- implementação concluída
- documentação atualizada
- critérios de aceite satisfeitos

## Objetivo

Transformar trabalho concluído em conhecimento permanente.
Investigações históricas de sessão também vivem aqui (arquivos prefixados com `rigel_`, `kbd_`, etc.).

---

## Índice de Issues Consolidadas

| Arquivo | Tópico | Status |
|---------|--------|--------|
| [issue_infrastructure_build.md](issue_infrastructure_build.md) | Build system, patches Emu68, estrutura de diretórios | ✅ Estável |
| [issue_rom_overlay.md](issue_rom_overlay.md) | ROM loading via initramfs, overlay MMU | ✅ Funciona (OVL live path fix) |
| [issue_emu68_jit_integration.md](issue_emu68_jit_integration.md) | CACR_IE, v30 save/restore, ExecutionLoop, bridge, Fast RAM | ✅ Funciona |
| [issue_interrupt_pipeline.md](issue_interrupt_pipeline.md) | INTENA/INTREQ/IPL, FIQ, CIA→Paula chain | ✅ Funciona (⚠️ cross-core timing) |
| [issue_cia_8520.md](issue_cia_8520.md) | CIA 8520 completo: timers, TOD, ports, serial, ICR | ✅ Funciona |
| [issue_agnus_dma_copper.md](issue_agnus_dma_copper.md) | Agnus beam, DMA arbiter, Copper raster-time, Blitter | ⚠️ LOF bug pendente |
| [issue_denise_rendering.md](issue_denise_rendering.md) | Denise bitplanes, sprites, rendering raster-time | ⚠️ Sprites sem compositing completo |
| [issue_paula_serial_floppy.md](issue_paula_serial_floppy.md) | Paula, disk DMA, serial bridge, floppy | ✅ Funciona (⚠️ DiagROM1.3 loopback) |
| [issue_harness_ks13_boot_screen.md](issue_harness_ks13_boot_screen.md) | Investigação tela de boot KS1.3 — bloqueador LOF | 🔴 Bloqueado: LOF=1 permanente |
| [issue_multicore_runtime.md](issue_multicore_runtime.md) | RPi3 multicore: Core 1=CPU, Core 2=Rigel, Core 3=IO | ✅ Funciona (⚠️ cross-core sync) |
| [issue_usb_host_dwc2.md](issue_usb_host_dwc2.md) | CherryUSB + DWC2 host, BCM2837 bring-up | ⚠️ QEMU OK, Pi 3B pendente |
| [issue_bluetooth.md](issue_bluetooth.md) | BCM43430A1 bootstrap | 🔴 Bloqueado em phase 1 |
| [issue_logging_miniuart.md](issue_logging_miniuart.md) | Console fora do PL011, mini-UART compartilhado com Paula | ✅ Resolvido |
| [issue_cdrom_boot.md](issue_cdrom_boot.md) | ATAPI CD-ROM boot: RIPPLE board, lide.device, FindCDFS, AROS CDFS gap | 🔴 Bloqueado: sem 'CD01' em FSR |
| [issue_multicore_boundary_logging.md](issue_multicore_boundary_logging.md) | Cross-core boundary logging (CORE1↔CORE2, CORE3↔CORE2) | ✅ Resolvido |
| [issue_paula_audio_timing.md](issue_paula_audio_timing.md) | Paula AUD0-3 timing trace (6 eventos, 4 canais) + ring buffer consumer | ✅ Resolvido (⚠️ ver ISSUE-0009) |
| [issue_bt_scan_stability.md](issue_bt_scan_stability.md) | BT scan stability | — |
| [issue_disk_dsken_vbl_timing.md](issue_disk_dsken_vbl_timing.md) | Disk DSKEN VBL timing | — |

## Investigações Históricas

| Arquivo | Tópico |
|---------|--------|
| [rigel_aros_adf_investigation.md](rigel_aros_adf_investigation.md) | Investigação completa AROS boot (WORDSYNC, Copper halt, GfxBase, slow RAM) |
| [rigel_ks20_video_investigation.md](rigel_ks20_video_investigation.md) | Investigação vídeo KS2.0 — hires DIW, sprite viewport, BPL1MOD |
| [rigel_graphics_dma_scroll_investigation.md](rigel_graphics_dma_scroll_investigation.md) | Investigação gráfica Rigel — EON 6-plane lores, BPLCON1, DMA bitplane |
| [kbd_hid_cia_chain.md](kbd_hid_cia_chain.md) | USB HID → CIA-A teclado single-core: 4 root causes, handshake KDAT |
| [project_refactoring_sprint11.md](project_refactoring_sprint11.md) | Sprint 11: tipos canónicos CIA/Agnus/Denise, Paula dona de INTREQ/INTENA |

## Ações Imediatas (ver também issues/ ativas)

### 1. Fix LOF bit — Alta Prioridade
**Arquivo**: `src/chipset/agnus/agnus.c`
**O quê**: `beam.lof = 0` em modo PAL não-interlace. VHPOSR bit 15 deve ser 0.
**Impacto**: desbloqueia bitmap producer do KS1.3 → tela de boot aparece.
Detalhes: `issue_harness_ks13_boot_screen.md` + `issue_agnus_dma_copper.md`.

### 2. AROS ROM+ADF Version Mismatch
**O quê**: Obter par ROM+ADF do mesmo build AROS. Detalhes: ISSUE-0015.

### 3. CD Boot: Inject 'CD01' into FileSystem.resource
**O quê**: `FindCDFS()` in lide.device searches FSR for DosType 'CD01'.
Detalhes: `issue_cdrom_boot.md`.

### 4. Cross-core CIA→INTREQ Sync
**O quê**: `RuntimeMailbox` ou evento atômico Core 3→Core 2 para propagação CIA ICR.
Detalhes: `issue_multicore_runtime.md` + `issue_interrupt_pipeline.md`.

## Fases do Projeto

| Fase | Deliverable | Status |
|------|-------------|--------|
| 0 | Build + btrace | ✅ |
| 1 | Chip RAM + ROM | ✅ |
| 2 | CIA 8520 completo | ✅ |
| 3 | INTENA/INTREQ/VBL | ✅ |
| 4 | Copper + Bitplanes | ⚠️ (LOF fix pendente) |
| 5 | Happy Hand | ✅ DONE |
| 6 | Emu68 JIT integration | 🔄 Em progresso (ver ISSUE-0001, 0002, 0003) |
| 7 | AROS desktop | 🔄 Em progresso (ver ISSUE-0015) |
