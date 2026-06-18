# Proposta de Arquitetura Multicore para Bellatrix

## Status: mapeamento de cores implementado

O mapeamento físico de cores descrito em "Visão Geral" (Core 0 = Machine/Host,
Core 1 = CPU, Core 2 = Rigel, Core 3 = IO assíncrono) está implementado:

* `emu68/src/aarch64/start.c` — `bellatrix_init()` roda no boot core (Core 0),
  que em seguida chama `bellatrix_launch_cpu_and_park()`: em multicore, lança
  o backend de CPU (Emu68 `M68K_StartEmu`/Musashi) no Core 1 via
  `PAL_Core_LaunchCpu()` e estaciona o Core 0 num loop `wfe` leve; em
  single-core, o backend continua rodando inline no boot core, sem alteração
  de comportamento.
* `src/host/raspi3/pal_core.c` — `bellatrix_core1_entry()` agora espera o
  entry point de CPU; `bellatrix_core2_entry()` (antes um esqueleto morto de
  "Audio/Paula" de uma arquitetura pré-Rigel) agora roda
  `chipset_core_loop()` (Rigel). Core 3 (IO) não mudou.
* O protocolo CPU↔chipset (`bellatrix_runtime_publish_cpu_cycles()` /
  `bellatrix_runtime_host_step()` em `src/runtime/core_chipset.c`) já era
  baseado em atomics + WFE/SEV, agnóstico a qual core físico chama cada lado —
  não precisou ser reescrito, só passou a ser invocado pelos cores corretos.
* Lacuna de correção fechada nesta mudança: o lock que protege o estado do
  chipset contra acesso concorrente da CPU só existia no caminho de fault do
  Emu68 (`bellatrix_bus_access()`); o backend Musashi chamava
  `bellatrix_bridge_cpu_read/write()` direto, sem lock. Isso não importava
  enquanto CPU e chipset nunca rodavam de fato em cores físicos diferentes
  para Musashi. Agora o lock (`core_chipset_lock_acquire/release()`, definido
  em `src/runtime/core_chipset.c`) protege as duas chamadas de
  `bellatrix_bridge_cpu_read/write()` em `src/cpu/cpu_bridge.c`, cobrindo
  ambos os backends uniformemente.

As lacunas abaixo (barreira temporal de MMIO crítico, scheduler por deadline,
fila de comandos Core 1↔Core 2) **continuam abertas** — a análise original
permanece válida como registro do que falta além do mapeamento de cores.

## Objetivo

Definir uma arquitetura multicore para o Bellatrix que:

* preserve a precisão temporal necessária para a emulação Amiga;
* maximize o desempenho do backend Emu68;
* mantenha o Rigel determinístico;
* minimize locks e sincronizações frequentes;
* permita crescimento futuro sem reestruturações profundas.

O objetivo principal não é paralelizar tudo, mas sim separar claramente os domínios de responsabilidade do sistema.

---

# Visão Geral

Proposta para sistemas ARM com 4 núcleos:

```text
Core 0  -> Machine / Scheduler Global / Host
Core 1  -> CPU 68K (Emu68)
Core 2  -> Chipset (Rigel)
Core 3  -> Serviços Assíncronos
```

Cada core possui um domínio de responsabilidade bem definido.

---

# Core 0 — Machine / Host

## Responsabilidade

O Core 0 é o proprietário do estado global da máquina.

Ele não deve executar workloads pesados continuamente.

Sua função principal é coordenar os demais núcleos.

---

## Componentes

### Machine

```text
machine.c
machine_state.c
```

Responsável pelo estado global do sistema.

---

### SysBus

```text
sysbus.c
```

Roteamento de dispositivos.

---

### MMIO Dispatch

```text
bellatrix_bridge.c
```

Integração entre CPU e chipset.

---

### Page Fault Handler

Integração com Emu68.

Responsável por:

```text
MMIO
trap handlers
memory mirrors
```

---

### IRQ Aggregation

Recebe sinais de:

```text
Rigel
expansions
devices
```

e produz o IPL observado pelo 68K.

---

### Boot

```text
autoconfig
reset
power-on
```

---

### Temporal Scheduler

Responsável pela coordenação entre:

```text
Core 1
Core 2
Core 3
```

---

## O que NÃO deve ficar no Core 0

Evitar:

```text
USB polling
Bluetooth polling
SD polling
renderização
áudio HDMI
logs pesados
```

O Core 0 deve permanecer leve.

---

# Core 1 — CPU 68K

## Responsabilidade

Execução do código Amiga.

Backend principal:

```text
Emu68
```

Backend alternativo:

```text
Musashi
```

---

## Função

Executar o máximo possível sem bloqueios.

---

## Comunicação

Somente através de:

```text
chip ram
fast ram
MMIO
IRQ state
```

---

## Regras

O Core 1 não deve:

```text
acessar hardware ARM diretamente
executar USB
executar Bluetooth
executar SD
```

Toda interação deve passar pela Machine.

---

# Core 2 — Rigel

## Responsabilidade

Execução completa do chipset Amiga.

---

## Componentes

### Agnus

```text
DMA
Blitter
Copper
Beam
```

### Denise

```text
bitplanes
sprites
playfields
```

### Paula

```text
audio
disk
serial
interrupts
```

### CIAs

```text
timers
keyboard
ports
```

---

## Regra Fundamental

O Rigel deve continuar single-thread internamente.

Não dividir:

```text
Agnus
Denise
Paula
CIA
```

em múltiplos cores.

O custo de sincronização provavelmente será maior que o ganho.

---

# Avaliação Contra a Implementação Atual

## Conclusão curta

A implementação atual não corresponde integralmente a esta proposta. Ela é um
estágio intermediário com alguns blocos úteis já presentes, mas ainda sem o
modelo de coordenação temporal descrito aqui.

Veredito:

```text
A proposta é superior como arquitetura final.
A implementação atual é superior apenas como etapa intermediária de baixo risco.
```

Motivo: a proposta separa CPU, Machine/Scheduler, Rigel e IO em domínios mais
limpos, o que dá uma base melhor para performance multicore real e
determinismo. A implementação atual é mais simples de validar agora, mas mantém
CPU, Machine, MMIO e coordenação no Core 0, portanto conserva o principal
gargalo estrutural.

O desenho atual é:

```text
Core 0  -> Emu68 / Machine / MMIO / boot / scheduler implícito
Core 1  -> Rigel via bellatrix_runtime_host_step()
Core 2  -> loop de áudio existe, mas fica parado se PAL_Core_LaunchAudio()
           não for chamado
Core 3  -> IO físico parcial: USB e Bluetooth
```

A proposta recomenda:

```text
Core 0  -> Machine / Scheduler Global / Host
Core 1  -> CPU 68K (Emu68)
Core 2  -> Chipset (Rigel)
Core 3  -> Serviços assíncronos
```

Portanto, a implementação atual moveu o Rigel para fora do core da CPU, mas
ainda mantém CPU, Machine, MMIO e coordenação global juntos no Core 0.

## Matriz de aderência

| Tema | Proposta | Implementação atual | Avaliação |
|------|----------|---------------------|-----------|
| Core 0 | Machine, scheduler global, MMIO, IRQ aggregation, boot | Core 0 faz boot + `bellatrix_init()`, lança Core 1/2/3, estaciona em `wfe` | **Aderente** (mapeamento de core resolvido; barreira temporal de MMIO crítico ainda não) |
| Core 1 | CPU 68K | CPU (Emu68 JIT ou Musashi) | **Aderente** |
| Core 2 | Rigel completo, single-thread | Rigel completo via `chipset_core_loop()`/`bellatrix_runtime_host_step()` | **Aderente** |
| Core 3 | USB, Bluetooth, SD, FAT32, áudio HDMI, vídeo, logging | USB e Bluetooth; serial ainda co-localizado com chipset | Parcial |
| Rigel single-thread | Sim | Sim | Aderente |
| Comunicação lock-free | Filas/eventos preferenciais | `atomic_fetch_add`, `SEV/WFE`, lock de acesso em `core_chipset.c` (agora cobre Emu68 e Musashi) | Parcial |
| Scheduler por deadline | `T = min(eventos...)` | Target acumulado `s_cpu_cck_target`; quantum fixo `CHIPSET_QUANTUM=128` | Ausente |
| MMIO crítico | `rigel_step_until(cpu_time)` antes de ler/escrever | MMIO chama bridge diretamente; Core 2 drena em paralelo | Lacuna crítica |
| IRQ | Estado compartilhado consultado em pontos seguros | IPL publicado via máquina/PAL, mas ainda sem contrato de checkpoint JIT completo | Parcial |
| Core 0 leve | Evitar polling/IO/log pesado | Core 0 só faz boot e estaciona em `wfe`; nenhum trabalho recorrente | **Aderente** |

## Pontos positivos já presentes

1. Rigel continua single-thread internamente.
2. Existe um caminho de publicação CPU->chipset:

```text
bellatrix_runtime_publish_cpu_cycles()
  -> s_cpu_cck_target
  -> PAL_Runtime_WakeupChipset()
  -> bellatrix_runtime_host_step()
```

3. Core 3 já pode retirar USB/Bluetooth do caminho single-core.
4. `PAL_Runtime_Poll()` foi removido do hot path multicore do Emu68.
5. O profiling agora consegue medir se o Core 1 está realmente drenando o
   target publicado.

## Lacunas críticas

### 1. Mapeamento de cores diferente da proposta

A proposta separa Machine/Scheduler de CPU. A implementação atual ainda roda
Emu68 no Core 0 junto com Machine/MMIO. Isso reduz o benefício esperado porque
o Core 0 continua sendo o core mais carregado.

Isso não impede um ganho inicial, mas significa que a implementação atual deve
ser tratada como:

```text
Fase atual: CPU+Machine no Core 0, Rigel no Core 1
Fase proposta: Machine no Core 0, CPU no Core 1, Rigel no Core 2
```

### 2. MMIO não reconcilia tempo antes do acesso

A proposta exige:

```c
rigel_step_until(cpu_time);
aplicar_escrita();
```

ou, para leituras:

```c
rigel_step_until(cpu_time);
retornar_estado();
```

O código atual publica ciclos para o Core 1 e o Core 1 drena até
`s_cpu_cck_target`, mas `bellatrix_bus_access()` não força o Rigel a alcançar o
tempo da CPU antes de aplicar MMIO crítico. Isso pode causar leitura de estado
atrasado ou escrita aplicada fora da ordem temporal esperada.

Essa é a principal divergência funcional contra a proposta.

### 3. Lock global não protege o avanço do Core 1

O comentário em `bellatrix.c` diz que o lock impede concorrência entre MMIO e
`bellatrix_machine_advance()`, mas o avanço multicore real ocorre em
`core_chipset.c` via `rigel_step()`. Esse arquivo não adquire o mesmo lock.

Resultado: no desenho atual, o profiling pode medir `lock_wait`, mas esse lock
não prova sozinho que MMIO e `rigel_step()` estão serializados.

Antes de otimizar performance, é preciso decidir uma destas abordagens:

```text
opção A: Core 0 envia comando MMIO ao Core 1, e só o Core 1 toca Rigel
opção B: Core 0 força catch-up e entra em seção crítica real compartilhada
opção C: separar registradores com snapshot/atomics e restringir MMIO direto
```

A opção A é a mais alinhada com a proposta e com determinismo.

### 4. Scheduler temporal ainda é acumulador, não deadline-oriented

O atual:

```text
CPU publica ciclos acumulados
Core 1 roda Rigel em blocos de até 128 CCK
```

A proposta:

```text
T = min(próximo evento do chipset,
        próximo IRQ previsto,
        próximo polling do JIT,
        próximo MMIO crítico,
        limite máximo)
```

O acumulador é simples e útil para medir, mas não evita excesso de wakeups nem
garante que o próximo ponto de reconciliação seja semanticamente correto.

### 5. Core 3 ainda não cobre todos os serviços assíncronos

Hoje o Core 3 cobre principalmente:

```text
USB
Bluetooth
```

Ainda não está claro/implementado para:

```text
SD/FAT32
cache de armazenamento
HDMI audio
video present
logging pesado
```

Além disso, serial host ainda é polled via `bellatrix_machine_post_chipset_step()`
no Core 1, para ficar perto do estado serial do Rigel. Isso pode ser correto no
curto prazo, mas difere da proposta de isolar serviços assíncronos.

## Melhor resposta multicore para o próximo passo

A melhor resposta não é mover mais subsistemas imediatamente. O próximo passo
deve fechar o contrato temporal CPU/MMIO/Rigel.

Ordem recomendada:

1. Manter Rigel single-thread no Core 1 por enquanto, apesar da proposta final
   chamá-lo de Core 2. Renomear depois é menos importante que acertar o contrato.
2. Fazer MMIO crítico passar por uma barreira de catch-up:

```text
CPU publica target
CPU solicita MMIO
Core 1 drena Rigel até target
Core 1 aplica leitura/escrita ou libera acesso serializado
CPU recebe resultado
```

3. Medir o custo dessa barreira com o profiling multicore.
4. Só então avaliar mover CPU para Core 1 e Machine/Scheduler para Core 0, como
   na proposta final.
5. Expandir Core 3 depois que o caminho determinístico estiver estável.

## O que o profiling deve provar

O profiling multicore deve responder:

| Pergunta | Métrica |
|----------|---------|
| A CPU está publicando progresso em granularidade aceitável? | `publish_calls`, `avg_m68k`, `avg_cck` |
| O Core 1 está drenando Rigel? | `chipset_steps`, `chipset_cck_total` |
| O chipset fica atrasado? | `backlog_avg`, `backlog_max` |
| Wakeup custa mais que o trabalho realizado? | `publish_time`, `wakeups`, `avg_step_cck` |
| MMIO cria contenção relevante? | `lock_wait`, `total_access` |
| O contrato temporal está sendo respeitado? | precisa de métrica de catch-up/MMIO crítico |

Falta ainda uma métrica específica para:

```text
mmio_catchup_time
mmio_catchup_wait
critical_mmio_count
critical_mmio_backlog
```

Essas métricas são necessárias para comparar a proposta completa, porque
`publish_time` e `chipset_step_time` medem throughput, mas não provam correção
temporal no ponto exato do MMIO.

---

# Core 3 — Serviços Assíncronos

## Responsabilidade

Executar tarefas não determinísticas.

---

## Exemplos

### Entrada

```text
USB HID
Bluetooth HID
Gamepads
Mouse
Teclado
```

---

### Armazenamento

```text
SD Card
FAT32
Cache de leitura
ISO cache
```

---

### Áudio

```text
HDMI Audio
buffer submission
```

---

### Vídeo

```text
frame presentation
scanout
double buffering
```

---

### Debug

```text
serial log
tracing
profiling
```

---

# Comunicação Entre Cores

A comunicação deve ocorrer preferencialmente através de filas lock-free.

Exemplos:

```text
input_queue
audio_queue
storage_queue
irq_queue
event_queue
```

Evitar locks globais.

---

# Modelo Temporal

## Princípio

Não sincronizar a cada instrução.

Não sincronizar a cada acesso de memória.

Não sincronizar a cada DMA.

---

## Sincronização por Janela Temporal

Utilizar um tempo lógico compartilhado:

```text
T
```

O sistema avança em blocos.

---

## Exemplo

```text
CPU executa até T
Chipset executa até T
Machine reconcilia
Repete
```

---

# Deadline-Oriented Scheduling

A janela T não deve ser fixa.

Ela deve ser calculada dinamicamente.

---

## Fórmula Conceitual

```text
T = min(
    próximo evento do chipset,
    próximo IRQ previsto,
    próximo polling do JIT,
    próximo MMIO crítico,
    limite máximo configurado
)
```

---

# MMIO Crítico

Existem registradores que alteram imediatamente o comportamento do chipset.

Exemplos:

```text
BLTSIZE
COPJMP1
COPJMP2
DMACON
INTENA
INTREQ
BPLCON0
DDFSTRT
DDFSTOP
```

---

## Regra

Antes de aplicar a escrita:

```c
rigel_step_until(cpu_time);
```

Depois:

```c
aplicar_escrita();
```

---

## Objetivo

Garantir que o estado observado pelo 68K permaneça coerente.

---

# Leituras de Estado

Para registradores que representam estado dinâmico:

```text
DMACONR
INTREQR
VHPOSR
DSKBYTR
```

Antes da leitura:

```c
rigel_step_until(cpu_time);
```

Depois:

```c
retornar_estado();
```

---

# Tratamento de IRQ

## Não recomendado

Enviar IPI ARM a cada IRQ.

---

## Recomendado

O chipset atualiza estado compartilhado:

```text
INTREQ
INTENA
IPL
```

O JIT consulta esse estado em pontos seguros.

---

## Checkpoint Natural

Fim de bloco traduzido:

```c
if (shared->irq_pending)
{
    exit_tb();
}
```

---

# Papel do Core 0

O Core 0 é o árbitro do sistema.

Ele não deve competir com:

```text
CPU
Chipset
```

Seu papel é garantir:

```text
ordem temporal
integridade do sistema
coordenação global
```

---

# Papel do Core 3

O Core 3 é um executor de tarefas assíncronas.

Ele pode:

```text
atrasar
reordenar internamente
usar buffers
```

sem comprometer o determinismo da emulação.

Ele nunca deve modificar diretamente:

```text
estado do chipset
estado da CPU
chip RAM
IRQ Amiga
```

Toda alteração deve passar pela Machine.

---

# Resumo Final

A arquitetura recomendada é:

```text
Core 0
    Machine
    SysBus
    MMIO
    IRQ Aggregation
    Scheduler Global
    Boot/Reset

Core 1
    Emu68
    Musashi

Core 2
    Rigel
    Agnus
    Denise
    Paula
    CIA

Core 3
    USB
    Bluetooth
    SD
    FAT32
    HDMI Audio
    Video Present
    Logging
```

A sincronização deve ser baseada em **deadlines temporais e eventos**, nunca em locks frequentes ou sincronização por instrução.

O objetivo é que CPU e Chipset executem de forma amplamente independente, encontrando-se apenas nos pontos onde a precisão temporal realmente exige reconciliação.
