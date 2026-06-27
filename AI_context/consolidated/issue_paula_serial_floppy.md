# Issue: Paula, Serial, Floppy/Disk

## Status: CLOSED (2026-06-26)

Floppy (ADF), disk DMA, DSKBLK, MFM, serial bridge e audio channels
operacionais — confirmado pelo boot do Workbench via ADF. PORTS level-sensitive
implementado em `cia_interrupt_sync_irq_line`. Itens pendentes menores
(DiagROM loopback, audio DMA como requester real) são refinamentos futuros.

## Contexto

Paula é dona de INTREQ/INTENA e hospeda o motor de disk DMA, serial (SERDAT/SERDATR),
áudio e geração de interrupções. O caminho serial bare-metal (guest→UART host) e o
disk DMA (ADF→MFM→chip RAM) foram as principais áreas de work.

## Arquitetura Paula

### Módulos
- `paula.c` — dispatch de registros, lifecycle, wiring principal
- `paula_interrupt.h/.c` — engine de INTREQ/INTENA/IPL
- `paula_disk.h/.c` — disk DMA (DSKPTH/L, DSKLEN, DSKSYNC, ADKCON)
- `paula_serial.h/.c` — SERDAT/SERDATR serial guest
- `paula_audio.h/.c` — AUD0-3 (channels, DMA, IRQ)
- `paula_input.h/.c` — POTGO/POTGOR, mouse-right state

### Registros
- INTENA/INTREQ/INTENAR/INTREQR — interrupt control
- DSKPTH/L, DSKLEN, DSKBYTR, DSKSYNC, DSKDATR — disk DMA
- SERPER, SERDAT, SERDATR — serial
- ADKCON — disk/audio modes
- POTGO, POTGOR — controller ports  
- JOY0DAT, JOY1DAT, POT0DAT, POT1DAT — joystick/mouse
- AUD0-3: LCH/LCL/LEN/PER/VOL/DAT — audio

## Disk DMA (PaulaDisk)

### Fluxo
1. CPU escreve DSKPTH/L (DMA pointer), DSKLEN (len + DMAEN bit)
2. `paula_disk_start_dma()`: prepara track MFM no buffer interno (via `floppy_read_track`)
3. Agnus DMA arbiter: grants para `AGNUS_DMA_REQ_DISK` quando DSKEN=1
4. `paula_disk_dma_service_grant()`: copia 1 word para chip RAM por grant
5. Ao completar: `paula_irq_raise(PAULA_INT_DSKBLK)` → `[PAULA-DISK] DSKBLK fired`

### Fake DMA (Modelo Antigo — removido)
Sprint 14 introduziu `disk_dma_countdown = 46000 cycles` como stub. Sprint 21 migrou
para modelo real via DMA grants, removendo o countdown como modelo autoritativo.

### DSKBYTR — Status
- `WORDSYNC` setado quando sync word match durante DMA
- `DSKBYTR.BUSY` quando DMA ativo

### MFM Encoding
`encode_adf_track_to_mfm()` com inserção correta de clock bits.

### Checksum
`mfm_checksum()` opera em raw bytes (XOR de words 32-bit big-endian, sem `& 0x55`).
O mascaramento 0x55 pertence ao even/odd encoding, não ao checksum.

## Floppy Drive

### Sinais
- `/MOTOR` — bit 7 de CIA-B PRB
- `/STEP` — bit 0 de CIA-B PRB (step pulse)
- `/DIR` — bit 1 de CIA-B PRB (DIR=1 = step OUT = cylinder--)
- `/SIDE` — bit 2 de CIA-B PRB (lado 0/1)
- `/SELx` — bits 3-6 de CIA-B PRB (select drive)
- `/INDEX` → CIA-B TOD clock (via `cia_tod_pulse`)
- `/TRACK0` — bit 3 de CIA-B PRA (ativo quando cyl==0)
- `/RDY` — bit 5 de CIA-B PRA (drive pronto)
- `/DSKCHG` — bit 2 de CIA-B PRA (disk change latch)
- `/WPRO` — bit 3 de CIA-B PRA (write protected)

### ADF Image
`floppy_drive.c`: open file, track cache, cylinder/side step.
Todos os ADFs default `write_protected=1` no insert.

### Bug Sprint 16: Direção Invertida
DIR=1 incrementava cylinder. Hardware real: DIR=1 = step OUT = cylinder--.
Fix: lógica invertida com comentário explícito.

### Bug Sprint 16: /DSKCHG sem disco
`floppy_step()` limpava disk_changed ao receber step sem disco. Hardware: latch
deve manter assertado até step COM disco inserido.
Fix: `disk_changed=1` mantido quando drive vazio.

### Bug Sprint 16: /RDY em ID mode
ID mode: /RDY deve espelhar `idbit` (LOW quando 0, HIGH quando 1). Antes: sempre
LOW em ID mode, dando sequência de identidade errada.

## Serial Guest — Paula (PaulaSerial)

### Pipeline Serial
```
[GUEST] CPU escreve SERDAT → PaulaSerial TX queue
   (Core 2) paula_serial_step() → avança TX shift register
   (Core 3) uart_host_poll() → drena TX → PL011/miniuart backend → UART físico
   (Core 3) uart_host_poll() → injeta RX → PaulaSerial RX buffer → SERDATR
```

### Bug Sprint 15: SERDATR TBE oscillação
Em instant-TX mode, `uart_write_serdat()` entrava na state machine de buffer,
causando `tx_shift_busy=true` por 10 ciclos → TBE=0 → AROS loopeava ~20×/byte.
Fix: instant mode emite byte e dispara TBE sem tocar `tx_buffer_valid/tx_shift_busy`.

### Bug Sprint 16: TBE interrupt espúrio
`uart_raise_irq(UART_INTREQ_TBE)` sendo chamado em instant-TX mode → injetava
bit 0 em INTREQ durante boot → spurious IPL=1.
Fix: linha comentada em instant-TX.

### Bug Sprint 20: Bridge não drenando Paula TX
**Root cause crítico**: `uart_host_open_pl011()` chamava `uart_host_shutdown(host)`
que zeruzava `host->paula_serial = NULL`. Posterior `uart_host_poll()` retornava
cedo por `!host->paula_serial`.
Fix: `uart_host_open_pl011()` salva/restaura `host->paula_serial`.

**Log da path funcional**:
```
[SERIAL-TX] first SERDAT=0149 byte=49
[SERIAL-BRIDGE] draining Paula TX byte=49
[PL011-WRITE] first byte=49 CR=00000301
```

### Serial Loopback (DiagROM 1.3)
DiagROM 1.3 testa "serial loopback adapter". Implementado `NULL_MODEM_LOOPBACK`
(echo TX→RX) mas DiagROM 1.3 ainda não detecta como loopback válido.
Status: não resolvido. DiagROM 2.0 preferido para bring-up.

### Modo Instant vs Normal
Sprint 22: `paula_serial.c` default mudado para **não-instant**. TX avança via
`paula_step()`. Instant mode disponível para testes focados ou bridge-style.

## Áudio Paula (Sprint 21)

### Antes do Sprint 21
`paula_audio.c` existia mas estava fora do live path:
- `Paula` não embeddia `PaulaAudio`
- Paula dispatch não roteava `AUDx*` writes
- `paula_audio.c` não compilado no harness/Emu68

### Após Sprint 21
- `Paula` embeds `PaulaAudio audio`
- `paula_init/reset()` inicializam audio
- AUD0..AUD3 registers (LCH/LCL/LEN/PER/VOL/DAT) roteados
- `paula_step()` avança audio
- DMACON writes sincronizam via `paula_audio_set_dmacon()`
- `paula_audio.c` no build list

## Estado Atual

### O que Funciona
- Disk DMA via grants (1 word/grant, DSKBLK real)
- MFM encoding/decoding
- Floppy signals (TRACK0, DSKCHG, WPRO, RDY, INDEX→TOD)
- Serial bridge guest→host (bare-metal confirmado)
- PaulaSerial non-instant com step via paula_step
- Audio channels AUD0-3 live
- POTGO/POTGOR, JOY0/1DAT, POT0/1DAT
- DSKDATR latch do word corrente durante DMA

### O que Ainda Está Incompleto
- DiagROM 1.3 loopback detection não resolvido
- Áudio DMA não testado em hardware real (MFM de audio channels)
- Sprites com audio não testados em conjunto

### Pendente
- Wire `irq_line_level` para PORTS level-sensitive (CIA-A ICR deve manter PORTS
  ativo enquanto pendente — ver `issue_interrupt_pipeline.md`)
- Audio DMA como real DMA requester via arbiter (atualmente apenas `paula_audio_step`)

## Arquivos Relevantes
- `src/chipset/paula/paula.h/.c` — dispatch, wiring, step
- `src/chipset/paula/paula_interrupt.h/.c` — INTREQ/INTENA engine
- `src/chipset/paula/paula_disk.h/.c` — disk DMA, MFM
- `src/chipset/paula/paula_serial.h/.c` — SERDAT/SERDATR
- `src/chipset/paula/paula_audio.h/.c` — AUD0-3 channels
- `src/chipset/paula/paula_input.h/.c` — POTGO/POTGOR
- `src/chipset/floppy/floppy_drive.h/.c` — ADF, MFM, sinais
- `src/io/serial/uart_host.h/.c` — bridge serial guest→host
- `src/host/raspi3/pl011_backend.c` — backend PL011 bare-metal
- `src/runtime/core_audio.c` — Core 2: paula_disk_step, paula_serial_step, paula_audio_step
- `src/runtime/core_io.c` — Core 3: uart_host_poll (bridge drain)
