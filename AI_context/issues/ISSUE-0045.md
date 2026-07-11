---
id: ISSUE-0045
title: "Plataforma multicore: owner único de I/O, IRQs físicas e integração com o árbitro"
status: doing
priority: high
type: refactor
owner: agent
created_at: 2026-07-10
updated_at: 2026-07-11
tags:
  - multicore
  - irq
  - usb
  - io
  - arbiter
  - launcher
related_files:
  - src/runtime/core_io.c
  - src/runtime/core_io.h
  - src/cpu/emu68/bellatrix.c
  - src/host/raspi3/pal_core.c
  - src/io/usb/usb_host.c
  - src/io/usb/usb_hid_bellatrix.c
  - src/launcher/launcher.c
  - src/launcher/launcher_input.c
---

# Plataforma multicore: owner único de I/O, IRQs físicas e integração com o árbitro

## Objetivo

Eliminar o handoff launcher/runtime e construir inicialmente esta topologia:

```text
Core 0  supervisor + I/O    — boot, IRQs, USB/BT/UART/HDMI e ordenação
Core 1  CPU 68k            — Emu68/Musashi, somente IPL emulado
Core 2  Rigel              — chipset determinístico e injeção emulada
Core 3  reservado           — futuro worker RTG/AHI ou serviço medido
```

Princípio: **IRQ física vai ao Core 0, owner dos periféricos; evento emulado
entra na máquina por filas/estado atômico e nunca pelo vector PiStorm**.
Afinidade ARM, ownership, wakeup inter-core e IPL 68k são contratos separados.

## Decisão arquitetural de 2026-07-11

O desenho anterior colocava I/O no Core 3. Ele foi deliberadamente substituído:
o custo e a complexidade de transportar todo I/O não se justificam enquanto
Core 0 está quase ocioso. Core 0 passa a ser owner permanente, e Core 3 fica
estacionado até RTG/AHI ou medições demonstrarem carga para um worker dedicado.

O lançamento do Core 3 permanece disponível como mecanismo futuro, mas não faz
parte da topologia ativa. As seções históricas abaixo que dizem "Core 3 = I/O"
descrevem o plano antigo e servem apenas como referência de extração futura.

## Motivação e baseline

`ISSUE-0044` comprovou corrupção quando Core 0 e Core 3 chamaram
`usb_host_step()` sobre a mesma stack. `launcher_owns_usb` corrige a corrida por
gating, mas conserva dois executores e um handoff implícito. O alvo remove essa
classe de erro por construção.

Restrições atuais:

- Emu68 possui vectors PiStorm-cêntricos que traduzem IRQ ARM em Amiga INT6;
- USB/BT funcionam por polling no Core 3;
- Core 0 ainda participa diretamente do USB durante o launcher;
- `launcher_input` não é atômico/cross-core;
- launcher usa MSC/FAT, portanto uma fila apenas de HID é insuficiente;
- waits síncronos CherryUSB podem bombear `USBH_IRQHandler()` e devem permanecer
  no mesmo owner da stack.

## Contratos alvo

### Ownership

- Somente Core 3 acessa DWC2, CherryUSB, URBs, endpoints e lifecycle USB.
- Somente Core 3 acessa periféricos físicos de BT/UART/HDMI após o cutover.
- Core 0 não executa bottom-half nem callbacks internos de drivers.
- Core 2 produz/consome dados emulados por filas; nunca toca hardware físico.
- Core 1 nunca recebe IRQ física de device.

### Comunicação

- Core3→Core0: SPSC de eventos/completions, com wakeup coalescido.
- Core0→Core3: fila de comandos/RPC para block I/O e controle de devices.
- Core0→Core2: eventos externos timestampados, injetados em fronteira segura.
- Core2→Core0→Core1: mudança de IPL emulado, sem conversão de IRQ ARM.
- Áudio: Core2 produz PCM; Core3 possui DMA/HDMI; underrun/completion retorna
  como telemetria, não como mutação direta do Rigel.

### IRQ

- Implementar vectors/dispatch de plataforma Bellatrix sem entrar nos handlers
  PiStorm do Emu68.
- Preferir afinidade DWC2/UART/HDMI com Core 3.
- Handler mínimo: snapshot/ack obrigatório, OR em pending bitmap, wakeup.
- Core 3 drena o periférico até idle; IRQs repetidas podem ser coalescidas.
- Se BCM2837 impedir afinidade direta, Core 0 atua somente como gateway mínimo.
- Polling permanece backend válido e fallback durante a migração.

## Plano incremental

### Fase 0 — estabilizar e medir o fix transitório

- Validar `launcher_owns_usb` em Pi 3B com HID + MSC.
- Instrumentar owner/core de cada entrada em CherryUSB e detectar reentrância.
- Registrar latência/custo do polling e frequência de trabalho USB/BT/áudio.
- Não remover o gate antes do cutover completo.

### Fase 1 — API de serviço e ownership verificável

- Introduzir `platform_io_start/poll/notify/stop` com backend single/multicore.
- Guard de debug afirma que apenas o executor autorizado entra na stack.
- Separar ativação (`poll`, `irq`, `request`) do processamento do serviço.
- Nenhuma mudança funcional no launcher nesta fase.

### Fase 2 — transporte Core3↔Core0

- SPSC de eventos/completions Core3→Core0, índices em cache lines separadas.
- Fila de comandos Core0→Core3 com IDs e completion explícita.
- Pending bitmap atômico + SEV inicialmente; política de overflow definida.
- Métricas: depth/max/dropped/latency e watchdog de request.

### Fase 3 — HID e dispatcher por contexto

- Callback USB publica evento normalizado exclusivamente no Core 3.
- Core 0 roteia `LAUNCHER` ou `MACHINE`; transição reconcilia teclas pressionadas.
- Core 0 timestampa; Core 2 injeta teclado/mouse no Rigel em fronteira segura.
- Substituir `launcher_input` atual pela SPSC/dispatcher.

### Fase 4 — MSC por block-I/O RPC

- Core 3 possui USB MSC/URBs e expõe read/write assíncrono.
- Core 0 mantém FAT e política do launcher.
- Adaptar FAT32 para request/completion sem reentrar CherryUSB no Core 0.
- Testar timeout, disconnect, erro parcial e remoção durante leitura.

### Fase 5 — cutover permanente

- Lançar Core 3 antes do launcher como owner já estabelecido.
- Remover `bellatrix_launcher_pump_usb()` e `launcher_owns_usb`.
- Launcher e runtime apenas alteram contexto/consumidor.
- Preservar single-core com serviço cooperativo e mesma API semântica.

### Fase 6 — subsistema de IRQ Bellatrix

- Pré-requisito: Emu68 escalonável e estável sem possuir IRQs de sistema.
- Mapear controlador de interrupções/afinidade BCM2837 por fonte.
- Instalar vector/dispatch per-core do Bellatrix e rotear I/O ao Core 3.
- Começar por uma fonte mensurável; manter polling fallback por device.
- Validar storm, lost wakeup, IRQ durante init/shutdown e coexistência com JIT.

### Fase 7 — integrar I/O ao árbitro temporal

- Core 0 atribui `host_timestamp` e `earliest_visible_tick` a eventos externos.
- Integrar completions ao cálculo de deadline/epoch de `ISSUE-0007`.
- Definir pacing e backpressure entre Core 2 PCM e Core 3 HDMI/DMA.
- Confirmar que throughput de I/O nunca bloqueia progresso determinístico.

## Critérios de aceite

- [ ] Exatamente um core pode entrar em cada driver/stack física.
- [ ] Launcher usa HID e MSC sem chamar CherryUSB/DWC2 no Core 0.
- [ ] Nenhum handoff de controladora entre launcher e runtime.
- [ ] Single-core e multicore usam a mesma API e passam os mesmos cenários.
- [ ] Eventos têm ordering, overflow e transição de contexto definidos.
- [ ] MSC sobrevive a timeout, disconnect e remoção durante operação.
- [ ] IRQ física nunca entra no handler INT6 PiStorm do Emu68.
- [ ] DWC2 e demais devices têm afinidade Core 3 ou fallback documentado.
- [ ] Polling e IRQ produzem comportamento funcional equivalente.
- [ ] Core 0 ordena eventos; Core 2 injeta; Core 1 vê somente IPL emulado.
- [ ] Métricas demonstram ausência de reentrância, drops e lost wakeups.
- [ ] Pi 3B boota launcher→Kickstart/Workbench com USB, multicore e HDMI áudio.

## Dependências e relação com outras issues

- `ISSUE-0044`: fix transitório e caso de regressão obrigatório.
- `ISSUE-0042`: faseamento de boot; será absorvido pelo cutover da Fase 5.
- `ISSUE-0007`: árbitro temporal; consome eventos/completions desta issue.
- `ISSUE-0002`/`emu68_public_api`: saída cooperativa antes da Fase 6.
- `issue_emu68_pistorm_interrupt_contract.md`: separação dos vectors.
- `issue_paula_audio_timing.md`: pacing Core2→Core3.

## Condições de parada

Parar a fase corrente se houver segundo executor entrando em CherryUSB, perda de
input, corrupção MSC, deadlock de completion, IRQ sendo traduzida para IPL6 pelo
Emu68, regressão de boot ou piora não explicada de pacing. Manter polling e o
último cutover funcional como rollback não destrutivo.

## Progresso

### 2026-07-11 — Fase 1: Core 0 assume I/O; Core 3 fica reservado

- removido o lançamento do loop de I/O no Core 3 durante boot e runtime;
- supervisor do Core 0 chama o serviço físico com cadência de ~1 kHz;
- heartbeat agora usa o contador da plataforma, sem delay fixo;
- launcher continua sendo o pump USB explícito e drena o console no Core 0;
- depois do launcher, o mesmo Core 0 assume o pump regular, sem handoff;
- Core 2 continua comunicando serial/Paula somente pelas filas existentes;
- `PAL_Core_LaunchIO()` foi preservado, mas não é chamado.

Validação necessária no Pi: launcher USB, boot multicore, HID no runtime,
console, HDMI áudio e confirmação de que Core 3 permanece estacionado.

Validação local concluída:

- harness compilou;
- 34/34 testes passaram (KS13, KS20, KS31, AROS e testes Rigel);
- Musashi 68040 single-core + launcher + USB + HDMI compilou;
- Musashi 68040 multicore + launcher + USB + HDMI compilou;
- a imagem instalada ao final é a variante multicore.

### 2026-07-10 — prioridade transferida para throughput multicore

O usuário confirmou em Pi 3B que o gate + guard restauraram launcher e runtime
multicore com KS13/Battle, sem USB log e sem MSC. O objetivo funcional imediato
foi alcançado. O FPS, porém, é baixo demais para validar áudio; como o cenário
não usa MSC e não produz logs USB, o redesenho completo de I/O não atacaria o
gargalo dominante observado. Esta issue permanece como arquitetura alvo, mas
volta a backlog enquanto `ISSUE-0007` mede e corrige CPU↔Rigel/HDMI throughput.

### 2026-07-10 — Fase 0 iniciada: boundary de ownership instrumentada

Implementado em `usb_host.c/.h`:

- guard atômico em todas as entradas públicas `init/step/shutdown`;
- identificação do executor por `MPIDR_EL1`;
- nesting permitido somente no mesmo core, necessário para callbacks e waits
  síncronos da CherryUSB;
- contenção cross-core é contada, logada uma vez e serializada antes de entrar;
- contadores de chamadas por core e log de transição de owner sob
  `BELLATRIX_USB_LOG`;
- o guard envolve a operação inteira, portanto também cobre os
  `USBH_IRQHandler()` internos disparados por `usb_osal_sem_take()`.

A auditoria encontrou uma segunda entrada real que o gate de ISSUE-0044 não
cobria: HDF/ISO anexados pelo launcher mantêm `Fat32File` backed por USB e fazem
`usbh_msc_scsi_read10()` durante runtime. Essa chamada agora usa
`usb_host_stack_enter/leave`. Até o RPC da Fase 4, ela é serializada com o pump
do Core 3; corrupção concorrente é impedida e a métrica revela a frequência da
contenção. Isso confirma que `launcher_owns_usb` sozinho não era uma fronteira
completa de ownership.

Validação de compilação concluída:

- Musashi multicore + launcher + USB log + HDMI áudio: build completo passou;
- Musashi single-core + launcher + USB sem log: build completo passou;
- rebuild incremental após incluir o caminho MSC no guard: passou.

Pendente para encerrar Fase 0: Pi 3B com HID + MSC e captura dos logs
`[USB-OWNER]`, especialmente durante leitura HDF/ISO após o launcher.
