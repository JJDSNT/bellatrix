// AI_context/sprint_07.md

# Sprint 07 — Copper + btrace noise reduction

## Status: concluído

---

## Contexto

`denise_render_frame()` já estava completo (Phase 4). O que faltava para
produzir imagem era o Copper: sem ele o KS não escreve COLOR00-COLOR31 nem
os ponteiros de bitplane (BPL1PT…) por frame, então `denise_render_frame()`
operava com paleta e ponteiros zerados.

---

## Issue 1 — btrace init com BTRACE_ALL em vez de BTRACE_UNIMPL

`src/core/btrace.c` → `btrace_init()`

O comentário na linha 19 dizia "default: log unimplemented only" mas o corpo
de `btrace_init()` sobrescrevia com `BTRACE_ALL`, causando log massivo de
todos os acessos CIA e chipset implementados.

**Fix:** `s_filter = BTRACE_ALL` → `s_filter = BTRACE_UNIMPL`.

---

## Issue 2 — [BUS#N] kprintf a cada 100 chamadas

`src/cpu/bellatrix.c` → `bellatrix_bus_access()`

O bloco de diagnóstico `s_bus_call_count` imprimia nas primeiras 5 chamadas
e depois a cada 100 — gerando ruído contínuo no UART. Útil durante bring-up
inicial (Phase 0), obsoleto agora.

**Fix:** bloco removido inteiramente. Mensagem de init atualizada de
"Phase 3 ready" → "Phase 4 ready".

---

## Issue 3 — Copper não implementado

**Novos arquivos:**
- `src/chipset/agnus/copper.h`
- `src/chipset/agnus/copper.c`

**Modelo Phase 4 — batch VBL:**

`copper_vbl_execute()` é chamado em `agnus_vbl_fire()` **antes** de
`denise_render_frame()`. A cada VBL:

1. PC recarregado de `COP1LC` (comportamento Amiga padrão).
2. Instrução MOVE: despacha `agnus_write(0xDFF000 | (ir1 & 0x01FE), ir2, 2)`.
   - Rotas via `agnus_write` → Denise (COLOR00-31, BPLCON0, BPLxPTH/L) ou
     Agnus (DMACON, etc.).
3. Instrução WAIT: transparente — a execução continua (sem simulação de beam).
   - Exceção: sentinel `ir1=$FFFF, ir2=$FFFE` → para execução (end of list).
4. Instrução SKIP: ignorada.
5. Guard limit de 8192 instruções contra listas corrompidas.
6. Proteção DANGER: MOVEs para registradores `< $DFF040` são descartados
   (COPCON CDANG=0 é o default do KS).

**Registradores implementados:**
- `COP1LCH/L` ($DFF080/$DFF082) — pointer lista 1
- `COP2LCH/L` ($DFF084/$DFF086) — pointer lista 2
- `COPJMP1/2` ($DFF088/$DFF08A) — strobes (sem estado persistente)
- `COPINS`    ($DFF08C) — CPU direct write (sem estado persistente)

**Integração:**
- `agnus_write()` agora despacha os 7 registradores copper para `copper_write()`.
- `agnus_init()` chama `copper_init()`.
- `agnus_vbl_fire()`: ordem agora é `copper_vbl_execute()` → `denise_render_frame()`.
- `emu68/CMakeLists.txt`: `copper.c` adicionado à lista de fontes.

Build verde confirmado.

---

## Limitações conhecidas (para sprints futuros)

1. **WAITs ignorados** — efeitos de copper mid-scanline (raster bars, split
   screens, per-line palette) não funcionam. Para o KS boot screen (paleta
   uniforme + bitplanes estáticos) não é problema.
2. **COP2LC nunca executada** — alguns copper lists usam lista 2 (ex: sprites).
   Por ora só COP1LC é executada no VBL.
3. **COPJMP1/2 durante execução** — um MOVE para COPJMP1/2 dentro da própria
   copper list (para jumps explícitos) não é tratado. Seria necessário
   detectar writes para $DFF088/$DFF08A dentro do loop e redirecionar o PC.
4. **Sprites** — não implementados. Amiga usa sprites para o cursor animado
   ("Happy Hand"). Próxima grande milestone de Phase 5.

---

## Próxima sessão

1. Flash na placa e capturar btrace com o novo código.
2. Verificar no log se `COP1LCH/L` está sendo escrito pelo KS (indica que
   a copper list foi carregada na chip RAM).
3. Confirmar que `COLOR00` muda de valor entre frames (indica copper executando).
4. Se imagem aparecer mas cursor não → implementar sprites (Phase 5).
5. Se tela preta → checar se DMACON[COPEN] está set antes do VBL, e se
   `s_cop1lc != 0` quando `copper_vbl_execute()` roda (adicionar kprintf
   temporário se necessário).
