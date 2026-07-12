---
id: ISSUE-0040
title: "Multicore: mover propriedade da UART fisica e drain do console para o Core 3"
status: superseded
priority: medium
type: architecture
owner: agent
created_at: 2026-07-10
updated_at: 2026-07-10
tags:
  - multicore
  - logging
  - serial
  - uart
  - io
related_files:
  - src/host/raspi3/console_log.c
  - src/host/raspi3/console_log.h
  - src/host/raspi3/pal_core.c
  - src/runtime/core_io.c
  - src/machine/machine_rigel_step.c
  - src/io/serial/uart_host.c
---

# Issue: Core 3 deve ser o unico proprietario da UART fisica

## Status: implemented; hardware validation pending

## Contexto

No runtime multicore, o Core 3 e o dominio de I/O fisico/assincrono (USB e
Bluetooth), mas a saida serial ainda e drenada pelo Core 2, dentro do caminho
do chipset:

```text
Core 2 / chipset_core_loop
  -> bellatrix_runtime_host_step
  -> bellatrix_machine_post_chipset_step
  -> machine_step_host_serial_rigel
     -> Paula TX -> uart_host
     -> console_log_drain -> mini-UART
```

Isso mistura duas responsabilidades: o Core 2 deve possuir o tempo e o estado
do Rigel, enquanto o Core 3 deve possuir perifericos ARM fisicos. Tambem faz o
scheduler do chipset gastar tempo esperando disponibilidade da FIFO UART.

Durante a investigacao do falso "reboot loop" multicore, o ring de console
usava `volatile` para `head`/`tail`, sem ordenacao release/acquire. Em AArch64,
o consumidor podia observar o novo `head` antes do payload correspondente e
ler bytes antigos de uma volta anterior do ring. O efeito observado foi replay
byte-a-byte de blocos inteiros de boot, NULs e texto corrompido, embora a
emulacao continuasse normalmente. A correcao imediata tornou os indices
atomicos, mas nao resolve a separacao de dominios descrita nesta issue.

## Arquitetura desejada

```text
Core 2 / Rigel
  -> retira bytes do Paula TX no instante correto do chipset
  -> publica os bytes numa fila SPSC Core2 -> Core3

Core 0/1/2/3
  -> kprintf
  -> publica linhas no ring de console multicore

Core 3 / I/O
  -> unico escritor da mini-UART/serial fisica
  -> drena primeiro a fila Paula TX
  -> usa a capacidade restante para o ring de console
```

O Core 3 nao deve acessar diretamente o estado interno do Rigel: o Core 2
continua responsavel por retirar o TX de Paula e somente transfere bytes por
uma fila. Isso preserva ownership e evita uma nova corrida sobre `g_rigel`.

## Requisitos

- Core 3 e o unico codigo que escreve na UART fisica durante o runtime.
- Core 2 nunca espera a FIFO UART e nao chama `console_log_drain()`.
- Paula TX tem prioridade sobre logs, preservando a politica atual.
- A fila Core2 -> Core3 tem contadores explicitos de overflow/drop.
- O ring de console permanece seguro para produtores em todos os quatro cores.
- O caminho single-core continua funcional sem exigir um Core 3.
- BTStack/PL011 e mini-UART mantem ownership separado conforme a configuracao.
- Nenhum acesso concorrente ao contexto Rigel e introduzido.

## Plano sugerido

1. Criar uma fila SPSC de bytes para Paula TX, produzida pelo Core 2 e
   consumida pelo Core 3.
2. Separar `machine_step_host_serial_rigel()` em coleta logica de Paula e
   escrita fisica no host.
3. Mover `console_log_drain()` e a escrita de ambas as filas para
   `core_io_step()`/loop do Core 3.
4. Manter um drain local equivalente no fallback single-core.
5. Instrumentar profundidade maxima e drops das duas filas.
6. Validar simultaneamente Paula serial, console intenso, USB HID e BT.

## Criterios de aceite

- Log multicore prolongado sem replay, NULs ou mistura de linhas.
- Paula TX chega integralmente e antes de logs pendentes quando as filas
  competem pela UART.
- `CORE0-SUP` mantem beats monotonicamente crescentes sob log intenso.
- Core 2 nao apresenta stall mensuravel causado pela UART.
- QEMU e Raspberry Pi 3 passam pelos mesmos testes de boot KS13/multicore.

## Relacionado

- `ISSUE-0012`: relacao entre `core_log` e RigelTrace.
- `ISSUE-0036`: estabilidade do console mini-UART durante boot/USB.
- `AI_context/consolidated/issue_multicore_boundary_logging.md`.

## Implementacao 2026-07-10

O ownership fisico foi movido para o Core 3:

- Core 2 retira TX de Paula e publica numa fila SPSC de 1024 bytes;
- Core 3 drena a fila para `machine->uart_host`, sem acessar `g_rigel`;
- RX fisico usa uma segunda fila SPSC Core3 -> Core2, onde o byte e injetado
  em Paula;
- Core 2 nao chama mais `console_log_drain()` no multicore;
- Core 3 atende Bluetooth e USB, depois Paula serial e por ultimo o console;
- se a UART estiver cheia, o byte TX permanece na fila para o proximo passo;
- overflow TX/RX e contado, assim como profundidade maxima das filas;
- o supervisor publica `uart_tx=depth/max drop=N` e o equivalente de RX;
- o fallback single-core conserva o caminho direto anterior e drena console
  localmente.

As filas separam as cache lines de `head`, `tail` e payload e usam
release/acquire. O produtor nunca espera o consumidor; fila cheia incrementa
`dropped` e a emulacao continua. Um `SEV` agregado pelo event register do ARM
acorda o Core 3 quando TX passa a ter trabalho, sem handshake bloqueante.

Validacao local:

- build bare-metal multicore Musashi 68000: passa;
- QEMU multicore KS1.3 + `Workbenc13.adf`: 402 frames, progresso monotonicamente
  crescente, backlog limitado, log limpo, `uart_tx/rx drop=0`;
- build bare-metal single-core Musashi 68000: passa;
- QEMU single-core KS1.3 + ADF: init e transicao OVL 1->0 passam.

Ainda requer Pi 3/serial real para validar simultaneamente Paula TX/RX, console
intenso, USB e Bluetooth e medir se Core 2 perdeu todo stall atribuivel a UART.
