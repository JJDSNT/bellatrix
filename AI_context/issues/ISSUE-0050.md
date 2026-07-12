---
id: ISSUE-0050
title: "Execution tracker for ISSUE-0049"
status: doing
priority: high
type: task
owner: agent
created_at: 2026-07-12
updated_at: 2026-07-12
parent: ISSUE-0049
tags: [pistorm, multicore, execution]
---

# Objetivo operacional

Executar integralmente a ISSUE-0049. A especificação, fases, métricas e gates
permanecem na issue-mãe; este arquivo registra ordem corrente, evidências e
bloqueios sem criar um escopo paralelo.

# Estado global

| Fase | Estado | Gate |
|---|---|---|
| 0 — baseline/observabilidade | em andamento | perfil reproduzível |
| 1 — beam f(t) | em andamento | boot + queda de waits |
| 2 — protocolo de contatos | pendente | ordenação explícita |
| 3 — event stream | em andamento | menos empty steps no Pi 3 |
| 4 — epochs/Core 0 | em andamento | horizons corretos |
| 5 — Core 2 self-paced | em andamento | avanço realtime autônomo |
| 6 — semântica CPU | em andamento | só contatos sincronizam |
| 7 — frames/áudio | pendente | realtime sustentado |
| 8 — promoção/limpeza | pendente | matriz final aprovada |

# Fila de execução

- [x] Completar API pública e integração inicial do beam.
- [x] Ampliar equivalência beam projetado/real.
- [x] Instrumentar fast path/fallback VPOSR/VHPOSR.
- [x] Rodar testes e build multicore profile.
- [ ] Executar boot/perfil QEMU e registrar A/B.
- [ ] Executar gate equivalente no Pi 3.
- [ ] Continuar pela primeira pendência da fase seguinte na ISSUE-0049.
- [x] Iniciar matriz de contatos MMIO e corrigir lacunas temporais/triggers.
- [x] Implementar event stream PAL no Core 2 com fallback de plataforma.
- [x] Implementar timeline CPU/realtime/híbrida e horizon atômico do Core 0.
- [x] Fazer Core 2 consumir horizon e adaptar MMIO ao agora do chipset.
- [x] Mover frame presentation e HDMI DMA poll do Core 2 para o Core 0.
- [x] Converter política MMIO em tabela pura com teste unitário.
- [x] Limitar bursts do Core 2 e rebasear gaps de wall clock >250 ms.
- [x] Encaminhar pause/resume e troca de modo ao owner Core 0 por pedidos
  atômicos, sempre com rebase no contador corrente.
- [x] Extrair/testar a fila SPSC e fechar liveness quando pause coincide com
  fila cheia.
- [x] Audit cross-core do caminho Core 0 (presenter, input, serial, áudio).
- [x] Stress SPSC com pthreads reais sob TSAN no ctest.
- [ ] Experimento de granularidade: drain sem cortes em deadlines observáveis
  (avg_step 79 → ~2000+) e medir fps no Pi antes de aceitar "custo por CCK".
- [x] A/B formal Fase 0 no QEMU: 3 rodadas × {single, multicore-cpu,
  multicore-hybrid} com PROFILE; tabela de contadores + variância na issue.
- [x] Reavaliar CHIPSET_PUBLISH_MIN_CCK 1→227 com o A/B montado (beam f(t)
  inverteu a conta que fazia disso uma regressão) — ADOTADO 227.
- [ ] Política de presenter: apresentar só o último frame pendente quando
  atrasado (hoje `while (frames--)` apresenta todos em sequência).
- [ ] Pi: capturar `[BOOT] ARM Clock` desde o início e ler `[CORE0-HW]`
  durante o run antes de qualquer conclusão de performance.

# Evidências

- 2026-07-12: 26/26 testes Rigel passaram.
- 2026-07-12: build Musashi 68040 multicore passou.
- 2026-07-12: equivalência beam projetado/real passou em PAL e NTSC para
  posições sucessivas, wraps e avanço maior que um frame; 26/26 testes verdes.
- 2026-07-12: build Musashi 68040 multicore com BPROF passou.
- 2026-07-12: smokes AROS multicore de 18-75 s chegaram no máximo a 148 frames,
  com progresso monotônico e backlog limitado. Servem apenas como sanidade de
  bootstrap: AROS só inicia atividade relevante por volta dos frames 1500-2000,
  portanto os contadores de beam zerados nesses smokes não avaliam o fast path.
  O smoke também mostrou que o autodump BPROF era Emu68-only; heartbeat
  profile-only adicionado para leitura dos contadores no Musashi.
- 2026-07-12: política MMIO documentada; DSKDATR/SERDATR passaram a leituras
  temporais e DSKLEN/BLTSIZV/BLTSIZH a triggers críticos.
- 2026-07-12: lifecycle inicial do snapshot fechado: publicação no init/reset
  e invalidação no shutdown, evitando geometria de uma geração anterior.
- 2026-07-12: BPROF ganhou fila postada queued/applied/full-waits/depth-max para
  tornar pressão e liveness observáveis antes do self-paced.
- 2026-07-12: execução AROS multicore profile atravessou o gate de atividade e
  foi encerrada em 2329 frames: VPOSR fast=218.407, VHPOSR fast=5.460.179,
  fallback=0, snapshot_miss=0, progresso monotônico e backlog limitado.
- 2026-07-12: event stream arquitetural (`CNTKCTL_EL1`) implementado no Core 2
  em ~250 kHz; progresso ordinário deixa de emitir SEV por bloco quando ativo,
  enquanto contatos/backpressure preservam wakeups cirúrgicos.
- 2026-07-12: QEMU confirmou stream ativo a 244.140 Hz, boot/progresso e fila
  postada drenada (12 queued/applied, depth max 4, zero full wait). Heartbeat
  ganhou contadores de scheduler para o A/B de empty steps.
- 2026-07-12: CPU-driven com horizons passou no QEMU; `empty_host_steps`
  estabilizou em 9.079 (ordem anterior registrada: ~10 milhões).
- 2026-07-12: módulo de timeline passou teste host de CPU/realtime/híbrido,
  clamp, pause/rebase e delta longo. QEMU híbrido confirmou mode=2 e horizon
  limitado por CPU+8192; TCG não acompanha realtime, logo promoção depende Pi 3.
- 2026-07-12: Emu68 multicore híbrido compilou e bootou AROS no QEMU. Métricas
  mostraram realtime target, clamp ativando quando CPU ficou atrás, horizon,
  Rigel drift, geração, event stream e fila drenada sem full wait.
- 2026-07-12: Musashi 68000 híbrido também compilou e progrediu no QEMU;
  AROS executou seu probe FPU e caiu no F-line esperado para CPU_TYPE=68000.
- 2026-07-12: `PAL_ChipsetTimer_Stop/Start` passou a publicar pause/resume para
  o Core 0, sem escrita cross-core no estado da timeline; troca de modo ganhou
  o mesmo protocolo. O teste host verifica que tempo parado não vira burst de
  recuperação. Builds Emu68 e Musashi passaram, assim como 26/26 testes Rigel.

- 2026-07-12: fila SPSC extraída para `runtime/posted_writes.[ch]` (pura,
  host-testável) com `try_push` NÃO-bloqueante — a política de espera fica no
  call site (`core_chipset.c`), onde `running`/pausa são visíveis, e o
  produtor sempre pode cair no caminho síncrono: liveness de pausa × fila
  cheia fechada por construção e coberta por teste (ordem, wrap, stamp-limit,
  fila cheia, reset).
- 2026-07-12: `test_timeline`, `test_mmio_policy` e `test_posted_writes`
  plugados no ctest do harness (`bellatrix_unit_*`); antes rodavam ad hoc.
- 2026-07-12: o teste de timeline expôs uma decisão de spec pendente da Fase
  6 (comportamento na liberação do clamp): as expectativas dos dois episódios
  eram mutuamente inconsistentes. Definido e implementado: deficit de parede
  acumulado sob clamp é REPLAYED até 250 ms (máquina honesta ao realtime) e
  REBASED acima disso (CPU efetivamente estagnada; fast-forward seria o burst
  que o clamp existe para impedir) — mesma filosofia do limiar de gap de
  host. Teste ajustado e ampliado para cobrir a liberação curta (replay) e a
  longa (rebase).
- 2026-07-12: suíte completa verde: 37/37 no ctest do harness (26 Rigel,
  incluindo test_beam, + 4 unit + integrações).

- 2026-07-12: a migração de frame/HDMI para o Core 0 tornou a fila de áudio
  de `output.c` cross-core (produtor `audio_output_tick` no Core 2, consumidor
  `hdmi_audio_dma_poll` no Core 0), mas head/tail/count eram uint32 puros e
  `head` era escrito pelos DOIS lados (drop-oldest do produtor + pop do
  consumidor) — corrupção real em ARM. Refeita como SPSC canônico: cursores
  atômicos free-running com máscara (QUEUE_SIZE=2^14), produtor dono do tail,
  consumidor dono do head, drop-newest quando cheia (produtor não toca o
  cursor do consumidor), depth derivado, primed atômico. A produção de áudio
  permanece no Core 2 por design (é função do tempo emulado e do estado da
  Paula); apenas o lado host consome no Core 0.

- 2026-07-13: PRIMEIRO GATE NO PI 3 REAL (Musashi 68040 multicore, hybrid,
  KS1.3): 5-8 fps. Protocolo saudável (fila postada full_waits=0 depth<=9,
  IO Core 0 a 7us, event stream ativo), mas dois furos expostos e corrigidos
  (52d2fa2): (1) cpu_target chegou ao init da timeline inflado em 4,3e9 CCK
  de free-run pré-runtime -> clamp do hybrid morto e drift sem limite; agora
  o init rebaseia e loga o delta descartado (origem do free-run ainda a
  caçar — provável fase launcher/ISSUE-0045). (2) nada limitava o horizon
  quando o CHIPSET é o lado lento — que é o regime real do Pi; horizon agora
  segue min(chipset,cpu)+backlog com a regra de liberação de 250 ms.
- 2026-07-13: CONCLUSÃO ESTRUTURAL do Pi: Core 2 sustenta ~0,38 M CCK/s
  (~2,6 us/CCK no A53 com -O3 já ligado) = ~9% do realtime — o gargalo dos
  fps é o custo por CCK do Rigel no A53, não o protocolo. Vale para ambos os
  backends (Emu68 dará fps similar; previsão falsificável). Próximo passo da
  Fase 7: profiling do Rigel por domínio no Pi (Agnus vs composição Denise)
  antes de qualquer otimização; candidatos: hot loop do slot scheduler,
  mtune=cortex-a53 (build usa a72), frameskip de composição.
- 2026-07-13: builds Pi SEM BTStack (BT em desenvolvimento prende o launcher
  na tela de scan com USB morto; regra registrada) e com USB+PROFILE.
- 2026-07-13: REVISÃO da conclusão estrutural (consideração externa em
  `consideracao.md` + recomputação do log do Pi): a métrica decisiva
  avg_step = CCK/chamada deu **~79 CCK por rigel_step_until** (beat 29→30:
  756.830 CCK em 9.586 chamadas; um beat ≈ 2 s) — granularidade SUB-scanline
  (linha = 227 CCK), cortada ~3× por linha por
  `rigel_get_next_observable_deadline`. O overhead EXTERNO por chamada segue
  irrelevante (~4,7 K chamadas/s), mas custo fixo INTERNO do Rigel por
  chamada seria multiplicado 3×: a conclusão "2,6 µs/CCK intrínseco" fica
  SUSPENSA até o experimento de granularidade (modo que ignora deadlines
  observáveis no drain e corta só em target/posted-stamp; avg_step deve ir a
  ~2000+). Se o fps subir → problema é custo por chamada (correção barata:
  coarsening de deadlines); se não → custo por CCK confirmado, seguir Fase 7.
  Hipóteses do documento já refutadas pelos dados: handshake dominante
  (full_waits=0, IO 7 µs, empty steps congelados), avanço duplo (call site
  único em `core_chipset.c`), fps de apresentação (CCK/s ≈ 5,2 fps virtuais
  ≈ 5,4 apresentados — emulação e apresentação coladas).
- 2026-07-13: PONTO CEGO tipo "QEMU órfão" no Pi: o log do primeiro gate NÃO
  capturou `[BOOT] ARM Clock` (captura serial começou tarde) e nada monitora
  throttle em runtime — firmware derruba o ARM para 600 MHz por undervoltage
  /térmica sem outro sintoma, o que halvaria toda a conta. Adicionado ao
  heartbeat: `[CORE0-HW] arm_mhz= throttled=` (mailbox GET_CLOCK_RATE ARM +
  GET_THROTTLED, sticky bits 16-18) e `[BPROF-SCHED] avg_step= core2_busy=`
  (ocupação de parede do Core 2 dentro da seção de step). Usuário reporta
  que já houve bare metal >10 fps com o Rigel (config a localizar) — se
  confirmado no mesmo clock, reforça a hipótese de integração/granularidade.
- 2026-07-13: item 1 (audit cross-core do Core 0) CONCLUÍDO — 4 raças da
  mesma classe do bug da fila de áudio corrigidas: (a) sync de controller
  ports (input Core 0 × rigel_step Core 2) sob lock; (b)
  `post_chipset_step` (serial/CIA SDR) sob lock; (c) presenter lê o struct
  do frame (width/pitch/pixels) sob lock — cópia de pixels fora, tearing
  documentado como único risco residual; (d) produção de áudio + leitura de
  IPL movidas para DENTRO da seção com lock do drain. Auditados e seguros:
  sync_ipl (só sob lock), console ring (atômico), ADF insert (pré-runtime).
- 2026-07-13: item 2 (TSAN) CONCLUÍDO — `bellatrix_tsan_posted_writes` e
  `bellatrix_tsan_audio_queue` no ctest: produtor/consumidor em pthreads
  reais sob `-fsanitize=thread`, sequência/ordem/perda verificadas através
  de muitos wraps. Armadilha de ambiente: TSAN aborta com "unexpected memory
  mapping" sob ASLR de kernels novos (vm.mmap_rnd_bits=32) — testes rodam
  via `setarch -R`. Suíte completa: 39/39 verdes.
- 2026-07-13: política de presenter implementada — o drain do Core 0
  apresenta apenas o frame pendente mais novo (Rigel só guarda o último
  composto; re-apresentar N vezes eram N-1 cópias redundantes de
  framebuffer). Contabilidade por frame emulado (counter, mouse tick)
  preservada via `on_frame_skipped`.
- 2026-07-13: A/B FORMAL Fase 0 no QEMU (TCG, host limpo verificado — 0
  QEMU órfão, load 0.44 —, 3 rodadas × 60 s por config, KS13 + megademoA,
  Emu68 backend, PROFILE):

  | config | CCK em ~17-18 s | frames | CCK @ ~58 s | frames | avg_step |
  |---|---|---|---|---|---|
  | single | 4.639.280 (@17,0-20,3 s parede) | 65 | sem dump tardio | — | — |
  | mc-cpu | 4.556.656-4.625.168 | 64-65 | 16,9-17,2 M | 238-243 | 174-175 |
  | mc-hybrid | 4.570.416-4.601.480 | 64 | 17,4-17,6 M | 245-248 | 221-224 |

  CONCLUSÕES: (1) PARIDADE single × multicore no mesmo ponto emulado —
  todos atingem ~4,6 M CCK/65 frames em ~17-18 s; a "regressão multicore"
  do início da ISSUE-0049 não existe mais no QEMU. (2) hybrid ≈ mc-cpu
  (+1-2%, dentro do ruído). (3) VARIÂNCIA: contadores no mesmo beat <1%
  entre rodadas; parede até ±10% (single r1 20,3 s vs r2/r3 17,0 s) —
  confirma contadores como métrica canônica no TCG. (4) O single é
  determinístico em CCK/frames entre rodadas; só a parede varia. (5)
  avg_step no QEMU (KS13+demo) = 175-224 ≈ scanline — o 79 do Pi é anômalo
  também nesta comparação, reforçando o experimento de granularidade.
  Observação de semântica: em mc-cpu o CPU respeita o backlog (8,2-8,5 K);
  em hybrid o CPU corre à frente sem limite (backlog ~200 M CCK no TCG,
  onde o m68k JIT é ~12× realtime) — no Pi real isso não ocorre (CPU <
  realtime), mas fica registrado como questão de spec da Fase 6.
- 2026-07-13: item 4 — CHIPSET_PUBLISH_MIN_CCK 1→227 medido contra a
  baseline: hybrid 18,1/18,5/18,9 M CCK e 255/261/266 frames vs
  17,4/17,6/17,4 M e 245-248 (+6% consistente, 3/3 acima de toda a
  baseline); cpu-driven em paridade no topo (17,4-17,7 M vs 17,2-17,5 M
  intercalado). Rodadas baixas apareceram em AMBAS as imagens no teste
  intercalado (baseline 15,2 M; 227 15,0/10,5 M) = ruído do host, que subiu
  de load 0,44 (baseline) para ~1,9 — DISCIPLINA: registrar load antes de
  cada bateria e comparar pelo topo/mediana dos contadores, nunca por
  rodada isolada. A regressão histórica do 227 (caught_up 11k→783k por
  VHPOSR) não voltou: beam f(t) removeu o rendezvous dos polls. DECISÃO:
  227 adotado (uma scanline de agregação; ~10× menos stores cross-core;
  atraso máximo de target ≤64 µs emulados).

# Bloqueios

Nenhum.
