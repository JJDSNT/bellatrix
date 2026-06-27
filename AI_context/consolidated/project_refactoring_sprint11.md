---
name: Refactoring estrutural Sprint 11
description: Estado pós-migração arquitectónica — tipos, ownership, singletons eliminados
type: project
---

Migração estrutural completa executada no Sprint 11. Build verde.

**Decisões fixadas:**
- `CIA_State` → `CIA` (typedef); `AgnusState` → `Agnus` (typedef); `Denise` é struct explícita
- `CIA_ID` enum: `CIA_PORT_A` (irq_level=2, PORTS) / `CIA_PORT_B` (irq_level=6, EXTER)
- Paula é a única dona de INTREQ/INTENA; `agnus_compute_ipl` foi removido
- `agnus_intreq_set/clear` mantidos como wrappers que delegam para Paula
- `bellatrix_machine_advance(uint32_t ticks)` é a API pública de avanço temporal (substituiu `machine_step`)

**Why:** docs/roadmap.md e docs/timing_and_architetura.md definem este modelo como canónico.

**How to apply:** Nunca reintroduzir `CIA_State` como nome primário, nem `INT_*` globais em agnus.h, nem singleton em denise.c. Se Paula precisar de novos registos, adicionar a `paula_handles_read/write`.
