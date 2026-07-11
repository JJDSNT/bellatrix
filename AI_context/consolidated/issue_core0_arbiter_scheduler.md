# Issue: Árbitro/Scheduler temporal no Core 0

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

1. **Já é o papel reservado.** `docs/future_roadmap.md:445` nomeia Core 0 como
   "Machine/Host — arbiter"; `docs/runtime_and_timing.md:189` diz que ele hoje só
   "parks in a light wfe loop afterward — no recurring work of its own". O core do
   árbitro existe e está **desperdiçado dormindo**.
2. **Zero handoff de posse.** Core 0 é o BSP: boota, inicializa, lança os
   secundários (`PAL_Core_LaunchCpu/Chipset/IO`). Virar árbitro é transição natural
   de "init" → "dono da timeline". A inversão passaria a posse-da-máquina de Core 0
   (boot) para Core 1 (runtime) — contra o fluxo do BSP.
3. **Não arrisca boot provado.** JIT no Core 1 funciona em hardware (Workbench,
   Happy Hand). Devolvê-lo ao Core 0 mexeria em `bellatrix_launch_cpu_and_park` /
   fluxo de lançamento — churn com risco de regressão.
4. **Isola o JIT das IRQs da plataforma.** Nunca quisemos device IRQ no core do
   JIT, pois isso perturba o ABI pinado x13–x29. O árbitro no Core 0 recebe
   eventos normalizados e os ordena na timeline, enquanto o Core 3 possui os
   devices e é o destino preferencial das IRQs físicas. Ver a emenda abaixo e
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
| 0 | **Árbitro / dono da timeline** — epochs, deadlines, pacing e ordenação de completions/eventos externos | sim: deixa de estacionar |
| 1 | CPU — Emu68 JIT / Musashi, worker escalonado por `run_until` | placement inalterado |
| 2 | Rigel/chipset, worker executado até deadline | inalterado |
| 3 | **Owner do I/O físico** — USB/DWC2, BT/UART, HDMI/DMA, IRQs físicas e completions | evolui de polling para serviço orientado a eventos/IRQ |

### Emenda de arquitetura (2026-07-10): afinidade de IRQ não pertence ao árbitro

O roteamento atual de IRQ ARM ao Core 0 é herança do contrato PiStorm do Emu68,
não uma restrição nem uma decisão do Bellatrix. Quando o Emu68 estiver operando
como backend escalonável sem possuir as exceções da plataforma, o Bellatrix deve
programar explicitamente sua própria topologia.

Regra decidida: **IRQ física vai, preferencialmente, ao core que possui o
periférico; evento emulado passa pelo árbitro**. Portanto USB/DWC2, UART/BT e
HDMI/DMA devem ter afinidade com o Core 3 quando o BCM2837 permitir. O Core 0
recebe apenas seu timer de scheduling, sinais de supervisão e notificações
inter-core coalescidas. Se uma fonte não puder ser roteada ao Core 3, o Core 0
pode atuar como gateway técnico mínimo (ack/mask + pending bit + wakeup), sem
executar a stack ou adquirir ownership do device.

São conceitos independentes: afinidade da exceção ARM, ownership do driver,
wakeup inter-core e IPL observado pela CPU 68k. O Core 3 normaliza eventos e
publica filas/completions para o Core 0; o árbitro atribui ordem/timestamp e
decide a fronteira emulada segura; Core 2 injeta no Rigel; Core 1 observa apenas
o IPL emulado. Tracker de execução: `ISSUE-0045`.

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

**Fase 6 (futuro, guiado por medição) — device IRQ para I/O físico** com owner no
Core 3, se a Fase 0/5 mostrar que jitter de polling importa *depois* de a
velocidade estar resolvida. Core 0 recebe somente notificações/completions já
normalizadas, salvo fallback inevitável de roteamento. Não antes.

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
