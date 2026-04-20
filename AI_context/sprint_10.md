// AI_context/sprint_10.md

# Sprint 10 — Timing único, pipeline VBL/Denise, silêncio do UART

## Status: concluído (build verde, aguarda flash e validação na placa)

---

## Decisão de arquitetura: single-core

Optamos por rodar tudo em um único core por ora. Multicore fica para fase futura.

**Mudança**: `pal_core.c` → `multicore_enabled = 0` (era `1`).

Consequência: `PAL_ChipsetTimer_Init(50, NULL)` não tem mais utilidade
(o timer de 50 Hz era o gatilho do core chipset). Substituído por
`PAL_Runtime_Init()` em `bellatrix_init()`.

---

## Timing wall-clock → machine_step

**Problema anterior**: `machine_step` nunca era chamado com ticks reais
porque o único caller era o Core 1 (desabilitado). VBL nunca disparava.

**Fix**: override forte de `bellatrix_runtime_host_step` em `bellatrix.c`.
`PAL_Runtime_Poll()` lê o ARM generic timer (CNTPCT), calcula delta em cycles
M68K (PAL: 7.09 MHz) e chama `machine_step()`:

```c
#define BELLATRIX_M68K_CLOCK_HZ 7093790ULL

void bellatrix_runtime_host_step(uint64_t host_now, uint64_t host_freq)
{
    static uint64_t s_last = 0;
    if (!s_last) { s_last = host_now; return; }
    uint64_t delta = host_now - s_last;
    s_last = host_now;
    if (host_freq && delta > host_freq) delta = host_freq;   // cap em 1s (pausa debugger)
    if (!host_freq) return;
    uint64_t m68k_cycles = (delta * BELLATRIX_M68K_CLOCK_HZ) / host_freq;
    if (m68k_cycles) machine_step(bellatrix_machine_get(), m68k_cycles);
}
```

`PAL_Runtime_Poll()` é chamado no **topo de cada `bellatrix_bus_access()`**,
ou seja, a cada bus fault do M68K. O timing avança a cada acesso, não por timer.

---

## Pipeline VBL → Copper → Denise

`agnus_step()` acumula cycles e detecta frames completos (313 linhas × 454 cycles):

```
agnus_step(ticks)
  → blitter_step()
  → beam progression (hpos/vpos)
  → por cada frame completo:
      agnus_intreq_set(INT_VERTB)        ← IPL 3 para o M68K
      copper_vbl_execute(&copper, agnus) ← roda copper list completa
      denise_render_frame(agnus)         ← rasteriza para framebuffer VC4
```

**Copper batch**: `copper_vbl_execute` percorre toda a copper list uma vez
por frame. MOVE instructions chamam `agnus_write_reg()` que roteia para
AgnusState (BPLxPT, DIW, DDF) ou `denise_write()` (BPLCON, COLOR).

---

## Separação de estado Agnus / Denise

| Dono    | Registradores                                      |
|---------|----------------------------------------------------|
| Agnus   | BPL1-6PTH/L, DIWSTRT, DIWSTOP, DDFSTRT, DDFSTOP  |
| Denise  | BPLCON0/1/2, BPL1MOD, BPL2MOD, COLOR[32]          |

`denise_render_frame(const AgnusState *agnus)` lê os ponteiros e janela
de Agnus em tempo de render; não duplica estado.

---

## Framebuffer: extern de globals do Emu68

O Emu68 já aloca o framebuffer via mailbox VC4 **antes** de `bellatrix_init()`.
Basta externar:

```c
extern uint16_t *framebuffer;
extern uint32_t  pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
```

Sem nenhuma alocação própria. `denise_render_frame` escreve direto.
Paleta pré-convertida para LE16 RGB565 em `denise_write(COLOR_xx)`.

---

## Fix UART flood → sistema travado

**Problema**: kprintfs de alta frequência saturavam o UART (115200 baud)
e bloqueavam o sistema inteiro esperando a serial drenar.

**Culpados e fix**:

| Local                     | Frequência (antes)        | Fix                        |
|---------------------------|---------------------------|----------------------------|
| `cia_tod_increment`       | milhares/s (cada tick TOD)| kprintf removido           |
| `cia_raise_icr`           | dezenas/s (timer underflow)| kprintf removido          |
| `agnus_compute_ipl`       | toda mudança INTENA/INTREQ| kprintf removido           |
| `agnus_notify_ipl`        | idem                      | kprintf removido           |
| `agnus_intreq_set/clear`  | toda mudança              | kprintf removido           |
| INTENA/INTREQ/DMACON write| todo acesso de registrador| kprintf removido           |
| CIA TOD write (LO/MID/HI) | boot + alarme             | kprintf removido           |
| AGNUS unhandled read/write| todo reg desconhecido     | kprintf removido (btrace já loga) |

**Mantidos** (frequência aceitável):
- `[VBL] frame=N ...` — uma vez por frame (~50 Hz)
- `[CIA] ICR read/write` — acknowlege de IRQ, raro
- `[CIA] CRA/CRB write` — init do KS, poucas vezes
- `[BELA]` init logs — uma vez no boot

---

## Observação do Gemini (consulta externa, sprint anterior)

O Gemini levantou um ponto válido: antes de depurar render de bitplanes,
fazer o **"Pau de Cego"** — pintar o framebuffer inteiro com uma cor sólida
dentro de `bellatrix_init()` para confirmar que o framebuffer está vivo e
o VC4 está mostrando o que escrevemos:

```c
// Diagnóstico: pinta framebuffer de vermelho se disponível
if (framebuffer && pitch && fb_width && fb_height) {
    for (uint32_t y = 0; y < fb_height; y++) {
        uint16_t *row = (uint16_t *)((uintptr_t)framebuffer + y * pitch);
        for (uint32_t x = 0; x < fb_width; x++)
            row[x] = 0x00F8;  // vermelho em LE16 RGB565
    }
}
```

**Não implementado ainda** — deixar para a próxima sessão se a tela
continuar sem sinal após o fix do UART.

O Genesis sugeriu dispatch table para o bus router. Válido como otimização
futura, mas não urgente — o switch atual é suficiente para a fase corrente.

---

## Arquivos alterados neste sprint

- `src/host/raspi3/pal_core.c` — `multicore_enabled = 0`
- `src/host/pal.h` — declarações de `PAL_Runtime_*`, `PAL_Time_*`, `PAL_Core_*`
- `src/cpu/bellatrix.c` — strong `bellatrix_runtime_host_step`, `PAL_Runtime_Poll()` no topo do bus handler, `PAL_Runtime_Init()` em init, `denise_init()` adicionado
- `src/chipset/agnus/agnus.h` — `AGNUS_PAL_HPOS=454`, DIW/DDF/BPLxPT defines e campos no struct
- `src/chipset/agnus/agnus.c` — pipeline VBL completo, remoção de todos kprintfs de alta frequência
- `src/chipset/denise/denise.h` — redesenhado: forward decl AgnusState, offsets decodificados, API limpa
- `src/chipset/denise/denise.c` — render completo: bitplanes, palette LE16, 2× scale, centralizado
- `src/chipset/cia/cia.c` — remoção de kprintfs de alta frequência (TOD, ICR raise, TOD write)

Build: verde (`[100%] Built target Emu68.elf`).

---

## Próxima sessão

1. Flash e boot real
2. Verificar `[VBL] frame=1` no serial — confirma timing e pipeline vivos
3. Se tela em branco: implementar "Pau de Cego" (cor sólida em init) para
   isolar se o problema é no framebuffer ou no render de bitplanes
4. Se VBL não aparecer: `machine_step` não está recebendo cycles suficientes
   — investigar se `PAL_Runtime_Poll` está sendo chamado (verificar via
   `[BELA]` contador de bus faults no btrace)
5. Paula stubs (ADKCON, POTGO, SERPER) — não bloqueantes mas geram ruído
   no btrace como "unimplemented"
