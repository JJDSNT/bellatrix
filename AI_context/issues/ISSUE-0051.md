---
id: ISSUE-0051
title: "Fast Amiga compute: KS3.1/AROS, multicore and 68040+/Emu68 without instability"
status: doing
priority: critical
type: architecture
owner: agent
created_at: 2026-07-12
updated_at: 2026-07-15
tags: [performance, aros, ks31, multicore, emu68, 68040, pistorm]
blockers:
  - "provar Emu68 em hardware Pi real além do desktop — adiado por decisão do usuário"
related_files:
  - src/cpu/emu68
  - src/cpu/musashi
  - src/runtime/core_chipset.c
  - src/cpu/cpu_bridge.c
---

> **EMENDA ARQUITETURAL 2026-07-15 — ISSUE-0058 prevalece.** O objetivo de
> computação rápida permanece, mas a colocação Core1=Emu68, a independência de
> IRQ física e a substituição do fault handler não são mais decisões
> não-negociáveis. O baseline conservador volta a ser Emu68 no Core 0, com o
> contrato original de `start.c`/`vectors.c` auditado antes de qualquer nova
> topologia. Toda frente Emu68 desta issue fica subordinada à ISSUE-0058.

# Objetivo canônico

Construir uma máquina Amiga computacionalmente rápida e estável. O objetivo
primário original do Bellatrix é aceleração, não reprodução de um A500 stock:

```text
KS3.1 ou AROS
CPU 68040+ / Emu68
multicore
Fast RAM
boot e workloads CPU-bound muito mais rápidos que hardware clássico
custom chipset funcional sem desestabilizar a máquina
```

KS1.3/Battle/68000 deixam de dirigir a arquitetura. Permanecem cargas de
compatibilidade secundária e regressão do chipset, nunca razão para reduzir a
potência do sistema moderno.

# Decisões não negociáveis (corrigidas por ISSUE-0058)

- Emu68 executa inicialmente no Core 0, preservando o placement e o ambiente do
  desenho original. Outro placement exige prova de equivalência completa.
- Core 0 não throttla a CPU por wall clock, FPS, VBL ou throughput do Rigel;
  isso não implica separar o Host Reactor do core do JIT.
- Rigel mantém ownership único. Seu core e o placement dos serviços físicos
  serão confirmados depois da auditoria de startup/IRQ, não por inércia da
  topologia atual.
- A propriedade PiStorm é preservada: CPU rápida, writes postados, estado quente
  publicado, IPL push e barreiras raras.
- O fault handler nativo permanece baseline. API explícita é A/B opcional.
- AROS/KS3.1 chegando rapidamente ao desktop é gate de produto e não pode
  regredir silenciosamente por uma correção temporal.
- Métricas não são intercambiáveis: potência CPU, tempo até milestone,
  fidelidade do chipset e apresentação são eixos independentes.
- Estabilidade vence microbenchmark: nenhum ganho entra sem boot prolongado,
  memória/FPU/MMIO/IRQ íntegros e rollback disponível.

# Workloads e métricas primárias

## Tier 1 — produto

1. AROS 68040 e Emu68: tempo de seleção da mídia até milestones de boot e
   desktop utilizável.
2. KS3.1/Workbench: boot, Fast RAM, aplicações e benchmark CPU/memória/FPU.
3. SysInfo/AIBB ou equivalente: valor visto pelo guest, registrado junto com a
   fonte de timer usada pelo benchmark.
4. Emu68 multicore: throughput, liveness, STOP/IRQ, MMIO e estabilidade longa.

Métricas:

- wall-time até bootblock, Exec, DOS e desktop;
- benchmark guest (MIPS/Dhrystone/índices CPU/FPU/memória);
- ciclos/retired-work CPU por wall second, quando mensurável;
- latência MMIO e tempo em lock/contato;
- crash, freeze, corrupção, IRQ perdida e progresso prolongado.

## Tier 2 — chipset e compatibilidade

- KS1.3/Battle, demos e jogos conhecidos;
- áudio, beam, Copper, blitter, disk e presenter;
- correção funcional e ausência de regressões visíveis/audíveis.

Tier 2 não define a velocidade geral do produto. Uma regressão funcional ainda
bloqueia promoção, mas chipset a 100% do wall clock não é meta desta issue e
não autoriza throttlar Tier 1 para imitar 68000 stock. Otimização interna do
Rigel pertence exclusivamente à ISSUE-0053.

# Frentes de implementação

## A — Congelar o ganho já observado

- [ ] Instrumentar milestones AROS/KS3.1 com timestamps wall monotônicos.
- [ ] Registrar baseline Musashi 68040 multicore atual em AROS e KS3.1.
- [ ] Rodar SysInfo/AIBB e registrar CPU, FPU, Chip RAM e Fast RAM.
- [ ] Definir margem de não regressão para tempo até desktop e benchmarks.

## B — Emu68 como backend primário rápido

- [x] Concluir primeiro a auditoria e o rebaseline Core 0 de ISSUE-0058
  (P0/P1 completos 2026-07-15).
- [x] Raiz-causar e corrigir a regressão que travava todo build Emu68 após
  `[JIT] Let it go...` — ISSUE-0061 (2026-07-16): o progress driver do
  `MainLoop` tinha sido deletado por um refactor e excluído por
  `BELLATRIX_EMU68_FAULT_DRIVEN`; restaurado incondicional. Confirmado em
  QEMU (`frame_counter` 0→500+, IPL/interrupção real). Ainda não provado em
  hardware Pi real.
- [ ] Provar Emu68 no Pi com AROS/KS3.1 além do desktop no baseline fault-driven.
- [ ] Fechar liveness STOP/IRQ/MMIO sem janela periódica imposta pelo Core 0.
- [ ] Medir fault/callback bus e otimizar hot paths um a um.
- [ ] Validar código mutável, cache/JIT invalidation, FPU e memória longa.
- [ ] Comparar Emu68 contra Musashi 68040 no mesmo milestone e wall-time.

## C — Bellatrix multicore/bus sem frear CPU

- [ ] Não mover o JIT para Core 1 antes do gate de equivalência de ISSUE-0058.
- [ ] Medir wakeups reais, cache-line bouncing, locks e publicações efetivas.
- [ ] Eliminar SEV/barreiras/republicações sem consumidor ou mudança de estado.
- [ ] Manter/expandir MMIO snapshot/postado e contatos explícitos.
- [ ] Avaliar remoção do lock somente se não virar rendezvous periódico.
- [ ] Preservar Core 0 leve e Core 3 reservado até medição justificar worker.

## D — Contrato funcional do chipset e serviços

- [ ] Manter correção de jogos/demos como gate de compatibilidade.
- [ ] Melhorar áudio sem mascarar falta de samples com métrica visual.
- Otimização de throughput do Rigel não faz parte desta frente; ISSUE-0053 é a
  única issue autorizada a acompanhá-la.

## E — Matriz e promoção

- [ ] 39/39 testes e novos testes permanecem verdes.
- [ ] AROS e KS3.1: Musashi 68040 multicore e Emu68 multicore no Pi.
- [ ] Boot prolongado, SysInfo/AIBB, Fast RAM, FPU, USB/HID, disco e áudio.
- [ ] KS1.3/Battle/demos como regressão secundária.
- [ ] Feature flags e imagens A/B preservadas até promoção.

# Critério final

Bellatrix entrega aceleração mensurável e estável em AROS/KS3.1 com multicore e
68040+/Emu68, preservando o protocolo PiStorm e sem transformar Core 0 num
throttle. Compatibilidade do chipset permanece correta, mas não redefine o
produto como um Amiga stock.
