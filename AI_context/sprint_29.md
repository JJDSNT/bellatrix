# Sprint 29 — Correção arquitetural de domínios: Core 2 = Paula, Core 3 = IO físico

## Contexto

Sprint 28 havia ativado Cores 2 e 3, mas com um desvio arquitetural importante:
`paula_interrupt_update`, `paula_disk_step` e `paula_serial_step` estavam em
`core_io_step` (Core 3) em vez de `core_audio_step` (Core 2). O Core 3 estava
fazendo trabalho que pertence ao domínio Paula.

## Diagnóstico

Conforme `temp2.md`, a separação correta é:

```
Core 2 — Paula: áudio + disk Paula + serial Paula + INTREQ/INTENA/IPL
Core 3 — IO físico: CIA + teclado + UART host
```

O desvio era:
- `core_io_step_interrupts()` chamava `paula_interrupt_update` + `bellatrix_machine_sync_ipl` → deveria ser Core 2
- `core_io_step_serial()` chamava `paula_serial_step` (registro Paula) → deveria ser Core 2
- `core_io_step_disk()` chamava `paula_disk_step` (registro Paula) → deveria ser Core 2

## Correções

### `src/runtime/core_audio.c` — reescrito

Core 2 agora é o dono completo do domínio Paula:

```c
core_audio_step(core, target_master_cycles):
    delta = target - last_master_cycles
    raw_cycles = (uint32_t)delta          // disk/serial/interrupts — sem clamp
    audio_cycles = min(delta, 4096)       // áudio — clampado contra burst

    paula_audio_step(..., audio_cycles)
    paula_disk_step(..., raw_cycles)      // DSKBYTR/DSKLEN
    paula_serial_step(..., raw_cycles)    // SERDAT/SERDATR
    paula_interrupt_update(...)           // INTREQ/INTENA consolidação
    bellatrix_machine_sync_ipl()          // IPL → CPU
```

Includes adicionados: `paula_disk.h`, `paula_serial.h`, `paula_interrupt.h`, `core/machine.h`.

### `src/runtime/core_io.c` — reescrito

Core 3 agora representa apenas o mundo físico externo:

```c
core_io_step(core, cycles):
    cia_step(cia_a, cia_ticks)            // timers E-clock (CPU/10)
    cia_step(cia_b, cia_ticks)
    cia_tod_pulse(cia_b, hsync_pulses)    // TOD de Agnus hsync
    bellatrix_keyboard_step(...)          // protocolo teclado CIA-A SP/CNT
    uart_host_poll(...)                   // UART físico host (TX drain / RX inject)
```

Removidos: `paula_serial_step`, `paula_disk_step`, `paula_interrupt_update`,
`bellatrix_machine_sync_ipl`, `machine_drain_serial_fallback`.

Includes removidos: `paula_serial.h`, `paula_interrupt.h`, `paula_disk.h`.

### Floppy drive

`floppy_step()` não precisa de step periódico em `core_io_step`. Ele é acionado
por escrita em CIA-B PRB via `machine_floppy_update()` no caminho do bus dispatch
— comportamento correto e mantido.

## Fluxo correto após esta sprint

### Interrupções CIA → CPU:
```
Core 3: cia_step() → CIA timer underflow → ICR set
    ↓ (próximo passo de Core 2)
Core 2: paula_interrupt_update() → lê CIA ICR → INTREQ set
Core 2: bellatrix_machine_sync_ipl() → calcula IPL → publica ao JIT
    ↓
Core 0: JIT detecta IPL → exception
```

### Serial:
```
Core 3: uart_host_poll() → injeta RX em PaulaSerial / drena TX do PaulaSerial
Core 2: paula_serial_step() → timing SERDAT/SERDATR, IRQ serial
```

### Disk Paula:
```
Core 3: floppy_step() via CIA-B PRB write (bus dispatch)
Core 2: paula_disk_step() → DSKBYTR/DSKLEN/ADKCON
Core 1: Agnus concede DMA slot
```

## Resultado

- 7/7 testes passando
- Build bare-metal verde (Emu68.elf)
- Domínios de Core 2 e Core 3 alinhados com `temp2.md`

## Estado dos cores

| Core | Domínio | Implementado |
|------|---------|-------------|
| 0 | Emu68 JIT | ✓ (fixo) |
| 1 | Agnus + Denise + DMA | ✓ `core_gfx_step` |
| 2 | Paula (áudio + disk + serial + IRQ/IPL) | ✓ `core_audio_step` |
| 3 | CIA + teclado + UART host | ✓ `core_io_step` |

## Pendente

- Sincronização cross-core: CIA ICR → INTREQ (Core 3 → Core 2) ainda depende de
  ordenação temporal não garantida. Requer `RuntimeMailbox` ou evento atômico.
- `RuntimeSync` ready-flags não conectados.
- `machine_drain_serial_fallback` (kprintf quando sem backend) ausente do caminho
  multicore — afeta apenas debug sem UART aberto.
- Cores 2 e 3 podem executar com `s_chipset_lock` contendo os 4 cores (0–3) —
  sincronização fina futura via RuntimeMailbox/RuntimeEvent.
