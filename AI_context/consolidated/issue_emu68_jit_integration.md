# Issue: Emu68 JIT Integration — CACR_IE, v30, ExecutionLoop, Bridge

## Contexto

A integração profunda entre Bellatrix e o JIT do Emu68 envolveu vários bugs não
óbvios relacionados ao modelo de execução do JIT, ao ABI AArch64 BE, e ao mecanismo
de avanço de tempo da máquina virtual.

## Bug 1: CACR_IE não setado — JIT em uncached mode

### Sprint 18

**Problema**: `exec_pc=00000000` sempre no btrace. Chipset nunca avançava.

**Root cause**: O bloco BELLATRIX em `M68K_StartEmu()` em `start.c` não setava
`__m68k.CACR`. Com `CACR=0`, `cacr & CACR_IE = 0` em `ExecutionLoop.c` linha 339
→ JIT rodava em **uncached mode** (slow path com `SaveContext + VerifyUnit(CRC32)`
por iteração).

**Descoberta crítica**: `EMU68_HOST_BIG_ENDIAN=1` em `emu68/include/config.h`.
Emu68 roda AArch64 em big-endian mode (SCTLR.EE=1, E0E=1). Portanto:
- `BE32(x) = x` (identity, definido em `support.h` linha 42)
- `BE32(CACR_IE) = CACR_IE = 0x00008000`

**Fix**: `__m68k.CACR = BE32(CACR_IE);` em `start.c` bloco BELLATRIX.

**Referências**:
- `emu68/include/config.h` linha 33: `#define EMU68_HOST_BIG_ENDIAN 1`
- `emu68/include/support.h` linha 42: `static inline uint32_t BE32(uint32_t x) { return x; }`
- `emu68/include/M68k.h` linha 155: `#define CACR_IE 0x00008000`
- `emu68/src/ExecutionLoop.c` linha 339: `if (likely(cacr & CACR_IE))`

---

## Bug 2: v30 Register Corruption — Sistema Travando Após CACR_IE

### Sprint 19

**Problema**: Após habilitar CACR_IE (Sprint 18), o sistema travava logo após alguns
writes de vetores. Nunca chegava a `[CIAA-W]` ou `[INTENA-W]`.

**Root cause**: `v30` (V30.d[0]) é o **JIT instruction counter**. `EMIT_LocalExit`
em `M68K_Translator.c` emite `vadd_2d(30,30,0)` a cada saída de bloco JIT.
O bloco BELLATRIX em ExecutionLoop lê `v30` para calcular `bela_delta` e chamar
`bellatrix_bridge_cpu_progress(bela_delta * 8u)`.

**`v30` é caller-saved** (V16–V31 per AArch64 ABI). Cada `kprintf` pode clobberá-lo.

`SYSWriteValToAddr` / `SYSReadValFromAddr` salvavam `x18` (M68K PC) ao redor do
bus hook, mas NÃO salvavam `v30`. Qualquer bus fault com kprintf (e.g., `[VEC-W]`,
`[CIAA-W]`) zeroa `v30`.

**Efeito catastrófico**:
```
bela_delta = (uint32_t)(v30_após_kprintf - bela_insn_prev)
           = (uint32_t)(0 - N_acumulado)
           ≈ 2^32 - N  (valor enorme!)
bellatrix_machine_advance(2^32 * 8) → agnus_step() por bilhões de ticks
→ CPU nunca retorna para executar M68K
```

**Fix**: Save/restore de `v30` ao redor de `bellatrix_bus_access` em `vectors.c`:
```c
uint64_t _x18_save, _v30_save;
asm volatile("mov %0, x18"      : "=r"(_x18_save));
asm volatile("mov %0, v30.d[0]" : "=r"(_v30_save));
g_bellatrix_fault_pc = (uint32_t)_x18_save;
bellatrix_bus_access(...);
asm volatile("mov v30.d[0], %0" :: "r"(_v30_save));
asm volatile("mov x18, %0"      :: "r"(_x18_save));
```

---

## Bug 3: `__m68k_state` vs `TPIDRRO_EL0` — IPL não chegando ao M68K

### Sprint 08

**Problema**: CIA alarm gerado (`icr_d=0x04, irq=1`) mas IPL nunca chegava ao M68K.

**Root cause**: `PAL_ChipsetTimer_Init()` capturava `s_m68k_ctx` lendo `TPIDRRO_EL0`
durante `bellatrix_init()`, que roda **antes** de `M68K_StartEmu()`. O
`TPIDRRO_EL0` só é setado dentro de `M68K_StartEmu` na linha 2251 de `start.c`.
Resultado: `s_m68k_ctx = NULL`. `PAL_IPL_Set()` via `if (!ctx) return` — saía sem
fazer nada.

**Fix**: Substituir `TPIDRRO_EL0` por `__m68k_state` (ponteiro global setado dentro
de `M68K_StartEmu` **antes** de `MainLoop()`, em `start.c` linha ~2109):
```c
// antes (pal_ipl.c)
extern struct M68KState *volatile s_m68k_ctx;
struct M68KState *ctx = s_m68k_ctx;

// depois
extern struct M68KState *__m68k_state;
struct M68KState *ctx = __m68k_state;
```

---

## Bug 4: Fast RAM — Incoerência de Memória (Sprint 19)

**Problema**: AROS jumpa para `0x002e0014` onde Emu68 ICACHE via `0xffff` e
Bellatrix Fast RAM via `0x0000` — inconsistência causando `opcode ffff not implemented`.

**Root cause**: `fast_ram.c` indexava Fast RAM usando o endereço M68K absoluto ao
invés do offset dentro do bloco Fast RAM.

**Fix**:
```c
// antes
return addr & m->fast_ram_mask;

// depois (fast_ram.c)
return (addr - BELLATRIX_FAST_RAM_BASE) & m->fast_ram_mask;
```

Bellatrix Fast RAM apontada para alias baixo do Emu68 (`0x00200000` físico) em vez
do alias alto (`0xffffff9000200000`). Fast RAM não mais trap-mapeada (AF=0 removido)
— RAM é de Emu68, Bellatrix apenas observa.

---

## Arquitetura: Live Path Correto

### Sprint 21 — Conclusão Definitiva

O live path real para Bellatrix+Emu68 é:

```
Emu68 JIT block exit
  → vadd_2d(30,30,0)        ← acumula v30
ExecutionLoop.c (bloco BELLATRIX):
  → bela_delta = v30 - prev
  → bellatrix_bridge_cpu_progress(bela_delta * 8u)
     → bellatrix_machine_advance(cycles)
        → agnus_step, cia_step, paula_step, ...

Bus fault (data abort):
  → vectors.c: SYSWriteValToAddr / SYSReadValFromAddr
  → [save x18 e v30]
  → bellatrix_bridge_cpu_access(addr, val, size, dir)
     → bellatrix_machine_write/read(...)
        → paula/agnus/cia dispatch
  → [restore x18 e v30]
  → eret
```

**`bellatrix_bus_access()` não é chamado no live path** — apenas o bridge é.
Código crítico (OVL, ROM) deve estar em `vectors.c` ou nas funções chamadas pelo bridge.

---

## Patch 0003: ExecutionLoop

O bloco BELLATRIX em `emu68/src/ExecutionLoop.c`:
```c
#ifdef BELLATRIX
uint64_t bela_insn_prev = 0;
// ... no loop:
uint64_t v30_now = ...;   // lê v30.d[0]
uint32_t bela_delta = (uint32_t)(v30_now - bela_insn_prev);
bela_insn_prev = v30_now;
if (bela_delta > 0)
    bellatrix_bridge_cpu_progress(bela_delta * 8u);
#endif
```

**Multiplicador `* 8u`**: calibração empírica — 1 instrução JIT ≈ 8 ciclos M68K.
Pode precisar ajuste fino se CIA drift ou serial pacing forem observados.

---

## Estado Atual

### O que Funciona
- CACR_IE ativado → JIT cached mode ativo
- v30 salvo/restaurado → chipset avança corretamente após bus faults
- `__m68k_state` → IPL chega ao JIT corretamente
- Fast RAM coerente entre Emu68 e Bellatrix
- ExecutionLoop avança a machine com ciclos derivados do JIT

### O que Precisa Atenção / Hipóteses Não Validadas
- **Multiplicador `* 8u`** — pode causar CIA drift ou serial pacing mismatch. Validação via observação de frame timing e CIA timer behavior em hardware real.
- **Reset fidelity** — bare-metal usa `bellatrix_reset_isp/pc` pré-computados; harness usa pulse_reset via bus. Diferença não crítica mas potencial fonte de divergência.
- **bela_delta overflow** — se v30 for clobberado por algo não coberto pelo save/restore atual (improvável mas possível com futuras mudanças em vectors.c).

## Arquivos Relevantes
- `emu68/src/aarch64/vectors.c` — save/restore v30+x18, OVL trigger
- `emu68/src/aarch64/start.c` — CACR_IE, `__m68k_state`, bellatrix_init call
- `emu68/src/ExecutionLoop.c` — bloco BELLATRIX (v30 → bela_delta)
- `src/bridge/bellatrix_bridge.h/.c` — camada de bridge CPU/machine
- `src/host/raspi3/pal_ipl.c` — usa `__m68k_state`
- `src/core/memory/fast_ram.c` — offset fix
- `patches/0002-add-bellatrix-bus-hook.patch` — v30+x18 save/restore, CACR_IE
- `patches/0003-bellatrix-execution-loop.patch` — bloco ExecutionLoop
