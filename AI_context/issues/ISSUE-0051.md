---
id: ISSUE-0051
title: "Fast Amiga compute: KS3.1/AROS, multicore and 68040+/Emu68 without instability"
status: doing
priority: critical
type: architecture
owner: agent
created_at: 2026-07-12
updated_at: 2026-07-12
tags: [performance, aros, ks31, multicore, emu68, 68040, pistorm]
related_files:
  - src/cpu/emu68
  - src/cpu/musashi
  - src/runtime/core_chipset.c
  - src/cpu/cpu_bridge.c
---

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

# Decisões não negociáveis

- Core 1 executa a CPU à máxima capacidade. Core 0 não throttla CPU por wall
  clock, horizon, FPS, VBL ou throughput atual do Rigel.
- Core 0 é control plane/Host Reactor e publica permissão de trabalho para o
  chipset; não transforma a CPU em worker lockstep por epochs.
- Core 2 possui Rigel e progride autonomamente. Sincronização CPU↔chipset ocorre
  apenas em contatos que exigem coerência.
- A propriedade PiStorm é preservada: CPU rápida, writes postados, estado quente
  publicado, IPL push e barreiras raras.
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

- [ ] Provar Emu68 multicore no Pi com AROS/KS3.1 além do desktop.
- [ ] Fechar liveness STOP/IRQ/MMIO sem janela periódica imposta pelo Core 0.
- [ ] Medir fault/callback bus e otimizar hot paths um a um.
- [ ] Validar código mutável, cache/JIT invalidation, FPU e memória longa.
- [ ] Comparar Emu68 contra Musashi 68040 no mesmo milestone e wall-time.

## C — Bellatrix multicore/bus sem frear CPU

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
