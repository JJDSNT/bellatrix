---
id: ISSUE-0058
title: "Rebaseline conservador do Emu68: Core 0, fault handler, IRQ física e vectors.c"
status: ready
priority: critical
type: research
owner: agent
created_at: 2026-07-15
updated_at: 2026-07-15
tags: [emu68, pistorm, core0, irq, bluetooth, fault-handler, vectors, multicore]
related_files:
  - authors_note.md
  - uae_references.md
  - emu68/src/aarch64/start.c
  - emu68/src/aarch64/vectors.c
  - emu68/src/ExecutionLoop.c
  - patches/0007-bellatrix-boot-sequence.patch
  - src/cpu/emu68/bellatrix.c
  - src/cpu/cpu_bridge.c
  - src/runtime/core_io.c
  - AI_context/issues/ISSUE-0051.md
  - AI_context/issues/ISSUE-0052.md
  - AI_context/issues/ISSUE-0057.md
---

# Resumo executivo

Esta é a decisão arquitetural vigente para a integração Emu68. Ela supera as
decisões anteriores que tratavam o Emu68 como worker puro no Core 1, declaravam
desnecessária sua relação com IRQ ARM física, ou faziam da remoção do fault
handler uma condição de maturidade.

A orientação conservadora, após a conversa relatada pelo usuário com o autor do
PiStorm/Emu68, é:

1. não substituir nem redesenhar o fault handler como pré-requisito;
2. estudar o `start.c` original e satisfazer no Bellatrix o contrato de core,
   exceções, timers e IRQ do Emu68;
3. manter/restaurar o Emu68 no Core 0 como baseline até provar equivalência em
   outro core;
4. usar IRQ ARM normal para Bluetooth; não gastar FIQ com Bluetooth;
5. considerar FIQ para USB/DWC2/SOF somente se necessidade e medição futuras o
   justificarem;
6. usar `vectors.c` como ponto de entrada do barramento Emu68 para o chipset,
   convergindo num serviço comum que outros backends de CPU chamam diretamente.

`authors_note.md` preserva a orientação recebida. `uae_references.md` é
referência de simplificação e desempenho, não base substituta do Rigel.

# Evidência local já estabelecida

O checkout `emu68/` está patchado. A análise deve sempre separar:

```text
Emu68 HEAD 305f686f8471
        + patches Bellatrix
        = imagem realmente executada
```

No `HEAD` original:

- o boot e `M68K_StartEmu()` permanecem no Core 0;
- IRQ global, PMU e timers são roteados/habilitados no Core 0;
- os cores secundários recebem setup de EL1, MMU, caches, stack, PMU e
  `VBAR_EL1`, mas executam tarefas auxiliares ou estacionam;
- `vectors.c` traduz Data Abort em acesso externo e possui semântica própria
  para IRQ/FIQ no contexto M68K.

O patch `0007-bellatrix-boot-sequence.patch` move o backend/JIT para o Core 1 e
reaproveita os cores secundários, mas não reescreve o bloco original de
roteamento de IRQ/PMU/timers. Portanto a topologia atual mudou uma premissa
central sem demonstrar que todas as necessidades do Emu68 foram transportadas.

Há uso observável de IRQ física no desenho Emu68:

- os vetores IRQ/FIQ consultam `INT_shadow` e o contexto obtido por
  `TPIDRRO_EL0`;
- caminhos gerados pelo JIT manipulam `DAIF` de acordo com estado 68k;
- STOP/WFE, wakeup, PMU/timers, `VBAR_EL1` e registradores fixos fazem parte do
  mesmo ambiente de execução.

Não se deve concluir antecipadamente que todo esse mecanismo continua
necessário no variant Bellatrix, mas também não se pode declará-lo morto antes
de uma auditoria e uma prova de equivalência.

# Decisão arquitetural vigente

## 1. Baseline de colocação

O baseline conservador coloca o Emu68 no Core 0, como no desenho original.
Qualquer colocação no Core 1 ou em outro core passa a ser uma otimização futura
que exige prova explícita de equivalência do ambiente arquitetural.

A distribuição dos demais cores será redefinida depois da auditoria. Não se
deve preservar por inércia a topologia Core0=Host Reactor, Core1=CPU,
Core2=Rigel, Core3=aceleração. Rigel continua precisando de ownership único; a
posição concreta dos serviços físicos depende das limitações reais de
roteamento do BCM2837 e do contrato Emu68.

## 2. MMIO e fault handler

O caminho fault-driven é o baseline e não será removido nem tratado como dívida
arquitetural por padrão:

```text
JIT Emu68
  -> load/store AArch64
  -> Data Abort para região externa
  -> vectors.c
  -> adaptador comum do barramento Bellatrix
  -> Rigel/chipset
```

Fast RAM, Zorro III e regiões comprovadamente diretas permanecem no caminho ARM
direto. O espaço Amiga que precisa de semântica externa pode usar o fault
handler. Otimizações devem nascer de perfil real, sem substituir primeiro o
mecanismo funcional.

A API pública fault-independent de ISSUE-0057 pode ser preservada como pesquisa
ou opção experimental, mas não é mais prioridade nem destino obrigatório do
produto.

## 3. `vectors.c` como fronteira

`vectors.c` é a entrada Emu68 para transações externas, mas deve ser fino:

- preservar/restaurar exatamente o contexto do JIT;
- decodificar endereço, largura, direção e atributos do fault;
- chamar um serviço comum do barramento;
- devolver o valor ou a exceção correta e retomar o JIT.

Lógica interna de Agnus, Paula, CIA, Denise, scheduler, vídeo ou stacks físicas
não deve morar no vetor. Musashi e outros backends não devem fabricar Data
Abort: chamam diretamente o mesmo serviço comum que o final de `vectors.c` usa.

## 4. IRQ Bluetooth e FIQ

Bluetooth deve usar IRQ ARM normal. Antes de habilitá-la é obrigatório entender:

- afinidade real de GPU IRQ no BCM2837;
- qual vector table está instalada em cada core;
- como distinguir/atender a fonte Bluetooth sem convertê-la indevidamente em
  INT6/EXTER do Amiga;
- quais registradores e contexto devem ser preservados se a IRQ atingir o core
  do JIT;
- como reconhecer/acknowledge a fonte rapidamente e deferir a stack BT para um
  scheduler mínimo seguro.

FIQ não será usada para Bluetooth. USB/DWC2/SOF é apenas o candidato preferível
caso uma necessidade futura de FIQ seja demonstrada; isso não autoriza
implementá-la agora.

# Impacto nas branches existentes

## Branch atual `emu68-public-machine-api`

Não é candidata a merge arquitetural enquanto ISSUE-0058 não estiver
resolvida. Seus fixes de CPU, exceção, mapa, instrumentação e testes podem ser
reaproveitados seletivamente. O objetivo de substituir faults e a colocação do
Emu68 no Core 1 não devem ser promovidos por inércia.

## Branch de Bluetooth

O trabalho funcional pode ser reaproveitado, mas sua topologia, ownership,
vector table, afinidade IRQ e pressupostos sobre o core do Emu68 devem ser
reavaliados contra esta issue antes de merge ou cherry-pick.

Nenhuma das duas branches deve ser descartada; ambas viram fontes de commits
selecionáveis depois que o baseline conservador estiver provado.

# Prioridade e ordem obrigatória

## P0 — Congelar decisões divergentes

- [ ] Não promover ISSUE-0057 nem a topologia Core1=Emu68.
- [ ] Não integrar a branch Bluetooth antes da auditoria de startup/IRQ.
- [ ] Preservar imagens, logs e commits conhecidos para A/B e rollback.

## P1 — Auditoria do contrato Emu68 original

- [ ] Comparar sempre `git show HEAD:src/aarch64/start.c` com o arquivo patchado.
- [ ] Mapear `_start`, `_secondary_start`, `secondary_boot`, `boot` e
  `M68K_StartEmu` por core.
- [ ] Mapear por core: EL, MMU/TTBR, caches, stack, VBAR, TPIDR, PMU, timers,
  DAIF, IRQ/FIQ routing, mailbox, WFE/SEV e STOP.
- [ ] Traçar todos os consumidores/produtores de `INT_shadow`, `INT.ARM`, IPL e
  os caminhos gerados que habilitam/mascaram IRQ ARM.
- [ ] Documentar o que é requisito do JIT, requisito PiStorm físico ou código
  que o Bellatrix pode substituir com prova.

## P2 — Rebaseline funcional no Core 0

- [ ] Construir uma variante mínima com Emu68 no Core 0 e fault handler nativo.
- [ ] Manter `vectors.c` no caminho de MMIO e eliminar apenas duplicação
  Bellatrix que não faça parte do adaptador fino.
- [ ] Definir placement provisório de Rigel e serviços sem alterar o contrato
  Emu68 antes das medições.
- [ ] Validar DiagROM, KS1.3, KS3.1 e AROS, incluindo STOP/IRQ/MMIO/FPU/Fast RAM.
- [ ] Congelar essa imagem como referência de hardware Raspberry Pi 3B.

## P3 — IRQ Bluetooth alinhada

- [ ] Auditar o roteamento BCM2837 e as fontes PL011/DWC2.
- [ ] Projetar IRQ normal para Bluetooth compatível com o Core 0/Emu68.
- [ ] Handler mínimo: identificar e reconhecer o hardware, registrar pending e
  retornar; executar BTStack fora da exceção.
- [ ] Provar ausência de corrupção de contexto JIT, IRQ Amiga espúria, perda de
  byte UART, regressão de boot e starvation.
- [ ] Reaproveitar seletivamente a branch Bluetooth depois desses gates.

## P4 — Otimização posterior

- [ ] Medir fault handler, barramento, sincronização e Rigel separadamente.
- [ ] Comparar API explícita somente como A/B opcional contra o baseline.
- [ ] Avaliar mover Emu68 do Core 0 apenas com checklist de equivalência completo.
- [ ] Usar TeensyUAE/UAE 0.6.9 para estudar scheduler de eventos, hot path,
  renderização por linha e áudio adaptativo, sem copiar código/licença.

# Critérios de aceite

- [ ] Documento de contrato original-versus-patch completo e revisável.
- [ ] Emu68 Core 0 + fault handler boota de forma repetível em hardware.
- [ ] Data Abort/MMIO, STOP, IPL/IRQ, FPU, Fast RAM e código mutável validados.
- [ ] Bluetooth funciona por IRQ normal sem corromper o Emu68 ou gerar IPL
  Amiga espúrio.
- [ ] Cada core tem VBAR, DAIF, stack, timer e ownership documentados.
- [ ] Branches antigas só contribuem por commits revisados contra esta issue.
- [ ] Documentos canônicos em `docs/` são atualizados somente depois da prova;
  até lá, ISSUE-0058 é a autoridade operacional.

# Entendimentos anteriores explicitamente superados

Até nova validação, não usar como decisão vigente afirmações de que:

- Emu68 deve necessariamente rodar no Core 1;
- Core 0 deve necessariamente ser Host Reactor separado do JIT;
- Emu68 não depende de IRQ ARM física no Bellatrix;
- IRQ física nunca deve entrar no domínio/vector do Emu68;
- o roteamento original ao Core 0 é mera herança descartável;
- o fault handler deve ser substituído pela API pública;
- a API pública é pré-requisito para scheduler/multicore correto.

Esses textos permanecem como histórico útil, mas ISSUE-0058 prevalece quando
houver conflito.

# Log de execução

- 2026-07-15: orientação externa preservada em `authors_note.md`; referência
  UAE preservada em `uae_references.md`.
- 2026-07-15: comparação local confirmou que `start.c` no disco contém o patch
  Bellatrix e que o `HEAD` original mantém JIT, IRQ, PMU e timers no Core 0.
- 2026-07-15: criada esta rebaseline; branches pública e Bluetooth colocadas em
  revisão arquitetural antes de integração.
