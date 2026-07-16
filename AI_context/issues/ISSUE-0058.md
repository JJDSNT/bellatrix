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
blockers:
  - "validação em hardware real (Pi 3B) para P2/P3 — adiada por decisão do usuário até nova autorização"
related_files:
  - AI_context/specs/SPEC-0001-cpu-memory-integration.md
  - docs/authors_note.md
  - docs/uae_references.md
  - emu68/src/aarch64/start.c
  - emu68/src/aarch64/vectors.c
  - emu68/src/ExecutionLoop.c
  - patches/0007-bellatrix-boot-sequence.patch
  - src/cpu/emu68/bellatrix.c
  - src/cpu/cpu_bridge.c
  - src/runtime/core_io.c
  - src/runtime/topology.h
  - AI_context/consolidated/multicore_topology.md
  - AI_context/consolidated/emu68_routing_vs_synchronization.md
  - AI_context/issues/ISSUE-0051.md
  - AI_context/issues/ISSUE-0052.md
  - AI_context/issues/ISSUE-0057.md
  - AI_context/issues/ISSUE-0061.md
---

> **Atualização (2026-07-16, ISSUE-0061):** a "questão P1 aberta" da seção
> 3.1.1 abaixo (fonte de tempo do Rigel) foi respondida em parte: um refactor
> subsequente (a "public machine API" que esta issue já classificava como
> não-prioritária) tinha de fato deletado a chamada de
> `bellatrix_emu68_report_jit_progress()` do `MainLoop` e a excluído por
> `BELLATRIX_EMU68_FAULT_DRIVEN` — exatamente o erro que a seção 3.1.1 abaixo
> preveniu ("progress reports do CPU... nunca como única fonte de
> VBL/CIA/Paula/IPL"). A ISSUE-0061 restaurou a chamada incondicional
> (`patches/0003`) e confirmou empiricamente `frame_counter` avançando de 0
> até 500+ com IPL/interrupção real. Isso NÃO fecha a pergunta arquitetural
> mais ampla de 3.1.1 (CPU-progress-driven vs. timer-driven Rigel), que
> continua aberta — ver `docs/fault_handler.md`. Também não fecha os
> checklists P2/P3 abaixo (validação em hardware real permanece adiada por
> decisão explícita do usuário). Ver `AI_context/issues/ISSUE-0061.md` para o
> relato completo e `AI_context/consolidated/emu68_routing_vs_synchronization.md`
> para a lição conceitual extraída.

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
3. manter/restaurar o Emu68 no Core 0 como baseline provisória de estabilização,
   não como topologia final, até provar equivalência em outro core;
4. usar IRQ ARM normal para Bluetooth; não gastar FIQ com Bluetooth;
5. considerar FIQ para USB/DWC2/SOF somente se necessidade e medição futuras o
   justificarem;
6. usar `vectors.c` como ponto de entrada do barramento Emu68 para o chipset,
   convergindo num serviço comum que outros backends de CPU chamam diretamente.

`docs/authors_note.md` preserva a orientação recebida. `docs/uae_references.md` é
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

Esta tabela descreve uma arquitetura de estabilização. Ela reduz a quantidade
de premissas alteradas em relação ao Emu68 original, mas não decide onde a CPU
deverá permanecer no produto final. Nenhuma ABI de memória, MMIO, IRQ ou backend
pode depender de `Core 0`; placement é uma política de runtime posterior.

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
- STOP volta ao caminho nativo em vez de yield gerenciado. A variante
  Bellatrix agora seleciona o mesmo loop `WFE` com predicado agregado usado no
  PiStorm; Musashi também expõe seu estado STOP ao shell comum, que estaciona
  Core 0 em `WFE` até `SEV`;
- a entrega de interrupção combina `INT.ARM`/níveis 6-7 originais com o IPL
  publicado pelo Rigel, sem descartar a IRQ física;
- mudanças de IPL no Core 2 atualizam o `M68KState` nativo via `PAL_IPL_Set()` e
  emitem `SEV`, pois não existe mais um quantum gerenciado no Core 1 para puxar
  `s_pending_ipl`.

### Contrato por função, não por número de core

A topologia provisória deve atribuir explicitamente quatro funções: owner do
Emu68/JIT, owner único do Rigel, host reactor/presenter e core auxiliar. Código
comum não pode pressupor que "Core 0" significa supervisor ou presenter.

Esse requisito deixou de ser apenas preventivo em 2026-07-15: no rebaseline,
Core 2 publicava frames normalmente, mas o drain/presenter continuava dentro do
loop supervisor usado pelo Musashi. Como Core 0 estava ocupado pelo loop nativo
do Emu68, ninguém consumia `s_pending_frames` e o framebuffer permanecia no logo
Emu68. O drain passou a fazer parte de `bellatrix_runtime_io_step()`, isto é, do
contrato do host reactor. Com a topologia finalmente unificada, esse serviço
roda no Core 3 tanto com Musashi quanto com Emu68.

O contrato foi centralizado em `src/runtime/topology.h`: Emu68 e Musashi usam
o mesmo placement `CPU0/AUX1/Rigel2/HOST3`; código de launch, timeline e logs
deixa de repetir condicionais ou mapas locais. A matriz
canônica e seus invariantes estão em
`AI_context/consolidated/multicore_topology.md`.

O A/B QEMU confirmou a fronteira: antes da correção, o framebuffer ainda era o
logo Emu68 mesmo após OVL/Autoconfig/FPU no serial; depois, o framebuffer passou
a receber o frame claro produzido pelo Rigel. Isso prova o reparo da
apresentação, mas ainda não constitui boot visual da Kickstart.

### Destino da public machine API

A API explícita ampla não é mais uma direção arquitetural do Bellatrix. Seus
patches permanecem como evidência histórica e fonte seletiva de testes ou
contratos, mas nenhum trabalho novo deve depender dela. A direção vigente é o
Emu68 original, com Data Abort e `vectors.c` preservados, mais um hook Bellatrix
fino para bus/MMIO. Depois que esse desenho estiver completamente assimilado,
a API experimental poderá ser removida em vez de mantida como segundo runtime.

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
verifica offsets, dispatch físico sem injeção guest, contenção de fonte
desconhecida, contexto completo e FIQ intocada em todo build.

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
- QEMU com PL011 ligado a socket e resposta HCI Reset sintética: sete bytes
  atravessam IRQ 57, trampoline, ring e parser; o parser solicita o bloco
  seguinte e transmite `Read Local Version`. Como o polling não drena enquanto
  a rota está armada, esse resultado exercita efetivamente o novo top-half;
- QEMU `raspi3b` + AROS 1 MiB por 60 s: alcança JIT, OVL e as instruções
  `FNOP`/`FSAVE`; ainda não é boot AROS nem gate de FPU concluído;
- o log confirma `core=0`, `native fault-driven execution selected`, Core 2 a
  244140 Hz e Core 3 a 953 Hz;
- imagem do gate: SHA-256
  `d055e3f1b1078d750109dcdc54b2a29cb5b76f4f944af29fc50aa9d5548c8206`.

## Gate da topologia unificada

Em 2026-07-15 o placement deixou de variar com o backend. Emu68 e Musashi
68040 foram compilados e executados em QEMU com o mesmo banner efetivo:
`irq=0 cpu=0 chipset=2 host=3 aux=1`. Nos dois casos Core 2 iniciou seu event
stream e Core 3 iniciou o reactor a ~953 Hz; Core 1 permaneceu auxiliar. O
smoke sem ROM valida bootstrap/placement, não boot do guest.

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

## 3.1. `vectors.c` como hook oficial Emu68 -> Bellatrix

Para o Emu68, `vectors.c` é o ponto oficial de integração com a arquitetura
Bellatrix. O hook substitui o destino PiStorm do fault handler pelo serviço
síncrono do barramento emulado, sem substituir o mecanismo de fault nem mover
o chipset para dentro do vetor:

```text
Emu68/JIT em página externa
  -> Data Abort -> vectors.c -----------+
                                         |
Musashi/outro backend                    +-> bellatrix_external_bus_access()
  -> classificação explícita ----------+       -> Rigel/chipset owner
```

O hook é a fronteira comum de transação externa. `vectors.c` é seu adaptador
específico para Emu68; Musashi não deve simular um Data Abort nem depender da
vector table ARM, chamando diretamente a implementação final do mesmo hook.
Rigel permanece dono do estado de Agnus, Paula, Denise, CIA e demais
dispositivos; o hook não duplica esse estado e não transforma `vectors.c` num
chipset.

A fronteira é usada somente para acessos que exigem semântica externa ou efeito
colateral. RAM/ROM já mapeada não atravessa o serviço por byte/palavra.

A forma de manutenção desejada não é continuar acrescentando blocos
`#elif BELLATRIX` extensos ao arquivo. Deve-se preparar uma alteração mínima e
upstreamável que faça `vectors.c` expor/chamar uma interface de plataforma. O
PiStorm permanece um adapter dessa interface; Bellatrix implementa Rigel em
arquivo próprio; Musashi usa diretamente a implementação Bellatrix. Símbolos
diretos resolvidos no link são preferíveis a function pointers/registries no
hot path.

### Contrato de custo do hook

O caminho normal não deve adicionar uma API genérica pesada sobre o handler:

- uma chamada direta e com ABI pequena a partir de `SYSReadValFromAddr()` ou
  `SYSWriteValToAddr()`;
- classificação curta/especializada para as regiões que realmente faultam;
- nenhum allocator, logger, BT/USB poll, scheduler ou dispatch de dispositivo
  físico no hot path;
- nenhum rendezvous cross-core para reads publicados ou writes seguramente
  postáveis; sincronização permanece apenas onde a semântica observável exige;
- tracing e profiling compilados fora do produto normal;
- nenhuma passagem de Chip/Fast/Z3 RAM pelo hook.

O código atual já possui a ligação direta `vectors.c -> bellatrix_bus_access`,
mas ainda precisa ser afinado antes de virar contrato estável: faz poll de
runtime no entry, normalização em mais de uma camada, atravessa
`bellatrix_bus_access -> cpu_bridge -> machine_dispatch` e pode tomar lock ou
esperar o Core 2 em acessos críticos. Alguns desses custos são instrumentação
ou correção temporal legítima; a tarefa é medi-los e especializar o caminho,
não remover sincronização por suposição.

`bellatrix_emu68_report_jit_progress()` não é parte conhecida do contrato
original. Foi introduzida no refactor multicore Bellatrix e modificada depois
para tentar manter o chipset vivo. No baseline fault-driven atual, o report
periódico do `ExecutionLoop` é compilado fora e o chamado restante ocorre
dentro de `vectors.c`, portanto somente quando já houve Data Abort. Não há prova
de que esse mecanismo tenha produzido um clock funcional do Rigel. Ele deve ser
tratado como experimento substituível, não requisito do fault handler.

## 3.1.1. Fonte de tempo do Rigel — questão P1 aberta

O número de faults não pode ser a fonte fundamental de tempo: um loop em RAM ou
STOP pode não tocar MMIO e ainda assim VBL, CIA, Paula e IPL precisam avançar.
A orientação de scheduler mínimo + timer torna prioritário avaliar um Rigel
dirigido por timer em seu core de serviço, independente do hot path do Emu68.

O fault hook então precisa, no máximo, sincronizar/capturar o estado até o
instante observável da transação; não deve dirigir todo o chipset por efeito
colateral. Antes de mudar código, comparar os modos `CPU_DRIVEN`, `REALTIME` e
`HYBRID` já presentes no runtime, identificar por que os baselines anteriores
funcionaram somente pelo Data Abort e separar:

- relógio contínuo do chipset;
- ordenação de MMIO no instante do acesso;
- entrega de IPL/IRQ ao Emu68;
- pacing de frame/áudio e serviços físicos.

### Resultado da auditoria do runtime atual

A observação do usuário foi confirmada pelo código. `bellatrix_core0_supervise()`
é quem inicializa e atualiza a timeline `REALTIME`/`HYBRID`, porém esse loop
pertence à topologia anterior Core0=supervisor/Core1=CPU e não executa quando o
baseline coloca `M68K_StartEmu()` diretamente no Core 0.

O Core 2 possui event stream de aproximadamente 250 kHz, mas
`bellatrix_runtime_host_step()` só pode avançar até `s_chipset_horizon`. No
rebaseline Core 0 esse horizonte começa em zero e, em `CPU_DRIVEN`, cresce
somente quando o CPU publica ciclos. Como o report periódico do `ExecutionLoop`
é compilado fora em modo fault-driven, a publicação restante ocorre no hook de
`vectors.c`, depois de um Data Abort. Assim:

```text
timer acorda Core 2
  -> horizonte continua zero: não avança

Data Abort
  -> report Bellatrix publica delta
  -> horizonte cresce
  -> Core 2 finalmente avança Rigel
```

Esse comportamento não é requisito do Emu68; é uma dependência residual da
topologia abandonada. A direção conservadora é:

1. manter Emu68/fault handler no Core 0;
2. dar ao Rigel uma timeline baseada em timer que continue sem faults;
3. manter o hook de `vectors.c` apenas como integração/sincronização da
   transação MMIO;
4. tratar progress reports do CPU como telemetria, pacing opcional ou limite de
   política — nunca como única fonte de VBL/CIA/Paula/IPL;
5. decidir o owner da timeline entre Core 2 e um scheduler mínimo de serviço
   somente depois de verificar custo e races. O timer de Core 2 já existe; o
   Core 3 já hospeda o reactor BT/USB e é a outra opção natural.

## 3.2. Matriz provisória do mapa de memória

A leitura do Emu68 original estabelece uma distinção mais precisa que
"24 bits versus 32 bits": páginas de memória podem ser diretas mesmo dentro do
espaço baixo; páginas externas permanecem ausentes/protegidas para causar o
fault.

| Região após configuração | Caminho Emu68 | Caminho Musashi | Owner/serviço |
|---|---|---|---|
| Chip RAM e Fast RAM Z2 | `mmu_map`, load/store ARM | buffer direto | memória da máquina |
| ROM/overlay | `mmu_map`/remapeamento | buffer/bank direto | memória da máquina |
| custom, CIA e MMIO Amiga baixo | Data Abort + `vectors.c` | chamada explícita | Rigel |
| janela `$E80000` de Autoconfig | Data Abort + `vectors.c` | chamada explícita | sequenciador Zorro |
| RAM, ROM e VRAM Z3 configuradas | `mmu_map`, load/store ARM | região 32-bit direta | memória/board |
| registradores Z3 com efeitos colaterais | política explícita por board; não presumir RAM | chamada explícita | dispositivo/board |
| endereço não mapeado | open bus/bus error conforme perfil | mesma semântica comum | política da máquina |

No Emu68 original, a escrita final de Autoconfig chama `board->map(board)` e
as placas Z3 fornecidas chamam `mmu_map()` para expor diretamente sua ROM ou
memória. Consequentemente, uma Z3 configurada normalmente não chega a
`vectors.c`. O tratamento original de faults acima de `0x00ffffff` como
open bus é evidência desse contrato: trata um fault inesperado, não implementa
o datapath normal da Z3.

Dispositivos Z3 não podem ser generalizados todos como RAM. Uma janela
puramente armazenável deve ser direta; páginas de registradores com efeitos
colaterais precisam de uma política por board (fault fino no Emu68 e serviço
comum, ou outra representação explicitamente demonstrada). A decisão deve ser
feita no momento do mapeamento da placa, não por uma máscara global de endereço.

## 3.3. Estado real do suporte Z3 Bellatrix

Bellatrix ainda não possui suporte Z3 funcional. O código existente é
infraestrutura parcial e contraditória, não um contrato a preservar:

- a constante sem uso de `0x10000000` foi removida, mas Emu68 não fixa uma
  faixa de board: aceita o high word escrito em `$E80044` e chama `map()`;
  `0x40000000..0x7fffffff` é apenas política observada no AROS local;
- `memory_map_decode()` reduz o endereço a 24 bits antes da classificação;
- `cpu_bridge.c` rejeita todo endereço acima de `0x00ffffff` como open bus;
- a state machine Z3 atual oferece callbacks byte a byte, mas não instala o
  mapeamento direto ao concluir Autoconfig;
- não há board Z3 funcional registrada pelo runtime atual.

Portanto ISSUE-0032 deve primeiro definir o contrato de mapping/unmapping de
boards e separar espaços de endereço, antes de completar a state machine ou
adicionar Fast RAM/RTG. Não se deve simplesmente retirar a máscara de 24 bits:
ela ainda expressa wrap válido do perfil 68000/barramento baixo, mas está no
nível errado para um decoder universal de 32 bits.

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

## 4.1. Housekeeper PiStorm versus IRQ/FIQ física

A auditoria inicial do protocolo original mostra dois canais distintos:

```text
Core 2: ps_housekeeper()
  -> event stream + WFE
  -> polling dos pinos IPL do Amiga
  -> filtro (PiStorm32: duas amostras iguais)
  -> M68KState.INT.IPL
  -> SEV quando IPL != 0

Core 0: slot IRQ/FIQ de vectors.c
  -> INT_shadow.ARMPending
  -> M68KState.INT.ARM
  -> EXTER / nível 6 quando INTENA permite
```

O housekeeper não é um handler IRQ ARM: ele é polling periódico em core
auxiliar. No PiStorm clássico lê `PIN_IPL_ZERO`; no PiStorm32 lê os três bits
IPL e filtra skew entre amostras.

### Esclarecimento do autor do PiStorm/Emu68

Segundo a resposta encaminhada pelo usuário em 2026-07-15, o Emu68 não possui
uma ISR de dispositivo própria. O housekeeper é um loop que encaminha as
mudanças de IRQ/IPL do Amiga para o lado 68k. O canal ARM -> AmigaOS deve ser
usado somente quando existe algo no lado ARM que precisa ser deliberadamente
propagado ao lado AmigaOS.

Isso fixa três contratos separados:

```text
Rigel muda IPL do Amiga
  -> publica M68KState.INT.IPL
  -> SEV/wakeup do Emu68

PL011/USB/outro periférico do host
  -> ISR/top-half da plataforma ARM
  -> serviço ARM deferido
  -> não altera IPL/INT.ARM do guest por padrão

serviço ARM que precisa notificar AmigaOS
  -> injeção explícita no canal INT.ARM/EXTER
  -> somente por contrato conhecido com o software guest
```

No Bellatrix, a publicação de IPL do Rigel é o equivalente arquitetural do
housekeeper PiStorm. Ela deve observar o nível persistente do chipset, não ser
modelada como um pulso IRQ ARM.

Ainda está aberta a origem concreta que dispara o slot IRQ/FIQ físico no
produto PiStorm. O `start.c` roteia GPU IRQ, PMU e timers ao Core 0, mas:

- rotear uma classe de interrupção não habilita por si uma fonte;
- PMU interrupt é programada somente no modo diagnóstico `debug_cnt`;
- não foi encontrada nessa primeira passagem a configuração de GPIO edge IRQ
  ou timer compare que identifique o produtor normal de `INT.ARM`;
- `INT2_ENABLED` aparece definido no protocolo clássico, mas sem consumidor no
  HEAD analisado.

O histórico Git dá contexto adicional: o campo/caminho que se tornou
`INT.ARM` nasceu no commit `6275529` como suporte genérico a interrupções
assíncronas AArch64, antes do protocolo PiStorm atual. Os commits posteriores
mapearam IRQ/FIQ ARM para uma interrupção 68k e `37dd583` introduziu as sombras
`INTENA`/`INTREQ` para apresentá-la como `EXTER`. Logo, esse canal não é por
origem "a IRQ do housekeeper"; ele converte uma exceção ARM que chegou ao core
do JIT. A fonte física concreta continua sendo responsabilidade da plataforma.

Consequentemente, preservar o mecanismo disponível continua conservador, mas
o fallback que converte toda IRQ física não reconhecida em `INT.ARM` não é uma
política Bellatrix válida. O próximo passo é traçar produtor, acknowledge e
mask/unmask de cada fonte física Bellatrix e expor uma injeção guest separada e
explícita para futuros serviços ARM que realmente precisem notificar AmigaOS.

Para Bellatrix, o dispatcher deve decidir antes da injeção:

- PL011 Bluetooth: reconhecer/consumir como IRQ física do host, sem escrever
  `INT.ARM`;
- Rigel: publicar o nível Amiga persistente em `INT.IPL`, como o housekeeper;
- fontes ARM explicitamente destinadas ao guest: usar a semântica Emu68 de
  `INT.ARM`/EXTER somente por solicitação explícita;
- fontes desconhecidas: conter e diagnosticar, nunca fabricar `EXTER` por
  padrão sem identificar acknowledge e ownership.

Implementado no commit `0adc636`: no build Bellatrix, o slot IRQ SPx envia
UART0 ao top-half Bluetooth e envia qualquer outra fonte ao caminho de
contenção. A fonte desconhecida é contada, retorna com IRQ ARM mascarada para
evitar uma tempestade de nível e nunca escreve `INT.ARM`. O caminho original
Emu68 continua disponível somente em builds não-Bellatrix. FIQ permanece
inalterada e sem fonte habilitada; quando houver candidato USB/FIQ, deverá
ganhar dispatcher físico próprio em vez de injeção guest implícita.

O contador `bellatrix_physical_unknown_irq_count()` aparece na telemetria de
bootstrap/scan Bluetooth. Atualmente UART0/GPU IRQ 57 é a única fonte GPU IRQ
explicitamente habilitada pelo Bellatrix, portanto valor não zero é falha de
configuração ou nova fonte ainda sem ownership.

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
- [x] Tornar o host reactor owner explícito do drain/presenter, independente do
  core ocupado pelo backend de CPU.
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
- [x] Remover fallback Bellatrix que convertia IRQ física desconhecida em
  `INT.ARM`/EXTER; conter e contar a fonte sem tocar no guest.
- [ ] Provar ausência de corrupção de contexto JIT, IRQ Amiga espúria, perda de
  byte UART, regressão de boot e starvation.
- [x] Reaproveitar seletivamente ring SPSC, budgets e defer da branch
  Bluetooth, sem trazer FIQ nem a substituição dos vetores.
- [x] Incorporar o roteador HID comum USB/BT e consumir F11/F12 somente no
  host, sem gerar rawkey, botão ou IPL no Amiga.
- [x] Executar os modais F11/F12 no reactor Core 3; Core 0 permanece dedicado
  ao Emu68 e o handler físico continua sendo apenas um top half limitado.
- [x] Preservar mini-UART como log desde o boot e PL011 como UART exclusiva do
  Bluetooth, sem handoff entre elas.
- [ ] Validar os modais e a reconexão em hardware somente quando houver nova
  autorização explícita.

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
- toda IRQ física ARM deve ser convertida em `INT.ARM`/EXTER;
- o housekeeper PiStorm é uma ISR ARM;
- o roteamento original ao Core 0 é mera herança descartável;
- o fault handler deve ser substituído pela API pública;
- a API pública é pré-requisito para scheduler/multicore correto.

Esses textos permanecem como histórico útil, mas ISSUE-0058 prevalece quando
houver conflito.

# Log de execução

- 2026-07-15: orientação externa preservada em `docs/authors_note.md`; referência
  UAE preservada em `docs/uae_references.md`.
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
- 2026-07-15: topologia provisória registrada por papéis: ingresso IRQ/Core 0,
  Emu68/Core 0, Rigel/Core 2 e host reactor/Core 3; Core 1 permanece auxiliar.
- 2026-07-15: primeiro baseline Core0/fault implementado com rollback por flags.
  O caminho fault remove do `MainLoop` os hooks da API explícita, restaura STOP
  pelo caminho nativo (`WFI` na variante Bellatrix), preserva `INT.ARM` e
  entrega o IPL Rigel ao contexto nativo. A diferença para o `WFE` PiStorm
  permanece um gate aberto.
- 2026-07-15: build cruzado e unit tests passaram; QEMU/DiagROM alcançou OVL,
  UDS/LDS e teste de Chip RAM com CPU/Core0, chipset/Core2 e host/Core3.
- 2026-07-15: smoke AROS 1 MiB de 60 s alcançou JIT, OVL e `FNOP`/`FSAVE`;
  mantido como resultado parcial, não como boot ou validação de FPU.
- 2026-07-15: validação em hardware real explicitamente retirada do escopo
  atual pelo usuário; continuar somente com build, testes e QEMU.
- 2026-07-15: implementado primeiro protótipo de Bluetooth por IRQ normal.
  UART0 é discriminada dentro do slot SPx e desviada para trampoline completo;
  nessa etapa, demais IRQs ainda mantinham a semântica Emu68 e FIQ permanecia
  intocada.
- 2026-07-15: após esclarecimento do autor, separado housekeeper/IPL Amiga de
  IRQs físicas ARM. Removido no Bellatrix o fallback não-PL011 para
  `INT.ARM=6`; IRQ desconhecida agora é contida, contada e nunca chega ao guest.
- 2026-07-15: consolidado ownership sem handoff: miniUART=log desde o início,
  PL011=Bluetooth desde o início. QEMU/DiagROM repetido pela segunda UART.
- 2026-07-15: explicitado que Emu68/Core 0 é somente a baseline conservadora de
  estabilização. A topologia final permanece aberta e deverá ser decidida por
  equivalência funcional e medições, sem contaminar o contrato de integração.
- 2026-07-15: trabalho multicore congelado no estado provisório acima por
  decisão do usuário. Próxima sessão multicore retomará seus gates; a prioridade
  imediata passou a ser fechar RAM/Autoconfig/memory map.
- 2026-07-15: auditadas as aperturas fixas em
  `consolidated/fixed_memory_apertures.md`. Uniformizados mirror de Chip RAM,
  writes sob overlay, fonte do overlay de ROM de 1 MiB, decoder esparso CIA e
  limite de 64 KiB do Autoconfig. CIA/custom/Autoconfig permanecem fault/MMIO;
  RAM/ROM permanecem diretas.
- 2026-07-15: revertida a tentativa comum de `is_stopped/WFE`. O laço passou a
  interpretar `run() == 0` como nenhum tempo transcorrido e podia dormir antes
  que o produtor correspondente avançasse Rigel até VBL/CIA/IPL. Isso quebrou
  o boot real; DiagROM continuando não era evidência suficiente e foi
  inicialmente interpretado de forma incorreta. Restaurado exatamente o
  contrato anterior: retorno zero representa um quantum idle de 454 ciclos;
  single-core avança Rigel sincronicamente e multicore publica esse quantum ao
  Core 2. Não há `WFE` no laço comum até existir um protocolo idle comprovado
  separadamente para Musashi e Emu68. A validação decisiva continua sendo o
  boot screen, não apenas OVL/DiagROM.
- 2026-07-15: retirada também a board experimental `emu68.68040` do default.
  Ela era inserida automaticamente depois da Z2 Fast RAM em todo perfil 68040;
  portanto `[Z2] all boards configured` era apenas o ponto de transição para
  a próxima resposta em `$E80000`, não prova de falha no backing Z2. Enquanto
  seu Autoconfig Z3 não alcançar boot screen, somente
  `BELLATRIX_Z3_68040=1` deve habilitá-la. O baseline normal volta a não expor
  essa board.
