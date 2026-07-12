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

# Bloqueios

Nenhum.
