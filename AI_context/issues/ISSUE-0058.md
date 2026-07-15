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
  - AI_context/specs/SPEC-0001-cpu-memory-integration.md
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

- `zorro3.h` declara janela a partir de `0x40000000`, enquanto `memory.h`
  ainda declara `BELLATRIX_Z3_BASE=0x10000000`;
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

O protótipo atual ainda preserva byte a byte o fallback original para toda
fonte não-PL011. Isso foi correto para o primeiro A/B conservador, mas fica
agora marcado como dívida arquitetural: deve ser substituído por dispatch de
fontes conhecidas antes de habilitar outras IRQs físicas Bellatrix.

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
