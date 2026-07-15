---
id: ISSUE-0058
title: "Rebaseline conservador do Emu68: Core 0, fault handler, IRQ física e vectors.c"
status: doing
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

# Resultado da auditoria P1

## O que o `start.c` original realmente faz

O log `Setting IRQ routing to core 0` corresponde à escrita zero em
`0x4000000c` (vista no mapeamento virtual como `0xf300000c`): seleciona o Core 0
como destino das GPU IRQs. Não significa, isoladamente, "desabilitar todas as
IRQs". O bloco seguinte configura separadamente as fontes locais:

| Recurso | Core 0 original | Cores 1-3 originais | Consequência |
|---|---|---|---|
| execução Emu68/JIT | `M68K_StartEmu()` | tarefas PiStorm/park | CPU, contexto e exceções coincidem no Core 0 |
| `VBAR_EL1` | vetores Emu68 | vetores Emu68 em `secondary_boot()` | qualquer exceção entra no contrato Emu68 |
| `TPIDRRO_EL0` | aponta para `M68KState` ao carregar/iniciar contexto | não recebe contexto M68K | os vetores rápidos só são válidos no core do JIT |
| GPU IRQ | roteada ao Core 0 | não selecionada | periféricos ARM e caminho PiStorm convergem no Core 0 |
| PMU IRQ | habilitada/roteada ao Core 0 | mascarada nos outros cores | não remover antes de provar que overflow não participa do runtime |
| timer IRQ local | quatro fontes habilitadas no Core 0 | desabilitadas | preservar no primeiro baseline; medir consumidores depois |
| mailbox IRQ local | desabilitada | desabilitada | `SEV/WFE` não depende de mailbox IRQ |
| PMU cycle counter | inicializado | inicializado em cada secondary | contagem por core faz parte do ambiente usado por métricas/JIT |

O patch Bellatrix `0007` mantém esse bloco de roteamento, mas muda o consumidor:
Core 0 vira supervisor e o JIT passa ao Core 1. O Core 1 recebe MMU/cache,
`VBAR_EL1`, PMU e depois `TPIDRRO_EL0`, porém não recebe o roteamento físico que
continuou no Core 0. Assim, cada metade parece inicializada isoladamente, mas o
contrato original deixou de estar co-localizado.

## Contrato observado em `vectors.c` e no JIT

- Data Abort síncrono entra em `SYSHandler`/`SYSPageFault*Handler` e implementa
  o acesso externo PiStorm. Este é o fault handler a preservar.
- O slot IRQ de EL1h/SPx original é deliberadamente especial: salva somente
  `x0/x1`, consulta `INT_shadow`, obtém `M68KState` via `TPIDRRO_EL0` e publica
  `INT.ARM=6` quando `INTENA` permite EXTER.
- FIQ segue contrato semelhante e SError publica nível 7. Portanto não são
  slots genéricos onde uma função C comum possa ser ligada sem redefinir a ABI.
- O JIT usa registradores AArch64 fixos, inclusive SIMD, e código gerado altera
  `DAIF` de acordo com o IPL 68k em MOVE-to-SR, RTE e STOP. Preservar apenas a
  ABI C normal dentro de uma exceção não basta.
- `ExecutionLoop.c` consome `INT.ARM`, `INT.ARM_err` e IPL para construir a
  entrada de interrupção 68k. Desacoplar esses campos é uma alteração semântica,
  não mera limpeza de plataforma.
- STOP usa `WFE` no variant PiStorm e observa o estado de interrupção. O patch
  Bellatrix que transforma STOP em yield pode continuar útil, mas precisa ser
  validado como adaptação consciente, não como evidência de que o contrato
  físico original é dispensável.

## Classificação para a rebaseline

### Invariantes do core do JIT — preservar sem negociação no primeiro passo

- EL1, MMU/TTBR, caches, stack e `VBAR_EL1` válidos;
- `TPIDRRO_EL0` apontando para o contexto M68K corrente;
- contexto inteiro/SIMD do JIT preservado exatamente nas exceções;
- Data Abort e retomada pelo fault handler original;
- coerência entre IPL 68k, `DAIF`, STOP/WFE e wakeup;
- PMU/timer/routing original até existir teste que autorize cada remoção.

### Semântica PiStorm que pode mudar somente com prova de equivalência

- tradução de IRQ/FIQ física em `INT.ARM`/nível 6;
- sombras `INTENA`/`INTREQ`, overlay, autoconfig e despacho de bus em
  `vectors.c`;
- tarefas auxiliares e cadência de housekeeper que possam produzir eventos
  observados pela CPU.

### Placement que pode ser redefinido

- async logger, write-back e housekeeper nos cores secundários;
- core exato do Rigel e do reactor físico, desde que ownership e comunicação
  sejam explícitos;
- otimizações da API pública e polling cooperativo, depois do baseline.

## Auditoria da branch `issue-0054-bt-physical-irq`

A branch contém evidência de hardware e componentes valiosos: ring SPSC de RX,
top-half limitado, defer/rearm, budgets do BTstack, máquina de estados, parser
HID, contadores de erro/overflow e um gate que verifica os offsets arquiteturais
de 0x80 bytes da vector table. Esses itens são candidatos a cherry-pick ou
transplante seletivo.

Sua política de exceções não é transplantável como está:

- reserva FIQ para PL011, contrariando a orientação vigente;
- substitui os slots IRQ/FIQ do Emu68 por handlers físicos Bellatrix;
- trata `INT.ARM` como legado que nunca deve receber IRQ física;
- mascara PMU/timers no `start.c` sem antes reconstruir o contrato original;
- usa IRQ normal para DWC2 e FIQ para Bluetooth, alocação que agora deve ser
  reavaliada/invertida;
- chama C após um save de contexto criado pela própria branch; isso não pode
  ser aplicado sobre o slot SPx original, que salva apenas `x0/x1`.

O bug descoberto nessa branch — slots IRQ/FIQ maiores que 0x80 fazendo o
hardware entrar no meio do slot anterior — permanece uma lição e um gate de
build obrigatório para qualquer nova integração de `vectors.c`.

## Topologia conservadora provisória

| Core | Papel inicial | Contrato |
|---|---|---|
| 0 | Emu68/JIT | boot original, fault handler, `VBAR`, `TPIDRRO`, DAIF e IRQ física preservados |
| 1 | auxiliar/park | disponível para serviço medido; não recebe o JIT nesta fase |
| 2 | Rigel | owner único do chipset e produtor do IPL Amiga |
| 3 | reactor físico provisório | BTstack/USB/console fora da exceção, acordado por flag + `SEV` |

O Core 3 é provisório porque o runtime atual já possui uma entrada de I/O
estacionada, reduzindo o deslocamento necessário. A IRQ PL011 chega ao Core 0,
cujo top-half deve somente drenar/registrar pending e acordar o consumidor. Não
se deve executar BTstack no vetor nem depender de o loop infinito do JIT chamar
periodicamente o reactor.

## Forma mínima da integração IRQ Bluetooth

O slot SPx IRQ precisa primeiro identificar a fonte física. Para PL011, o
caminho Bellatrix deverá:

1. preservar o contexto adicional exigido pelo JIT, com layout e tamanho de
   slot verificados no ELF;
2. confirmar `UART0/MIS`, mascarar RX/RT e drenar uma quantidade limitada para
   o ring;
3. limpar/reconhecer apenas a fonte atendida, publicar pending e emitir `SEV`;
4. restaurar o contexto e retornar sem escrever `INT.ARM`;
5. para uma fonte PiStorm/Emu68, manter o comportamento original de nível 6;
6. para fonte desconhecida, contar e conter a fonte sem fabricar IRQ Amiga.

O dispatcher deve ser discriminador, não substituto do vetor Emu68. O primeiro
protótipo pode usar um trampoline assembly com save completo demonstrável; uma
chamada C só é aceitável depois dessa preservação. FIQ permanece intocada pelo
Bluetooth.

# Primeiro baseline implementado

O branch `emu68-core0-rebaseline` implementa a topologia conservadora atrás de
dois controles reversíveis:

- `BELLATRIX_EMU68_CORE0_REBASELINE=1` (padrão no branch) mantém o entry do
  Emu68 no boot Core 0; valor zero restaura a topologia multicore anterior;
- `BELLATRIX_EMU68_ACCESS_MODE=fault` (padrão no branch) seleciona a execução
  nativa fault-driven; `public` preserva o A/B com a API explícita.

No modo conservador:

- Core 0 entra diretamente em `M68K_StartEmu`, sem o loop de quanta da API;
- Core 2 continua owner único do Rigel;
- Core 3 executa o reactor físico e recebe event stream local de ~953 Hz;
- Core 1 permanece estacionado;
- o `MainLoop` compilado não chama helpers `emu68_machine_*`;
- STOP volta ao caminho PiStorm original com `WFE`, em vez de yield gerenciado;
- a entrega de interrupção combina `INT.ARM`/níveis 6-7 originais com o IPL
  publicado pelo Rigel, sem descartar a IRQ física;
- mudanças de IPL no Core 2 atualizam o `M68KState` nativo via `PAL_IPL_Set()` e
  emitem `SEV`, pois não existe mais um quantum gerenciado no Core 1 para puxar
  `s_pending_ipl`.

O fault handler e os slots de `vectors.c` não foram substituídos. O ELF mantém
os oito slots nos offsets arquiteturais `+0x000`, `+0x080`, `+0x100`,
`+0x180`, `+0x200`, `+0x280`, `+0x300` e `+0x380`.

## Invariante de ownership das UARTs

Por decisão explícita do usuário, não existe handoff de console:

- AUX miniUART pertence ao log desde o primeiro estágio de boot;
- PL011/UART0 pertence ao Bluetooth desde o primeiro estágio;
- no QEMU, o log deve ser capturado pela segunda UART (`-serial null -serial
  stdio`), nunca reaproveitando PL011 como console.

## Primeiro protótipo de IRQ Bluetooth alinhada

O protótipo preserva o slot SPx IRQ original. Dentro do limite de `0x80`, um
discriminador lê `IRQ_PENDING_2` e desvia somente UART0/GPU IRQ 57. Todas as
outras fontes continuam byte a byte pelo caminho Emu68 que publica
`INT_shadow`/`INT.ARM=6`.

O desvio UART entra num trampoline fora da vector table que salva e restaura
`x0-x30`, `q0-q31`, `FPCR` e `FPSR`. O top-half então:

- desabilita somente a rota UART0 no controlador BCM2837;
- mascara RX/RT no PL011;
- drena no máximo 64 bytes para o ring SPSC;
- publica `CORE_IO_EVENT_BLUETOOTH` e emite `SEV`;
- nunca chama BTstack, logger ou alocador dentro da exceção.

O reactor normal no Core 3 consome o ring e rearma PL011 + controlador. FIQ e
o fault handler não foram modificados. O gate `scripts/check_bt_irq_abi.sh`
verifica offsets, fallback Emu68, contexto completo e FIQ intocada em todo
build.

## Escopo de validação vigente

Por decisão do usuário em 2026-07-15, não executar validação em hardware real
por enquanto. Os gates desta fase são exclusivamente build, inspeção do ELF,
testes automatizados e QEMU. Imagem para Raspberry Pi 3B e testes físicos
permanecem adiados até autorização explícita futura.

## Validação inicial do baseline

- build cruzado Emu68/multicore/fault, USB/BT/launcher desligados: PASS;
- build de rollback `public` + topologia multicore anterior: PASS;
- `tests/unit/run_emu68_machine.sh`: PASS, preservando o A/B público;
- verificação de reprodução: patches `0003`, `0020` e `0025` passam
  `git apply --reverse --check` sobre o checkout configurado;
- QEMU `raspi3b` + DiagROM: PASS até OVL, UDS/LDS e detecção/teste de Chip RAM;
- QEMU com o novo vetor, BT desabilitado e log na segunda UART: PASS até OVL,
  UDS/LDS e teste de Chip RAM;
- QEMU com BT habilitado e log na segunda UART: após o timeout esperado do
  controlador inexistente, libera Core0/JIT e alcança OVL, UDS/LDS e Chip RAM;
  isso não é gate do transporte, pois o modelo não oferece um BCM4343x e não
  valida a IRQ Bluetooth real;
- QEMU/AROS 1 MiB com BT habilitado: após o mesmo timeout controlado alcança
  JIT, OVL, `FNOP` e `FSAVE`, igualando o smoke anterior sem provar boot/FPU;
- QEMU `raspi3b` + AROS 1 MiB por 60 s: alcança JIT, OVL e as instruções
  `FNOP`/`FSAVE`; ainda não é boot AROS nem gate de FPU concluído;
- o log confirma `core=0`, `native fault-driven execution selected`, Core 2 a
  244140 Hz e Core 3 a 953 Hz;
- imagem do gate: SHA-256
  `d055e3f1b1078d750109dcdc54b2a29cb5b76f4f944af29fc50aa9d5548c8206`.

O download de firmware do CMake produziu um DTB vazio neste worktree isolado;
o smoke QEMU usou o DTB válido do checkout principal, do mesmo modelo/commit.
Isso é uma pendência de infraestrutura do artefato, não um resultado do guest.

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

- [x] Não promover ISSUE-0057 nem a topologia Core1=Emu68.
- [x] Não integrar a branch Bluetooth antes da auditoria de startup/IRQ.
- [x] Preservar imagens, logs e commits conhecidos para A/B e rollback.

## P1 — Auditoria do contrato Emu68 original

- [x] Comparar sempre `git show HEAD:src/aarch64/start.c` com o arquivo patchado.
- [x] Mapear `_start`, `_secondary_start`, `secondary_boot`, `boot` e
  `M68K_StartEmu` por core.
- [x] Mapear por core: EL, MMU/TTBR, caches, stack, VBAR, TPIDR, PMU, timers,
  DAIF, IRQ/FIQ routing, mailbox, WFE/SEV e STOP.
- [x] Traçar todos os consumidores/produtores de `INT_shadow`, `INT.ARM`, IPL e
  os caminhos gerados que habilitam/mascaram IRQ ARM.
- [x] Documentar o que é requisito do JIT, requisito PiStorm físico ou código
  que o Bellatrix pode substituir com prova.

## P2 — Rebaseline funcional no Core 0

- [x] Construir uma variante mínima com Emu68 no Core 0 e fault handler nativo.
- [x] Manter `vectors.c` e o fault handler no caminho de MMIO.
- [ ] Afinar o adaptador e eliminar apenas duplicação Bellatrix comprovada,
  depois dos gates funcionais.
- [x] Definir placement provisório de Rigel e serviços sem alterar o contrato
  Emu68 antes das medições.
- [ ] Validar DiagROM, KS1.3, KS3.1 e AROS, incluindo STOP/IRQ/MMIO/FPU/Fast RAM.
  - [x] DiagROM em QEMU: OVL, UDS/LDS e Chip RAM alcançados.
  - [ ] AROS em QEMU (smoke chega a `FNOP`/`FSAVE`, boot ainda não provado).
  - [ ] KS1.3 e KS3.1 em QEMU (ROMs não presentes neste worktree).
  - [ ] STOP/IRQ/FPU/Fast RAM e código mutável em testes dedicados.
- [ ] Hardware Raspberry Pi 3B: adiado por decisão do usuário; não executar
  sem nova autorização explícita.

## P3 — IRQ Bluetooth alinhada

- [x] Auditar o roteamento BCM2837 e as fontes PL011/DWC2.
- [x] Projetar IRQ normal para Bluetooth compatível com o Core 0/Emu68.
- [x] Handler mínimo: identificar e reconhecer o hardware, registrar pending e
  retornar; executar BTStack fora da exceção.
- [ ] Provar ausência de corrupção de contexto JIT, IRQ Amiga espúria, perda de
  byte UART, regressão de boot e starvation.
- [x] Reaproveitar seletivamente ring SPSC, budgets e defer da branch
  Bluetooth, sem trazer FIQ nem a substituição dos vetores.

## P4 — Otimização posterior

- [ ] Medir fault handler, barramento, sincronização e Rigel separadamente.
- [ ] Comparar API explícita somente como A/B opcional contra o baseline.
- [ ] Avaliar mover Emu68 do Core 0 apenas com checklist de equivalência completo.
- [ ] Usar TeensyUAE/UAE 0.6.9 para estudar scheduler de eventos, hot path,
  renderização por linha e áudio adaptativo, sem copiar código/licença.

# Critérios de aceite

- [x] Documento de contrato original-versus-patch completo e revisável.
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
- 2026-07-15: auditoria P1 concluída. Confirmado que `0x4000000c` roteia GPU
  IRQ ao Core 0 (não é um mask global), que o slot IRQ SPx original faz parte
  da entrega `INT.ARM=6`, e que o patch `0007` separou JIT e roteamento físico.
- 2026-07-15: branch Bluetooth classificada para reuso seletivo. Ring, reactor,
  state machine, telemetria e gate de offsets dos vetores são aproveitáveis;
  FIQ para PL011, substituição dos vetores e mask preventivo de PMU/timers não
  são compatíveis com a rebaseline.
- 2026-07-15: topologia provisória registrada: Emu68/Core 0, Rigel/Core 2 e
  reactor físico/Core 3; Core 1 permanece auxiliar até haver necessidade
  medida.
- 2026-07-15: primeiro baseline Core0/fault implementado com rollback por flags.
  O caminho fault remove do `MainLoop` os hooks da API explícita, restaura STOP
  com `WFE`, preserva `INT.ARM` e entrega o IPL Rigel ao contexto nativo.
- 2026-07-15: build cruzado e unit tests passaram; QEMU/DiagROM alcançou OVL,
  UDS/LDS e teste de Chip RAM com Core0=Emu68, Core2=Rigel e Core3=IO.
- 2026-07-15: smoke AROS 1 MiB de 60 s alcançou JIT, OVL e `FNOP`/`FSAVE`;
  mantido como resultado parcial, não como boot ou validação de FPU.
- 2026-07-15: validação em hardware real explicitamente retirada do escopo
  atual pelo usuário; continuar somente com build, testes e QEMU.
- 2026-07-15: implementado primeiro protótipo de Bluetooth por IRQ normal.
  UART0 é discriminada dentro do slot SPx e desviada para trampoline completo;
  demais IRQs mantêm a semântica Emu68 e FIQ permanece intocada.
- 2026-07-15: consolidado ownership sem handoff: miniUART=log desde o início,
  PL011=Bluetooth desde o início. QEMU/DiagROM repetido pela segunda UART.
