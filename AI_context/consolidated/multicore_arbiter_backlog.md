# Multicore + Arbiter — backlog e separação de escopo

## Status: ativo (2026-07-10)

Persiste o backlog de trabalho iniciado na sessão de 2026-07-10 (antes só num
tracker de sessão, volátil). Separa o que é **específico do multicore** (branch
`wip/multicore-runtime`) do que é **específico do Emu68/JIT** (pertence à branch de
AROS-bootando-com-Emu68, onde as APIs públicas serão mergeadas). A fronteira é
tênue, mas útil: o JIT trava num bug JIT+FPU+Z3 comum a single e multicore, então
o trabalho de multicore avança com o backend **Musashi** (que roda estável).

Docs de detalhe: `[[issue_core0_arbiter_scheduler]]`,
`[[issue_emu68_pistorm_interrupt_contract]]`, `[[issue_multicore_runtime]]`,
`[[emu68_public_api]]`.

## Multicore-específico (branch wip/multicore-runtime)

| # | Item | Estado |
|---|---|---|
| 1 | **Contrapressão Core1↔Core2** (teto de backlog CCK) | ✅ feito e validado (Musashi): divergência ilimitada 452M → limitada ~0 |
| — | Validar boot end-to-end **multicore Musashi + KS13 + wb13.adf** (Workbench chega?) | em andamento |
| 4 | `rigel_next_event_tick()` — chipset expõe próximo evento | pendente (testável em Musashi) |
| 6 | **Arbiter por deadline** — epoch/rendezvous troca quantum fixo + lock por acesso | pendente (bloqueado por #4 e, pro caminho JIT, por #2) |
| — | Follow-ups da Fase 1: pico isolado de backlog (~753K); **kprintf sem lock entre cores** (log garble); afinar teto 8192 | pendente |

Aterrissado nesta branch (working tree, não commitado):
- `src/launcher/launcher.c` — `wait_ack` 400× menor (loop QEMU rápido)
- `src/cpu/emu68/bellatrix.c` — supervisor no Core 0 (`[CORE0-SUP]`) + log `Core0=Supervisor...`
- `src/runtime/core_chipset.c` — contrapressão (`s_chipset_cck` atômico, `CHIPSET_MAX_BACKLOG_CCK=8192`, SEV do Core 2)

## Emu68/JIT-específico (branch AROS-on-Emu68, não esta)

| # | Item | Estado |
|---|---|---|
| 5 | Verificar se o build Emu68/JIT sobe ROM | ✅ feito: **não sobe** |
| 7 | **Crash JIT: FMOVE FPU → 0xFFFFFFF6 (Z3)** | pendente — bug raiz do JIT |
| 2 | Emu68 `run_until` + saída cooperativa (API faltante) | pendente — prereq do arbiter no caminho JIT |
| 3 | Monopólio de IRQ do Emu68 / device IRQ áudio | pendente — Fase 6, guiado por medição |

### #7 detalhado (o bloqueador do JIT)
Emu68/JIT + KS13 + wb13.adf trava numa `FMOVE from SPECIAL` no M68K PC=0xFC1682
(ROM KS13, 68000) sob config 68040+FPU. **Não é multicore-específico** (bisect):
- multicore: data abort logado, `write to 0xFFFFFFF6`, ESR=0x96000046, CPU congela;
- single-core: trava **silenciosa** no mesmo FMOVE (sem fault logado).
Duas camadas: (a) alvo é Zorro III/32-bit >0xFFFFFF que o bus não expõe (ISSUE-0032);
(b) a FMOVE estendida (12B, emitida como store NEON de 16B) não é servível pelo
fault path de 1/2/4 bytes → "Unhandled". Direções: rotear/ignorar store estendido
não-mapeado como open-bus; ou expor Z3; ou investigar o EA selvagem.
Relacionado: ISSUE-0032, ISSUE-0034/0035, `bellatrix-musashi-fpu`.

## Decisão atual (2026-07-10)
Seguir **multicore com Musashi** (opção conservadora). #7 e a superfície JIT ficam
para a branch de AROS/Emu68. Ao mergear aquela branch, reconciliar com a API
pública (`[[emu68_public_api]]`) e reavaliar #2.
