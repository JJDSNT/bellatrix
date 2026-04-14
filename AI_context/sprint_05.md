// AI_context/sprint_05.md

# Sprint 05 — Fase 3: INTENA/INTREQ/VBL

## Status: concluído

---

## Issue 1 — Agnus: INTENA/INTREQ/DMACON/VPOSR

`src/chipset/agnus/agnus.h / agnus.c`

Globals acessíveis pelo FIQ handler via adrp:
- `uint16_t bellatrix_intena` — espelho de INTENA
- `uint16_t bellatrix_intreq` — espelho de INTREQ
- `uint64_t bellatrix_vbl_interval` — ARM ticks por frame (62500000/50 = 1250000)

Funcionalidades:
- `agnus_compute_ipl()` — priority encoder completo (bits 0-13, master enable bit 14)
- `agnus_intreq_set/clear()` — atualiza INTREQ e chama PAL_IPL_Set/Clear via notify_ipl()
- `agnus_vbl_fire()` — seta INTREQ[VERTB] + tick CIA TOD; chamado do FIQ handler
- VPOSR/VHPOSR simulado via CNTPCT_EL0 (313 linhas PAL @ 50 Hz)

---

## Issue 2 — ARM Generic Timer → VBL FIQ

`src/host/raspi3/pal_core.c` (PAL_ChipsetTimer_Init)

- Lê CNTFRQ_EL0, calcula `bellatrix_vbl_interval = CNTFRQ / hz`
- Configura CNTV_TVAL_EL0 e CNTV_CTL_EL0 (EL1 virtual timer)
- Roteia CNTVIRQ como FIQ no core 0: `LOCAL_TIMER_FIQ_CTRL0 = 0x08`
- Chamado de `bellatrix_init()` com hz=50

---

## Issue 3 — FIQ handler: patch vectors.c

`patches/0002-...` — `vectors.c`

Estrutura do handler:
```c
curr_el_spx_fiq:
    stp x0, x1, [sp, -16]!
    mrs/orr/msr SPSR
#ifdef BELLATRIX
    mrs CNTV_CTL_EL0; tbz #2, 1f    // só age se ISTATUS set
    reload CNTV_TVAL_EL0             // 3 instructions
    set bellatrix_intreq |= 0x0020   // 4 instructions
    check intena bits 14+5           // 2 instructions
    set M68KState.INT.IPL=3, ARM=3   // 6 instructions
    // total ~17 instructions = 68 bytes (sob o limite de 128)
#else
    [código original PiStorm]
#endif
1:  ldp x0, x1, [sp], #16
    eret
```

Note: `INT.ARM` recebe o VALOR do IPL (não apenas 1) para funcionar
corretamente na comparação do label 9 do execution loop.

Constraint `[ipl]` adicionado à lista de `__stub_vectors`.

---

## Issue 4 — Execution loop label 9

`patches/0002-...` — `start.c`

- `#ifdef PISTORM` → `#if defined(PISTORM) || defined(BELLATRIX)` (linha ~1900)
  - Ativa label 9 para BELLATRIX: checa INT.ARM_err, INT.ARM, INT.IPL
- `#ifndef PISTORM32` → `#if !defined(PISTORM32) && !defined(BELLATRIX)` (linha ~1914)
  - Pula leitura de GPIO para BELLATRIX (usa INT.IPL diretamente)

---

## Issue 5 — PAL_IPL_Set fix

`src/host/raspi3/pal_ipl.c`

`ctx->INT.ARM = ipl_level` (antes era `= 1`)

Razão: label 9 do execution loop compara `w10 (INT.ARM) cmp w1 (INT.IPL)` e
seleciona o maior. Para BELLATRIX ambos devem ter o mesmo valor IPL.

---

## Build e QEMU

Build: 100% `[100%] Built target Emu68.elf`

QEMU:
```
{"t":"init","msg":"Bellatrix Phase 3 ready"}
{"t":"btrace","tick":21876014,"m68k_pc":"0x00000000","addr":"0x000000e4","dir":"W",...}
```

Sem Kickstart ROM: PC=0, ISP=0, crash esperado. Com ROM (via initrd no QEMU ou
SD card) o Kickstart pode agora usar INTENA/INTREQ e receber VBL a 50 Hz.

---

## Próxima sessão: Fase 4 (conversar antes)

Envolve video output via VC4/HDMI. Requer discussão antes de implementar.
