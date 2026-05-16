# Issue: CIA 8520 — Emulação Completa

## Contexto

As duas CIAs (CIA-A em 0xBFE001, CIA-B em 0xBFD000) são responsáveis por timers,
ports de I/O, keyboard, serial (SP/CNT), TOD e geração de interrupções PORTS/EXTER.
A implementação passou por múltiplas iterações para atingir fidelidade suficiente.

## Decisões Arquiteturais

- **CIA-A**: irq_level=2, paula_irq_bit=PAULA_INT_PORTS (bit 3)
- **CIA-B**: irq_level=6, paula_irq_bit=PAULA_INT_EXTER (bit 13)
- **E-clock**: CIA avança a CPU/10 (7.09MHz/10 ≈ 709kHz). Implementado com
  acumulador `cia_tick_acc` para evitar deriva em quanta pequenos.
- **Módulos separados por responsabilidade** (Sprint 22):
  - `cia_timers.c` — Timer A/B com todos os modos
  - `cia_ports.c` — Port A/B, DDR, FLG
  - `cia_tod.c` — TOD counter, alarm
  - `cia_serial.c` — SP/CNT shift register
  - `cia_interrupt.c` — ICR, mascaramento, raise/clear

## Address Decode

```
CIA-A: endereços pares em 0xBFE000-0xBFEFFF (A0=1, bit 12=1)
  reg = (addr >> 8) & 0xF   → 0-15
CIA-B: endereços pares em 0xBFD000-0xBFDFFF (A0=0, bit 13=1)
  reg = (addr >> 8) & 0xF   → 0-15
```

## Registros Implementados

| Reg | Nome | Implementação |
|-----|------|---------------|
| 0 | PRA/PRB | R/W com DDR mask; CIA-A PRA bit 0 = OVL |
| 1 | DDRA/DDRB | R/W |
| 2 | TALO/TBLO | Timer A/B low latch/counter |
| 3 | TAHI/TBHI | Timer A/B high latch/counter |
| 4 | TODLO/TODMID | TOD low/mid byte |
| 5 | TODMID/TODHI | TOD mid/hi byte |
| 6 | TODHI/blank | TOD hi byte |
| 8 | SDR | Serial Data Register |
| 9 | ICR | read-clear, write: bit7=set/clr mask |
| 10 | CRA | Timer A control |
| 11 | CRB | Timer B control |

## Algoritmo de Timer (Reescrito em Sprint 16)

**Problema anterior**: `cia_timer_advance` com loop `O(ticks/period)` podia
disparar múltiplos underflows por call, causando phantom interrupt storms.

**Algoritmo correto**:
1. Se `ticks ≤ counter`: subtrai, retorna 0 underflows (fast path).
2. Caso contrário: `counter -= (latch + 1)`, recarrega `counter = latch`,
   consome remaining ticks. Máximo 1 underflow adicional por call (one-shot: para).
3. Retorna exatamente 1 quando underflow ocorreu.

**Modos de Timer B**:
- Contagem normal (phi2 clock)
- Contagem de pulsos CNT
- Contagem de underflows de Timer A
- Contagem de underflows de Timer A com CNT HIGH
(Sprint 22)

## TOD Counter

- **CIA-A**: clocked a 50Hz (VBL via `cia_vbl_tick()`)
- **CIA-B**: clocked via sinal /INDEX do floppy drive (Sprint 14: corrigido de
  454 ticks→0; só incrementa com `cia_tod_pulse` de Agnus hsync)
- **Latch**: leitura de TODHI congela o contador; liberado ao ler TODLO
- **TOD clock writes**: pausa clock até write do byte low (comportamento 8520 real,
  Sprint 22)
- **Alarme**: `ICR_ALRM` quando `tod == tod_alarm`

## ICR — Interrupt Control Register

- **Write**: bit 7=1 → seta bits na máscara; bit 7=0 → limpa bits
- **Read**: retorna snapshot + bit 7 (IR) se algum bit non-masked ativo; **read-clear**
- **Raise**: `cia_raise_icr()` → notifica Paula via `paula_irq_raise()`
- **ACK** (CPU leu ICR): `cia_read_reg(ICR)` → `paula_irq_clear()`

## Ports e Sinais Externos

### CIA-A Ports (Sprint 22)
- **PRA**: bit 0=OVL, bit 6=LMB, bit 1=disk_change
- **PRB**: não utilizado por CIA-A de forma significativa
- **ext_pra = 0xFF** em reset (pull-up; DiagROM checa bit 6=1 para não abortar)
- **DDRA mask**: bits de saída refletem PRA; bits de entrada retornam `ext_pra`

### CIA-B Ports (floppy — Sprint 16)
- **PRB**: MOTOR (/MTR bit 7), SELECT (/SELx bits 3-6), STEP (/STEP bit 0),
  DIR (/DIR bit 1), SIDE (/SIDE bit 2)
- **PRA (CIA-B Port A)**: /INDEX (bit 4), /TRACK0 (bit 3), /WPRO (bit 2),
  /DSKCHG (bit 2), /RDY (bit 5)
- Sprint 16: corrigido bug de direção (/DIR=1 = step OUT = cylinder--; estava invertido)
- Sprint 16: /DSKCHG latch correto: mantém asserted sem disco; limpa só com step+disco
- Sprint 16: /WPRO adicionado (campo `write_protected` no FloppyDrive)

### FLG (Sprint 22)
Modelado como falling-edge external input source; `CIA_ICR_FLG` setado no flanco.

## Serial CIA (Sprint 22)

**Modo input (SP como input, CNT gerado externamente)**:
- Shift register de 8 bits clocked por CNT
- Após 8 bits recebidos: dados disponíveis em SDR, `CIA_ICR_SP` setado
- Usado para teclado Amiga (CIA-A): controller envia pulsos SP/CNT

**Modo output (SPMODE=1)**:
- Shift register clocks SP em relação a CNT interno
- Após transmissão: `CIA_ICR_SP` setado
- Regressão corrigida (Sprint 22 follow-up): SPMODE deve ser resetado após handshake
  para não travar recepção subsequente

## PBON / OUTMODE (Sprint 22)

`PBON` (CRA bit 1) e `OUTMODE` (CRB bit 1): quando ativos, PB6/PB7 são conectados
às saídas dos timers. Antes implementados como control-register bits sem efeito.
Sprint 22: drive correto via timer output path.

## Machine: E-clock Accumulator (Sprint 14)

```c
m->cia_tick_acc += ticks;
cia_ticks = m->cia_tick_acc / 10;
m->cia_tick_acc %= 10;
cia_step(&m->cia_a, cia_ticks);
cia_step(&m->cia_b, cia_ticks);
```

Antes CIA avançava à velocidade do CPU (7MHz) — timers 10× rápido demais.

## Teclado Amiga (Sprint 22)

**Protocolo**: CIA-A SP/CNT. Controller Amiga envia byte de key (inverted, bit-shifted).
`bellatrix_keyboard_step()`: entrega byte quando CIA está pronta (handshake via ICR SP).

**Regressão Sprint 22 follow-up**: SPMODE remaining set após primeiro handshake
trava recepção. Fix: reset SPMODE após handshake.

Harness SDL: mouse delta, 3 botões → `controller_port` API → JOY0DAT, POTGOR, CIAAPRA.

## Estado Atual

### O que Funciona
- Timers A/B com algoritmo correto (single underflow por step)
- ICR com mascaramento, read-clear
- TOD com latch, alarme, clock pause on write
- Ports com DDR, pull-ups, signals externos
- FLG edge detection
- Serial CIA (SP/CNT) para teclado
- PBON/OUTMODE via timer output
- Notificação Paula via attach/raise/clear
- E-clock accumulator (CPU/10)
- Todos os modos de Timer B

### Pendente
- **Cross-core CIA→INTREQ timing**: Core 3 roda CIA step, Core 2 roda paula update.
  Propagação não atomicamente garantida entre os dois. `RuntimeMailbox` necessário.
- **CIA-B `/INDEX` pulse**: `cia_tod_pulse()` chamado de Agnus hsync no `core_io_step`.
  Verificar que latência entre Agnus beam e Core 3 não introduz drift de TOD.

## Arquivos Relevantes
- `src/chipset/cia/cia.h/.c` — struct CIA, init, reset, attach, dispatch
- `src/chipset/cia/cia_timers.h/.c` — Timer A/B, algoritmo, modos
- `src/chipset/cia/cia_ports.h/.c` — Port A/B, DDR, FLG
- `src/chipset/cia/cia_tod.c` — TOD, latch, alarm, clock pause
- `src/chipset/cia/cia_serial.h/.c` — SP/CNT shift register
- `src/chipset/cia/cia_interrupt.h/.c` — ICR engine
- `src/core/machine.c` — E-clock accumulator `cia_tick_acc`
- `src/runtime/core_io.c` — Core 3: `cia_step`, `cia_tod_pulse`, keyboard
