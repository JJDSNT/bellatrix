// AI_context/sprint_03.md

# Sprint 03 — Fase 2: CIAs 8520

## Status: concluído

---

## Issue 1 — CIA struct e implementação completa

### Arquivos criados

`src/variants/bellatrix/chipset/cia.h` — interface pública:
- `CIA_State` struct com todos os campos (ports, DDR, ICR, timers, TOD, latch)
- Constantes para registradores (0-15) e bits de ICR/CRA/CRB
- API: `cia_init`, `cia_read`, `cia_write`, `cia_irq_pending`, `cia_vbl_tick`

`src/variants/bellatrix/chipset/cia.c` — implementação:

**Timers A e B:**
- Lazy real-time update via `CNTPCT_EL0` e `CNTFRQ_EL0` 
- CIA_FREQ_PAL_HZ = 709.379 kHz
- Fórmula: `cia_ticks = (arm_elapsed * CIA_FREQ) / CNTFRQ`
- Underflow: seta `ICR_TA`/`ICR_TB`, reload se contínuo, stop se one-shot
- CRA/CRB `LOAD` bit: força carga imediata do latch

**ICR (Interrupt Control Register):**
- Write: bit 7=1 → seta bits na máscara; bit 7=0 → limpa bits
- Read: snapshot de `icr_data`, retorna bit 7 (IR) se algum bit não-mascarado ativo
- **read-clear**: após leitura, `icr_data` é zerado

**TOD (Time of Day):**
- Fase 2: lazy update via CNTPCT @ 50 Hz (ARM ticks / (CNTFRQ/50))
- Fase 3: substituído por `cia_vbl_tick()` chamado no loop de chipset
- Alarme: seta `CIA_ICR_ALRM` quando `tod == tod_alarm`
- Latch: TODHI lê congela o contador; latch liberado ao ler TODLO

**Ports A/B:**
- Leitura: bits de saída refletem PRA (mascarados por DDRA); bits de entrada retornam 1 (pull-up)
- DDR: 1 = output, 0 = input

---

## Issue 2 — Dispatch em bellatrix.c

`src/variants/bellatrix/bellatrix.c` atualizado:

**CIA-A** despacha quando `addr ∈ [0xBFE001..0xBFEF01]` e `addr & 0xFF == 0x01`:
```
reg = (addr >> 8) & 0xF  → CIA register 0-15
```

**CIA-B** despacha quando `addr ∈ [0xBFD000..0xBFDF00]` e `addr & 0xFF == 0x00`:
```
reg = (addr >> 8) & 0xF
```

**Overlay**: ainda tratado em `bellatrix_bus_access()` via `set_overlay()` quando `reg == CIA_REG_PRA` é escrito na CIA-A (bit 0 = OVL).

**IPL stub (update_ipl):**
```
CIA-B pending → IPL 6 (EXTER)
CIA-A pending → IPL 2 (PORTS)  [se maior que atual]
```
Fase 3 substituirá por roteamento via INTREQ/INTENA (Paula).

**btrace:** CIAs logam com `impl=1` (implementado). Outros endereços continuam com `impl=0`.

---

## Issue 3 — Patches atualizados

`patches/0001-...` agora inclui `chipset/cia.c` na lista `BASE_FILES` do CMakeLists.

---

## Critério de conclusão da Fase 2

- [ ] Build sem erros
- [ ] Kickstart avança além da detecção de hardware (sem travar nos CIAs)
- [ ] btrace mostra `impl=true` para acessos a CIA-A/B
- [ ] IPL trace mostra transições de nível quando timers expiram

---

## Próxima sessão: Fase 3 — INTENA/INTREQ/VBL

**Objetivo:**
1. Implementar `INTENA` / `INTREQ` / `DMACONR` (registradores Agnus básicos)
2. Conectar CIA interrupts via INTREQ (substituir `update_ipl()` stub)
3. VBL interrupt a 50 Hz (usando ARM generic timer, core 0 por enquanto)
4. `cia_vbl_tick()` chamado a cada VBL para avançar TOD

**Arquivos a criar:**
- `src/variants/bellatrix/chipset/agnus.h / agnus.c` — apenas INTENA/INTREQ/DMACON para MVP
- `src/variants/bellatrix/chipset/chipset.c` — dispatcher principal + loop tick

**Critério:** Kickstart chega ao loop de idle aguardando disco;
IPL trace mostra VBL (level 3) periódico a 50 Hz.
