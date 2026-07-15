# Issue: Árbitro/Scheduler temporal no Core 0

> **SUPERSEDED novamente em 2026-07-15 por ISSUE-0058.** A rejeição abaixo a
> devolver o JIT ao Core 0 baseou-se no entendimento agora superado de que o
> Emu68 era um worker independente de sua topologia original de IRQ/startup.
> Core 0 + fault handler volta a ser o baseline conservador até prova contrária.

> **DOCUMENTO HISTÓRICO — SUPERSEDED em 2026-07-12.** O Core 0 não agenda nem
> limita periodicamente a CPU. A arquitetura canônica está na ISSUE-0051:
> Core 1 livre, Core 2 self-paced e sincronização apenas em contatos reais.

> Atualização 2026-07-11: topologia ativa = Core 0 supervisor + owner de I/O,
> Core 1 CPU, Core 2 Rigel e Core 3 reservado para RTG/AHI. Referências abaixo
> a Core 3 como owner de I/O descrevem o desenho anterior.

## Status: decisão de arquitetura + plano (aberto, 2026-07-10)

Preenche a lacuna referenciada há tempo por `[[issue_multicore_runtime]]`
("Scheduler por deadline") e nunca escrita. Define **quem** é o dono da timeline
canônica do Bellatrix multicore, **por que** ele resolve os gargalos atuais, e
**em que ordem** implementá-lo sem quebrar o boot.

Relacionado: `[[issue_multicore_runtime]]`, `[[emu68_public_api]]`,
`[[issue_emu68_pistorm_interrupt_contract]]`, `docs/runtime_and_timing.md`,
`docs/future_roadmap.md`.

## Decisão

**Árbitro no Core 0; JIT permanece no Core 1** (Opção A). Rejeitada a inversão
(JIT de volta ao Core 0 / árbitro no Core 1, Opção B).

### Por quê (A sobre B)

1. **É o papel implementado.** Core 0 nasce como BSP, inicializa a plataforma e
   agora continua como Host Reactor/supervisor. O futuro arbiter temporal evolui
   nesse plano de controle sem transferência de ownership.
2. **Zero handoff de posse.** Core 0 é o BSP: boota, inicializa, lança os
   secundários (`PAL_Core_LaunchCpu/Chipset/IO`). Virar árbitro é transição natural
   de "init" → "dono da timeline". A inversão passaria a posse-da-máquina de Core 0
   (boot) para Core 1 (runtime) — contra o fluxo do BSP.
3. **Não arrisca boot provado.** JIT no Core 1 funciona em hardware (Workbench,
   Happy Hand). Devolvê-lo ao Core 0 mexeria em `bellatrix_launch_cpu_and_park` /
   fluxo de lançamento — churn com risco de regressão.
4. **Isola o JIT das IRQs da plataforma.** Nunca quisemos device IRQ no core do
   JIT, pois isso perturba o ABI pinado x13–x29. O Host Reactor no Core 0 possui
   os devices e futuramente suas IRQs; Core 1 observa somente IPL emulado. Ver
   `[[issue_emu68_pistorm_interrupt_contract]]`.

### A consequência que resolve a tensão de fundo

Com o árbitro como **dono da máquina**, o Emu68 deixa de ser dono e vira um
**worker escalonável** dirigido por `run_until(deadline)`. Onde o JIT roda passa a
ser decisão de tuning, não conflito com as premissas PiStorm-cêntricas do Emu68.
Essa subordinação é a resolução arquitetural da tensão levantada em
`[[issue_emu68_pistorm_interrupt_contract]]`.

## Por que o árbitro é a solução (não um detalhe)

Os gargalos atuais são sintomas de **não haver dono da timeline**:

- **Lock por acesso** (`s_chipset_access_lock`, `src/runtime/core_chipset.c:49`)
  pego em *cada* MMIO (`src/cpu/cpu_bridge.c:170,196`). Como o Core 2 avança o
  chipset em blocos de `CHIPSET_QUANTUM=128` CCK sob esse guard, a CPU (Core 1)
  pode travar em `wfe` atrás de um quantum inteiro. Os dois cores que deveriam ser
  paralelos fazem **ping-pong**. O árbitro substitui isso por rendezvous de
  epoch/deadline: publica `deadline = min(quantum, próximo evento do Rigel)`, CPU e
  chipset correm a janela **em paralelo** e se encontram na fronteira. O lock por
  cutucada de registrador desaparece.
- **Quantum fixo de 128 CCK** é placeholder do scheduler. O árbitro o troca por
  `T = min(próximo evento observável)` — avança exatamente até o instante que
  importa, não em blocos cegos.
- **Pacing vs realtime** (incl. áudio) não tem dono hoje. É trabalho do árbitro:
  reconciliar relógio de parede, decidir "atrás/adiante", sequenciar completions
  por `earliest_visible_tick`. Ver `[[issue_paula_audio_timing]]`.

## Distribuição de cores (decidida)

| Core | Papel | Muda? |
|------|-------|-------|
| 0 | **Host Reactor / control plane** — supervisor, I/O físico, futuro arbiter temporal | implementado parcialmente |
| 1 | CPU — Emu68 JIT / Musashi, worker escalonado por `run_until` | placement inalterado |
| 2 | Rigel/chipset, worker executado até deadline | inalterado |
| 3 | **Acceleration plane reservado** — futuro RTG/AHI ou jobs pesados medidos | estacionado |

### Emenda de arquitetura (2026-07-11): IRQ física pertence ao Host Reactor

O roteamento atual de IRQ ARM ao Core 0 é herança do contrato PiStorm do Emu68,
não uma restrição nem uma decisão do Bellatrix. Quando o Emu68 estiver operando
como backend escalonável sem possuir as exceções da plataforma, o Bellatrix deve
programar explicitamente sua própria topologia.

Regra decidida: **IRQ física vai ao core que possui o periférico; evento emulado
passa pela fronteira da máquina**. Como Core 0 possui DWC2/USB, UART e BT, suas
IRQs futuras terminam nele, marcam o pending bitmap e retornam; stacks continuam
fora do handler. Core 3 não recebe IRQ apenas por ser um worker disponível.

São conceitos independentes: afinidade da exceção ARM, ownership do driver,
wakeup inter-core e IPL observado pela CPU 68k. Core 0 normaliza eventos e
atribui ordem/timestamp; Core 2 injeta no Rigel; Core 1 observa apenas o IPL
emulado. Tracker de execução: `ISSUE-0045`.

## Pré-requisito não-negociável

O árbitro só computa deadline real se **duas** APIs existirem; nenhuma existe hoje:

1. **`emu68_run_until(deadline)` com saída cooperativa**, incl. `EXIT_MMIO` (CPU sai
   da janela ao tocar registrador crítico; árbitro adianta o chipset até o tick,
   aplica, retoma). Hoje há só o embrião `emu68_run_cycles()` — "janela cooperativa
   sobre `MainLoop()`", step mínimo de 8 ciclos, sem deadline preciso nem exit por
   MMIO (`[[emu68_public_api]]`).
2. **`rigel_next_event_tick()`** — o chipset precisa expor quando é seu próximo
   evento (copper/blitter/beam/CIA), senão o `min()` do deadline não tem segundo
   operando e recai no quantum fixo.

**Árbitro e contrato embutível do Emu68 são o mesmo projeto**, vistos de dois
ângulos. Um não funciona sem o outro.

## Plano incremental (cada fase compila, boota e é medível)

**Fase 0 — Baseline medido.** Rodar com profile (`g_bprof.bridge_ref_read/write`,
`chipset_step_time`, "critical mmio backlog" do commit `6868ce9`). Quantificar:
(a) % de tempo em fault+lock de MMIO; (b) stall de contenção Core1↔Core2; (c) ratio
de produção de PCM vs realtime. Decide se o barramento ou a velocidade de emulação
domina — **antes** de reescrever.

**Fase 1 — Scaffold do árbitro no Core 0 (sem mudança funcional).** Core 0 para de
estacionar e roda um loop que possui `machine_state`, readiness, hooks de pacing e
métricas — mas o publish CPU→chipset atual permanece como está. Prova que Core 0
sustenta um loop sem quebrar boot. Não criar estrutura especulativa desconectada
(lição do `[[issue_multicore_boundary_logging]]`: `RuntimeEventQueue`/`mailbox` viraram
dead code) — o loop já tem de fazer trabalho real (métricas + pacing).

**Fase 2 — `rigel_next_event_tick()`.** Chipset expõe próximo evento; árbitro passa
a limitar o avanço por `min(quantum, próximo evento)`. Ainda sob o modelo atual de
publish, mas com deadline real.

**Fase 3 — `emu68_run_until(deadline)` + exit cooperativo (incl. `EXIT_MMIO`).**
Evolui `emu68_run_cycles` para janela dirigida por deadline com saída limpa. É a
parte difícil e o prêmio: CPU roda uma janela inteira sem lock por acesso.

**Fase 4 — Rendezvous substitui o lock por acesso.** Com CPU em janelas e chipset
em deadlines, trocar o ping-pong de `s_chipset_access_lock` por rendezvous de epoch
+ resolução de MMIO na fronteira. **Medir o ganho contra a Fase 0.**

**Fase 5 — Pacing/realtime + completions.** Árbitro reconcilia relógio de parede,
hospeda a política de áudio realtime, sequencia completions por
`earliest_visible_tick`.

**Fase 6 (futuro, guiado por medição) — device IRQ para I/O físico.** A IRQ
termina no core que possui o periférico: para DWC2/USB, Bluetooth e UART, Core 0.
O handler reconhece a fonte, marca o pending bitmap e retorna; o Host Reactor
executa o stack fora da exceção. Core 3 permanece worker de computação e não
recebe IRQ física apenas por estar disponível.

## Progresso (2026-07-10)

**Fase 1 — supervisor no Core 0: aterrissado.** `bellatrix_core0_supervise()` em
`src/cpu/emu68/bellatrix.c` substitui o `wfe` de estacionamento por um heartbeat
que loga `cpu_target`/`chipset`/`backlog`. No primeiro run (multicore Musashi +
KS13 no QEMU) ele revelou o defeito central: **backlog CPU↔chipset crescia sem
limite** — de 960K para 452.000.000 CCK e subindo, com a CPU 5–20× mais rápida
que o Rigel. Divergência de ~2 min de tempo emulado = por que "o multicore nunca
funcionou direito".

**Contrapressão CCK: aterrissada.** `CHIPSET_MAX_BACKLOG_CCK=8192` em
`src/runtime/core_chipset.c`: `bellatrix_runtime_publish_cpu_cycles` bloqueia
(WFE) quando `cpu_target - chipset_cck` passa do teto; Core 2 faz `sev` ao fim de
cada `host_step`. `s_chipset_cck` virou `_Atomic` (lido cross-core). Resultado
medido: backlog mediana ~0, p90 ~8246, com CPU e chipset em lock-step (deltas
idênticos). O crescimento ilimitado sumiu.

Pendências desta fase: (a) pico isolado de backlog (~753K) de um publish de
quantum grande — pode ser aparado limitando a granularidade do publish;
(b) `kprintf` não é serializado entre cores → log garble (precisa de UART lock);
(c) teto 8192 é chute inicial, ainda não afinado por comportamento de boot.

## Condições de parada

Parar e corrigir a fase atual se houver: regressão de boot, deadlock no rendezvous,
perda de evento/completion, ou piora medida (não suposta) de performance vs Fase 0.

## Arquivos relevantes (ponto de partida)

- `src/runtime/core_chipset.c` — quantum fixo + lock a substituir
- `src/cpu/cpu_bridge.c` — lock por acesso a MMIO
- `src/host/raspi3/pal_core.c` — boot/park do Core 0 (`bellatrix_core1/2/3_entry`)
- `src/cpu/emu68/bellatrix.c` — `bellatrix_launch_cpu_and_park`
- `src/cpu/emu68/emu68_api.*` — onde `run_until` evolui
- `external/rigel` — onde `rigel_next_event_tick()` nasce
