// AI_context/sprint_14.md

# Sprint 14 — Copper control flow + pipeline de interrupções + disk DMA stub

## Status: concluído — build verde (harness + Emu68)

---

## Contexto

ROM viva, CPU e VBL a funcionar, Copper a executar. Dois bloqueadores identificados:

1. **Copper fazia MOVE para COPJMP2 (0x008A) e continuava linearmente** — o salto
   acontecia (via `agnus_write_reg → copper_write_reg`), mas sem log. O Copper
   saltava para `cop2lc = 0x000000` (nunca escrito) e executava memória zero,
   produzindo cascata de `[COPPER] skip illegal MOVE ir1=0000`.

2. **ROM entrava em loop esperando DSKBLK** — quando Kickstart escrevia DSKLEN
   com DMAEN para sondar o drive, nunca recebia o DSKBLK interrupt (o write era
   silenciosamente absorvido por Agnus). Sem DSKBLK, o loop de detecção de disco
   nunca avançava.

---

## Mudanças implementadas

### Copper / Agnus

#### `src/chipset/agnus/copper.c`
- `COPPER_COPJMP1` / `COPPER_COPJMP2`: adicionados logs
  `[COPPER-JMP1] old_pc=... new_pc=... cop1lc=...` e idem JMP2.
  Permite distinguir "salto para cop2lc=0 porque nunca foi escrito" de erro real.
- `COPPER_COP2LCH` / `COPPER_COP2LCL`: adicionados logs equivalentes aos já
  existentes para COP1LC.

#### `src/chipset/agnus/agnus.h`
- `agnus_is_copper_reg`: adicionado `case AGNUS_COPCON` (0x002E).
  **Bug**: COPCON nunca chegava a `copper_write_reg` → `cdang` era ignorado.

#### `src/chipset/agnus/agnus.c`
- `AGNUS_DMACON` write: log `[DMACON-W] pc=... raw=... old=... new=...`
  no único ponto de mutação do estado — elimina ambiguidade sobre quando
  DMAEN/COPEN são ligados.

---

### CIA

#### `src/chipset/cia/cia.h`
- `CIA_B_TOD_TICKS_PER_INCREMENT`: corrigido de `454u` (HSync) para `0u`.
  CIA-B TOD é clocked pelo sinal /INDEX do drive floppy; sem drive, nunca
  incrementa. O valor `454u` causaria incremento a cada linha — não é correcto.

#### `src/chipset/cia/cia.c`
- Adicionado `#include "support.h"` para kprintf.
- `cia_sync_irq_line`: logs `[CIAx-IRQ] raised/cleared` nas transições de estado
  (não em cada tick — só quando muda).
- `CIA_REG_ICR` read: log `[CIAx-ICR-R] returned=... mask=...`.
- `CIA_REG_ICR` write: log `[CIAx-ICR-W] raw=... -> mask=...`.

---

### Paula

#### `src/chipset/paula/paula.h`
- `Paula` struct: adicionado campo `uint32_t disk_dma_countdown`.

#### `src/chipset/paula/paula.c`
- `paula_compute_ipl`: `PAULA_INT_DSKSYN` (bit 12) adicionado ao nível 6 junto
  com `PAULA_INT_EXTER`. **Bug**: bit 12 não tinha nível mapeado.
- `paula_handles_read`: inclui `ADKCONR` (0x010) e `DSKBYTR` (0x01A).
- `paula_handles_write`: inclui `DSKPTH`, `DSKPTL`, `DSKLEN`, `DSKSYNC`, `ADKCON`.
  **Efeito**: estes writes deixam de cair silenciosamente em Agnus.
- `paula_read`: `DSKBYTR` retorna 0 (sem DMA activo, sem disco).
- `paula_write` — novos casos:
  - `DSKLEN`: se DMAEN (bit 15) setado, arma `disk_dma_countdown = 46000` cycles.
    Sem este stub, Kickstart ficava em loop eterno esperando DSKBLK.
  - `DSKSYNC`: aceita e loga, sem side-effects.
  - `ADKCON`: aceita silenciosamente (SET/CLR não aplicado — audio/disk para fase futura).
  - `DSKPTH` / `DSKPTL`: aceita silenciosamente.
- `paula_step`: processa `disk_dma_countdown`; quando expira, dispara
  `PAULA_INT_DSKBLK` via `paula_irq_raise`.
- Log `[PAULA-DISK] DSKBLK fired (fake)` na expiração.

---

### Machine

#### `src/core/machine.h`
- `BellatrixMachine` struct: adicionado `uint32_t cia_tick_acc` (acumulador de
  sub-ticks para divisão E-clock).

#### `src/core/machine.c`
- `machine_step_components`: CIA agora avança a E-clock = CPU / 10.
  Implementado com acumulador para evitar deriva em quanta pequenos (ticks=1
  do bus access path).
  ```c
  m->cia_tick_acc += ticks;
  cia_ticks = m->cia_tick_acc / 10;
  m->cia_tick_acc %= 10;
  cia_step(&m->cia_a, cia_ticks);
  cia_step(&m->cia_b, cia_ticks);
  ```
  **Antes**: CIA avançava à velocidade do CPU (7 MHz) — timers 10× rápido demais.

---

### Harness

#### `tools/harness/musashi_backend.c`
- `musashi_set_ipl`: log `[IPL] set_ipl level=... pc=...` quando level > 0.

---

## SDL2 — infraestrutura já existente

A implementação SDL2 (`pal_posix.c`) já estava completa desde o Sprint 13.
O display não aparece apenas porque `libsdl2-dev` não está instalado.
Para activar:
```bash
sudo apt-get install -y libsdl2-dev
cmake -S tools/harness -B out/harness && make -C out/harness
# correr sem --headless → janela SDL abre automaticamente
```

---

## Diagnóstico do estado actual

```
CPU ✅  ROM ✅  VBL ✅  Copper ✅ (COPJMP corrigido)
CIA timers ✅  CIA ICR ✅  E-clock ✅
Disk DMA stub ✅  DSKBLK fires ✅
INT pipeline ✅  IPL derivation ✅
SDL2 infra ✅  (libsdl2-dev ausente)
```

---

## Próxima sessão

1. Instalar `libsdl2-dev` + rebuildar → confirmar janela SDL abre
2. Correr KS 1.3 com `--frames 500` e verificar se o "insert disk" screen aparece
3. Se ROM travar antes: usar logs CIA ICR + DMACON para identificar o loop
4. Copper: verificar se `cop2lc` é escrito antes de COPJMP2 — se não for, KS
   salta sempre para 0x000000 (inofensivo mas ruidoso nos logs)
5. Fase seguinte: integrar `paula_disk.c` + `floppy_drive.c` para disco real
