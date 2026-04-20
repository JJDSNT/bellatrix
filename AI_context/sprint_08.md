// AI_context/sprint_08.md

# Sprint 08 — Fix CIA→IPL: __m68k_state em vez de TPIDRRO_EL0

## Status: concluído

---

## Problema

O sistema gerava corretamente o TOD alarm da CIA-B (`icr_d=0x04, irq=1`), mas
a IRQ não era promovida a IPL no M68K. O KS ficava parado sem receber
interrupções.

---

## Diagnóstico

`PAL_ChipsetTimer_Init()` capturava `s_m68k_ctx` lendo `TPIDRRO_EL0`:

```c
asm volatile("mrs %0, TPIDRRO_EL0" : "=r"(s_m68k_ctx));
```

Mas `PAL_ChipsetTimer_Init` é chamado dentro de `bellatrix_init()`, que roda
**antes** de `M68K_StartEmu()`. O `TPIDRRO_EL0` só é setado na linha 2251 de
`start.c`, dentro de `M68K_StartEmu`, imediatamente antes de `MainLoop()`:

```c
/* start.c linha 2251 */
asm volatile("msr TPIDRRO_EL0, %0"::"r"(&__m68k));
```

Resultado: `s_m68k_ctx = NULL` em tempo de init. `PAL_IPL_Set` checava
`if (!ctx) return` e saía sem fazer nada — a interrupção era gerada, mas nunca
chegava ao JIT.

---

## O que o ps32_protocol.c mostrava

O PiStorm não usa `TPIDRRO_EL0`. O housekeeper acessa o estado M68K via
`__m68k_state`, um ponteiro global definido em `start.c`:

```c
/* start.c linha 2090 */
struct M68KState *__m68k_state;

/* start.c linha 2109, dentro de M68K_StartEmu, ANTES de MainLoop() */
__m68k_state = &__m68k;

/* ps32_protocol.c — housekeeper */
__m68k_state->INT.IPL = ~pin & 7;
```

`__m68k_state` é setado antes de `MainLoop()` e antes de `TPIDRRO_EL0`.
É válido de qualquer core (Core 0 e Core 1 apontam para o mesmo estado).

---

## Fix implementado

**`src/host/raspi3/pal_ipl.c`**: substituído `s_m68k_ctx` por `__m68k_state`.

```c
// antes
extern struct M68KState *volatile s_m68k_ctx;
struct M68KState *ctx = s_m68k_ctx;

// depois
extern struct M68KState *__m68k_state;
struct M68KState *ctx = __m68k_state;
```

**`src/host/raspi3/pal_core.c`**: removidos `s_m68k_ctx` e a captura de
`TPIDRRO_EL0` em `PAL_ChipsetTimer_Init`. Comentários desatualizados corrigidos.

Build: verde (`[100%] Built target Emu68.elf`).

---

## Próxima sessão

- Flash na placa / rodar no QEMU e verificar no btrace/log se o IPL chega ao
  M68K após CIA alarm (esperado: `INT.IPL` e `INT.ARM` setados, KS entra no
  handler de interrupção).
- Checar se o VBL (Core 1 / FIQ) também está chegando corretamente com o fix.
