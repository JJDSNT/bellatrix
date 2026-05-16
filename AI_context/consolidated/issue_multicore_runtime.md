# Issue: Multicore Runtime — RPi3 Bare-Metal

## Contexto

Bellatrix usa os 4 cores do RPi3 com responsabilidades dedicadas. A arquitetura
foi iterada em 3 sprints (27-29) para alinhar os domínios corretamente.

## Arquitetura Final (Sprint 29)

| Core | Domínio | Loop | Implementação |
|------|---------|------|--------------|
| 0 | Emu68 JIT (CPU) | MainLoop | fixo — não alterado |
| 1 | GFX: Agnus+Denise+DMA | `core_gfx_step()` | `src/runtime/core_gfx.c` |
| 2 | Paula: áudio+disk+serial+IRQ/IPL | `core_audio_step()` | `src/runtime/core_audio.c` |
| 3 | IO físico: CIA+teclado+UART host | `core_io_step()` | `src/runtime/core_io.c` |

## Fluxo de Ciclos

### Core 0 → Core 1 (ciclos GFX)
```c
// ExecutionLoop.c (Core 0):
bellatrix_bridge_cpu_progress(bela_delta * 8u)
  → bellatrix_runtime_notify_cpu_progress(cycles)
     → atomic_fetch_add(&s_gfx_cycles_pending, cycles)
     → atomic_fetch_add(&s_io_cycles_pending, cycles)
     → sev                                    ← acorda Core 1 e Core 3
```

### Core 1 (GFX):
```c
bellatrix_runtime_host_step():
  cycles = atomic_exchange(&s_gfx_cycles_pending, 0)
  [acquire chipset lock]
  core_gfx_step(&g_runtime.gfx, cycles)    ← agnus_step, denise, DMA
  s_published_master_cycles = gfx.master_cycles   ← publish para Core 2
  [release chipset lock + sev]
```

### Core 2 (Paula/Audio):
```c
bellatrix_runtime_audio_step():
  master = acquire_load(&s_published_master_cycles)
  [acquire chipset lock]
  core_audio_step(&g_runtime.audio, master)
    → paula_audio_step(..., audio_cycles)
    → paula_disk_step(..., raw_cycles)
    → paula_serial_step(..., raw_cycles)
    → paula_interrupt_update(...)
    → bellatrix_machine_sync_ipl()          ← IPL → JIT
  [release chipset lock + sev]
```

### Core 3 (IO):
```c
bellatrix_runtime_io_step():
  cycles = atomic_exchange(&s_io_cycles_pending, 0)
  [acquire chipset lock]
  core_io_step(&g_runtime.io, cycles)
    → cia_step(cia_a, cia_ticks)
    → cia_step(cia_b, cia_ticks)
    → cia_tod_pulse(cia_b, hsync_pulses)
    → bellatrix_keyboard_step(...)
    → uart_host_poll(...)                   ← serial bridge
  [release chipset lock + sev]
```

## Sincronização

### Spinlock Principal
`s_chipset_lock` — `atomic_flag` (TAS spinlock + WFE)

**Holders**: Core 0 (MMIO write), Core 1 (GFX step), Core 2 (Paula step), Core 3 (IO step)

**Invariante**: Core 0 (MMIO) e Core 1 (advance) nunca seguram o lock simultaneamente.

### Accumulators Atômicos
- `s_gfx_cycles_pending` — `_Atomic uint32_t` — Core 0 produz, Core 1 drena
- `s_io_cycles_pending` — `_Atomic uint32_t` — Core 0 produz, Core 3 drena
- `s_published_master_cycles` — `_Atomic uint64_t` — Core 1 publica, Core 2 lê

### WFE/SEV
- Core 1/2/3 dormem em WFE quando idle
- Core 0 emite SEV quando adiciona ciclos (`notify_cpu_progress`)
- Cada core emite SEV ao liberar lock (para acordar waiters)

## Secondary Boot (start.c — BELLATRIX)

```c
secondary_boot(cpu_id):
  if BELLATRIX:
    if cpu_id == 1: bellatrix_core1_entry()  → chipset_core_loop()
    if cpu_id == 2: bellatrix_core2_entry()  → chipset_audio_loop()
    if cpu_id == 3: bellatrix_core3_entry()  → chipset_io_loop()
    else: while(1) wfe
```

`PAL_Core_LaunchChipset()` → `s_chipset_entry = chipset_core_loop; dsb+sev`
`PAL_Core_LaunchAudio()` → `s_audio_entry = chipset_audio_loop; dsb+sev`
`PAL_Core_LaunchIO()` → `s_io_entry = chipset_io_loop; dsb+sev`

Chamados de `bellatrix_init()` quando `BELLATRIX_MULTICORE=1`.

## Modo Single-Core (Harness / Debug)

`pal_posix.c`: `PAL_Core_IsMulticoreEnabled() = 0`

Weak stubs para todas as funções de runtime. `bellatrix_runtime_notify_cpu_progress()`
cai no stub → `bellatrix_machine_advance()` direto.

## Bug Sprint 28: Correção de Domínios (Sprint 29)

**Problema em Sprint 28**: `paula_interrupt_update`, `paula_disk_step` e
`paula_serial_step` estavam em `core_io_step` (Core 3). Correto seria Core 2.

**Confusão inicial**: Core 2 chamado "Audio" (nome do loop), mas domínio é Paula
completo (áudio + disk + serial + IRQ/IPL).

**Fix Sprint 29**: movidos todos os módulos Paula para `core_audio_step` (Core 2).
Core 3 ficou apenas com CIA, teclado e UART host.

## Bug Sprint 28: SEV Spurious em Single-Core

**Problema**: código single-core emitia `sev` que acordava Cores 1/2/3 no QEMU,
fazendo-os tentar executar antes de `bellatrix_core*_entry()` estar definido.

**Fix**: `bellatrix_runtime_notify_cpu_progress()` verifica
`PAL_Core_IsMulticoreEnabled()` antes de emitir SEV. Em single-core: advance direto.

## Pendentes / Riscos

### Cross-Core CIA→INTREQ (Não Garantido)
Core 3 roda `cia_step()`. Core 2 roda `paula_interrupt_update()`. Propagação
`CIA ICR → INTREQ` depende de ordenação temporal não garantida.
`RuntimeMailbox` ou evento atômico ainda não implementados.
**Risco**: Interrupções CIA podem ser perdidas ou atrasadas.

### RuntimeSync Ready-Flags
`RuntimeSync` ready-flags não conectados. Parte do design mas não wired.

### machine_drain_serial_fallback
`machine_drain_serial_fallback` (kprintf quando sem backend) ausente do caminho
multicore. Afeta apenas debug sem UART aberto.

### Audio Cycle Clamping
Audio em Core 2 recebe `delta` clamped a 4096 ciclos para prevenir burst.
Disk/serial/interrupts recebem `raw_cycles` sem clamp.

## Arquivos Relevantes
- `src/cpu/bellatrix.c` — `s_chipset_lock`, `s_gfx/io_cycles_pending`,
  `bellatrix_runtime_notify_cpu_progress`, `bellatrix_init` (launch cores)
- `src/host/raspi3/pal_core.c` — loops de Core 1/2/3, `PAL_Core_Launch*`
- `src/host/posix/pal_posix.c` — stubs single-core
- `src/runtime/core_gfx.h/.c` — Core 1: Agnus+Denise step
- `src/runtime/core_audio.h/.c` — Core 2: Paula (audio+disk+serial+IRQ)
- `src/runtime/core_io.h/.c` — Core 3: CIA+keyboard+UART
- `emu68/src/aarch64/start.c` — `secondary_boot()` BELLATRIX block
- `patches/0002-add-bellatrix-bus-hook.patch` — secondary_boot hunk
