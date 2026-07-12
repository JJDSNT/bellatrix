---
id: ISSUE-0049
title: "PiStorm temporal protocol: beam snapshots and self-paced Rigel"
status: doing
priority: high
type: performance
owner: agent
created_at: 2026-07-12
updated_at: 2026-07-12
tags:
  - pistorm
  - multicore
  - rigel
  - beam
  - scheduler
related_files:
  - external/rigel/include/rigel/rigel_beam.h
  - src/runtime/core_chipset.c
  - src/cpu/cpu_bridge.c
  - AI_context/issues/ISSUE-0048.md
  - AI_context/consolidated/issue_core0_arbiter_scheduler.md
---

# Objetivo

Aplicar ao Bellatrix a propriedade temporal observada no PiStorm: o chipset
progride por sua própria referência de tempo e a CPU sincroniza somente nos
pontos de contato que exigem coerência. O epoch do Core 0 é horizonte de
pacing/clamp, não barreira lockstep entre CPU e chipset.

# Arquitetura alvo

- Core 0 possui relógio de parede, seleção de política e horizonte de epoch.
- Core 2 possui o Rigel e progride autonomamente dentro desse horizonte.
- Core 1 roda a CPU livre; escritas comuns são postadas, IPL e estado quente
  são publicados, e apenas contatos críticos exigem barreira.
- Harness/testes preservam o modo CPU-driven determinístico.
- Bare metal oferece self-paced e modo híbrido com clamp no progresso da CPU.

# Itens PiStorm

- [x] Snapshot lock-free de DMACONR/INTENAR/INTREQR.
- [x] Fila SPSC de escritas postadas com timestamp.
- [x] Event stream por `CNTKCTL_EL1` para reduzir `empty_host_steps`.
- [x] Rigel self-paced contra wall clock, governado por horizons do Core 0.

# Definições que não podem se perder na implementação

- **Self-paced:** Rigel progride por uma referência temporal independente da
  CPU, como o chipset físico observado no protocolo PiStorm.
- **Epoch/horizon:** limite de trabalho publicado pelo Core 0; não é encontro
  obrigatório entre Core 1 e Core 2.
- **Deadline Rigel:** subdivisão local do trabalho do Core 2 para preservar
  eventos observáveis; não é barreira global.
- **Ponto de contato:** MMIO/IRQ/completion em que CPU e chipset precisam de
  uma relação de ordem explícita.
- **Modo determinístico:** timeline dirigida por ciclos publicados pela CPU,
  mantida para harness, testes e diagnóstico.
- **Modo realtime:** timeline derivada de `CNTPCT`, usada no produto bare metal.
- **Modo híbrido:** realtime limitado por
  `cpu_published + CHIPSET_MAX_BACKLOG_CCK` quando a CPU não acompanha.

# Plano completo de execução

## Fase 0 — Baseline, invariantes e observabilidade

- [ ] Congelar workloads de referência: KS1.3 boot, Workbench 1.3, AROS e
  megademo/Battle Squadron usados na ISSUE-0048.
- [ ] Registrar baseline single/multicore com `BELLATRIX_PROFILE=1`:
  CCK/s, `empty_host_steps`, MMIO por endereço, `caught_up`, tempo de espera,
  profundidade da fila postada, backlog CPU/chipset, frames e áudio produzido.
- [x] Separar resultados QEMU/TCG de hardware; usar contadores como fonte
  canônica no QEMU e múltiplas rodadas apenas para tempo de parede.
- [x] Adicionar contadores específicos para VPOSR/VHPOSR: projeção aceita,
  fallback, snapshot inválido e espera evitada.
- [x] Documentar invariantes de ordem: read-after-write, write-after-write,
  MMIO crítico, reset, STOP, IPL e mudança de modo temporal.
- [x] Definir feature flags/modos de runtime para permitir A/B e rollback sem
  recompilar todo o protocolo.

Gate: baseline reproduzível e cada caminho novo mensurável antes de mudar a
fonte de tempo.

## Fase 1 — Beam `f(t)` e eliminação do hot rendezvous

- [x] Definir `rigel_beam_geometry_t` e contrato público conservador.
- [x] Implementar `rigel_get_beam_geometry()` e `rigel_beam_position_at()`.
- [x] Exportar API, documentar e cobrir PAL/NTSC, wrap e entradas inválidas.
- [x] Publicar snapshot coerente pelo Core 2 com payload atômico/seqlock.
- [x] Servir VPOSR/VHPOSR no Core 1 por `f(cpu_time)` quando válido.
- [x] Manter rendezvous atual como fallback obrigatório para estado não
  representável (LOF/LOL/interlace/long-line).
- [x] Tornar VPOSW/VHPOSW/BEAMCON0 contatos críticos que invalidam/republicam
  a geometria antes de leituras posteriores.
- [x] Testar equivalência entre projeção e avanço real do Rigel em uma matriz
  maior de posições, múltiplos frames e mudanças PAL/NTSC.
- [ ] Validar boot/demo multicore e medir redução de MMIO crítico.
- [ ] Reavaliar `CHIPSET_PUBLISH_MIN_CCK` depois que VHPOSR não exigir flush.
- [ ] Decidir suporte exato a LOF/LOL ou manter fallback documentado com base
  na frequência medida em workloads reais.

Gate: nenhum desvio de VPOSR/VHPOSR, boot íntegro e queda mensurável das esperas.

## Fase 2 — Fechar o protocolo de contatos inspirado no PiStorm

- [x] Auditar todos os custom/CIA MMIO e classificar: snapshot, leitura live,
  escrita postada ou barreira crítica.
- [x] Formalizar read-after-write: leitura só observa após consumir todas as
  escritas anteriores relevantes; hot paths não podem furar essa ordem.
- [x] Formalizar timestamps da fila nos três modos temporais; no self-paced,
  escrita é aplicada no "agora" do chipset, não no contador livre da CPU.
- [ ] Definir política de fila cheia sem deadlock e contadores de pressão.
- [x] Garantir ordem entre writes postados, writes críticos, Copper e DMA.
- [x] Republicar snapshots após qualquer write que altere o estado publicado.
- [ ] Cobrir lifecycle: init, reset, pause, resume, shutdown e troca de modo
  zeram/rebaseiam fila, snapshots, sequência e timestamps coerentemente.
- [ ] Testar estresse SPSC e contatos concorrentes em host com sanitizers onde
  possível, mais testes bare-metal direcionados.

Gate: matriz de MMIO documentada e testes de ordenação sem depender do lock
global como garantia implícita.

## Fase 3 — Event stream e espera eficiente

- [x] Confirmar suporte ARMv8/BCM2837 e frequência útil do event stream.
- [x] Encapsular configuração de `CNTKCTL_EL1` no PAL; salvar/restaurar estado
  relevante e manter fallback quando indisponível.
- [ ] Escolher frequência por medição, sem alterar a timeline emulada.
- [x] Trocar polling/SEV excessivo do loop ocioso do Core 2 por WFE acordado
  pelo event stream e por eventos cirúrgicos (MMIO, IPL, shutdown).
- [x] Evitar tempestade de SEV entre Core 0/1/2; documentar quem acorda quem.
- [ ] Medir `empty_host_steps`, wakeups/s, custo ARM e latência de contato.
- [ ] Validar QEMU com fallback e Pi 3 real com event stream ativo.

Gate: redução clara de `empty_host_steps` sem perda de IRQ, frame, áudio ou
aumento relevante da latência de MMIO.

## Fase 4 — Autoridade temporal e epochs no Core 0

- [x] Criar estado explícito de timeline no Core 0: modo, `t0`, `cck0`,
  frequência, horizon publicado, geração e estado paused/rebase.
- [x] Converter `CNTPCT` para CCK com aritmética inteira/fração acumulada, sem
  drift por truncamento e sem assumir frequência fixa do counter.
- [x] Publicar horizon monotônico por atomics; Core 2 nunca lê wall clock
  diretamente como política paralela.
- [x] Implementar policies sobre a mesma interface:
  CPU-driven, realtime e híbrida com clamp.
- [x] Definir tamanho mínimo/máximo do horizon e limitar bursts sem criar
  rendezvous periódico.
- [ ] Tratar boot, pause, launcher, reset e troca de modo com rebase explícito;
  nunca tentar recuperar tempo de parede acumulado durante pausa.
- [x] Expor métricas: realtime target, horizon, Rigel time, CPU published,
  clamp ativo, drift e número de rebases.
- [x] Testar matematicamente monotonicidade, wrap e conversão longa.

Gate: Core 0 publica horizons corretos nos três modos, ainda sem remover o
caminho CPU-driven atual.

## Fase 5 — Core 2 self-paced

- [x] Fazer o loop do Core 2 consumir o horizon do Core 0 em vez de exigir
  autorização a cada quantum do Core 1 no modo realtime.
- [x] Manter `rigel_get_next_observable_deadline()` e próximo timestamp postado
  como cortes locais de `rigel_step_until(horizon)`.
- [x] Aplicar writes postados na semântica temporal definida na Fase 2.
- [x] Publicar progresso, IPL, hot regs, beam e completions incrementalmente.
- [x] Dormir quando alcançar o horizon; acordar por novo horizon, contato,
  event stream ou shutdown.
- [x] Implementar clamp híbrido e provar que degrada para o comportamento
  CPU-driven quando a CPU fica para trás.
- [x] Remover dependência funcional de `s_cpu_cck_target` apenas no modo
  realtime puro; mantê-la para métricas e para os outros modos.
- [x] Proteger contra salto enorme de wall clock, underflow, starvation e
  avanço após pause/reset.

Gate: Rigel continua avançando em realtime com Core 1 parado fora de um ponto
de contato, sem backlog ilimitado nem burst de recuperação.

## Fase 6 — Semântica da CPU no mundo self-paced

- [x] Leituras snapshot/projetáveis retornam sem rendezvous.
- [x] Leituras live sincronizam somente o necessário para observar o "agora"
  já alcançado pelo chipset, sem pedir que ele alcance tempo livre da CPU.
- [x] Escritas comuns são postadas no agora do chipset; críticas preservam
  ordem e efeito pontual sem reintroduzir lock por acesso em todo o barramento.
- [x] IPL permanece push do Core 2 para o backend CPU e acorda STOP via SEV.
- [ ] Definir comportamento quando CPU está muito rápida: VBL/beam limitam
  software bem-comportado; não criar throttle implícito no Emu68.
- [ ] Definir comportamento quando CPU está lenta e quando o clamp liga/desliga,
  com histerese se a medição mostrar oscilação.
- [ ] Validar Musashi 68000/68040 e Emu68 separadamente.
- [ ] Só criar API de ciclo exato no Emu68 se um contato ainda exigir essa
  informação; não é pré-requisito presumido do self-paced.

Gate: nenhum rendezvous global por quantum; sincronização ocorre apenas nos
pontos de contato documentados.

## Fase 7 — Frames, áudio e completions sob autoridade do Core 0

- [ ] Ordenar frame/audio/device completions pelo tempo emulado em que se
  tornam visíveis.
- [ ] Definir política de presenter quando adiantado/atrasado sem alterar o
  relógio interno do Rigel.
- [ ] Validar produção de Paula contra realtime antes de julgar qualidade.
- [x] Evitar que logging, presenter ou I/O físico bloqueiem o Core 2.
- [ ] Integrar métricas do Host Reactor: orçamento, misses e latência máxima.

Gate: ~3.546.895 CCK/s e ~50 frames/s PAL sustentados no Pi 3, áudio sem
underflow causado pelo scheduler e completions em ordem.

## Fase 8 — Validação, promoção e limpeza

- [ ] Testes unitários de timeline, beam, fila, clamp e rebase.
- [ ] Testes de integração single/multicore e CPU-driven/realtime/híbrido.
- [ ] QEMU: 4/4 ROM boots, boot+demo prolongado, zero freeze e backlog limitado.
- [ ] Pi 3: matriz KS1.3/WB1.3, AROS, demo/jogo, USB/BT quando aplicável.
- [ ] Rodada prolongada para drift, áudio, temperatura e estabilidade.
- [ ] A/B perfilado contra baseline da Fase 0; registrar regressões por domínio.
- [ ] Manter feature flag e fallback até hardware cumprir todos os gates.
- [ ] Atualizar documentação de arquitetura/runtime e consolidar ISSUE-0048.
- [ ] Remover caminho/locks antigos apenas depois da promoção, com medição que
  prove que não são mais usados.

Gate final: modo self-paced é padrão bare-metal somente após correção funcional,
realtime sustentado e estabilidade no Pi 3; harness continua determinístico.

# Métricas canônicas

- `rigel_cck / wall_second`, frames/s e áudio produzido/s.
- MMIO por classe/endereço; fast-path, fallback e waits evitados.
- Tempo/count de lock, rendezvous, WFE/SEV e event-stream wakeups.
- `empty_host_steps`, tamanho médio de `rigel_step_until` e motivo do corte.
- Fila postada: profundidade média/máxima, full waits e latência até aplicação.
- CPU published, Rigel time, realtime horizon, backlog, clamp e drift.
- IPL publish-to-observe e latência de saída de STOP.
- Orçamento do Core 0 e custo por core no hardware.

# Riscos e condições de parada

- Divergência de VPOSR/VHPOSR, perda de write ou violação read-after-write.
- Deadlock/livelock em fila, seqlock, WFE ou contato crítico.
- Rigel avançando durante pause/reset ou realizando burst após rebase.
- IRQ/completion perdida, duplicada ou fora de ordem.
- Regressão de boot/demo, backlog ilimitado ou piora medida contra baseline.
- Qualquer ocorrência interrompe a fase corrente; o modo CPU-driven permanece
  disponível para reprodução e bisecção.

# Log de execução

- 2026-07-12: recuperado o rascunho interrompido da API de beam no submódulo.
- 2026-07-12: implementadas captura e projeção pública, exportação no umbrella
  header, documentação e testes NTSC/PAL, wraps e fallbacks.
- 2026-07-12: Core 2 passou a publicar geometria por seqlock com payload
  atômico; Core 1 serve VPOSR/VHPOSR por `f(cpu_time)` e preserva o caminho
  síncrono quando a projeção não é exata.
- 2026-07-12: VPOSW/VHPOSW/BEAMCON0 classificados como writes críticos para
  que mudanças de contador/geometria não atravessem a fila postada.
- 2026-07-12: 26/26 testes Rigel passaram e o build bare-metal Musashi 68040
  multicore terminou limpo. Boot/demo e perfil ainda pendentes.
- 2026-07-12: AROS multicore profile chegou a 2329 frames no QEMU. Após a
  atividade iniciar (~frame 850, forte após 1500), o beam fast path acumulou
  218.407 VPOSR e 5.460.179 VHPOSR, com zero fallback e zero snapshot miss;
  progresso permaneceu monotônico e backlog limitado.
- 2026-07-12: event stream de Core 2 validado no QEMU a 244.140 Hz; no modo
  CPU-driven `empty_host_steps` estabilizou em 9.079, contra ~10 M antes.
- 2026-07-12: autoridade temporal implementada com modos CPU/realtime/híbrido,
  horizon atômico do Core 0, clamp, rebase de pausa e bursts Core 2 limitados.
  QEMU híbrido confirmou a política; TCG não sustenta realtime, como esperado.
- 2026-07-12: frame completion e polling HDMI migrados do Core 2 para Core 0;
  matriz MMIO virou política table-driven com teste host.
