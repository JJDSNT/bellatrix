---
id: ISSUE-0036
title: "Regressão bare-metal: console serial (mini-UART) silencioso/lixo na Pi 3B física"
status: resolved
priority: high
type: bug
owner: agent
created_at: 2026-07-04
updated_at: 2026-07-05
tags:
  - baremetal
  - regression
  - serial
  - raspberry-pi
  - tlsf
  - heap-corruption
  - non-determinism
related_files:
  - src/host/raspi3/console_log.c
  - src/host/raspi3/miniuart_backend.c
  - src/host/raspi3/pl011_backend.c
  - src/io/bluetooth/bt_host.c
  - src/io/bluetooth/bt_diag.c
  - src/io/usb/usb_config.h
  - src/io/usb/usb_host.c
  - src/io/usb/usb_hc_bellatrix.c
  - src/cpu/emu68/bellatrix.c
  - emu68/src/tlsf.c
  - emu68/include/tlsf.h
  - patches/0007-bellatrix-boot-sequence.patch
  - patches/0008-bellatrix-console-redirect.patch
  - patches/0019-emu68-tlsf-hardening.patch
---

# Estado atual

**Baseline estável: commit `776448b`** ("fix: resolve USB enumeration and console corruption during
USB init"). Console permanece nativo na mini-UART desde `setup_serial()` (sem handoff PL011→mini-UART
em nenhum ponto do boot; PL011 é reservado só pro Bluetooth). `kprintf` fica em modo direto/bloqueante
só na janela mínima entre o início do boot e `console_log_set_deferred()` (chamado logo antes de
`core_io_init()`); daí em diante escreve num ring buffer, drenado só a partir do loop de step do
chipset/emulação (`machine_step_host_serial_rigel()`), depois que o usuário escolhe uma ADF no
launcher.

Com `BELLATRIX_BTSTACK=0` este baseline produz log limpo do primeiro `[BOOT]` até o M68K rodando na
grande maioria dos boots — **mas resta um glitch residual não-determinístico** (ver seção final):
o mesmo binário, sem nenhuma mudança, às vezes mostra um bloco de corrupção durante a inicialização
pesada do USB, às vezes não.

**Não testado/resolvido**: log em tempo real durante a fase de pré-emulação (boot/USB/BT/launcher) —
várias tentativas foram feitas e revertidas (ver "Tentativas de log em tempo real", abaixo). Com
`BELLATRIX_BTSTACK=1`, a mesma classe de corrupção aparece durante o download do PatchRAM do BT; não
foi resolvida e não é prioridade atual (BT ainda não está funcional).

## Causas raiz confirmadas e corrigidas (permanentes)

1. **USB não enumerava** (`ctrl xfer failed`): `CONFIG_USB_ALIGN_SIZE` estava em `32`, não `64` (a
   linha de cache real da Cortex-A53/Pi 3B), com `USB_NOCACHE_RAM_SECTION` vazio (buffers de DMA do
   USB são `.bss` cacheado normal). Um buffer alinhado a só 32 bytes podia compartilhar linha de cache
   de 64 bytes com uma variável não relacionada; `usb_dcache_invalidate()` (chamado em toda transferência
   IN) descartava essa linha sem escrever de volta, apagando dados "sujos" da variável vizinha
   silenciosamente. Fix: `CONFIG_USB_ALIGN_SIZE=64` em `src/io/usb/usb_config.h`.

2. **`SIZE_ALIGN` do TLSF portado do upstream estava em 16, não 64**: mesmo mecanismo do item acima,
   mas no alocador de heap (`usb_osal_malloc()` → `tlsf_malloc()`). Corrigido em `emu68/src/tlsf.c`
   (`patches/0019-emu68-tlsf-hardening.patch`), mantendo as correções de lógica reais do TLSF upstream
   (hardening contra corrupção de heap, `TLSF_MULTITHREADING`) mas com o alinhamento certo pra esta
   plataforma.

3. **`GAHBCFG.GINT` (interrupção global do DWC2) deve ficar desabilitada**: Bellatrix roda o CherryUSB
   via polling cooperativo (`usb_host_step()`/`usb_osal_sem_take()`), sem vetor de IRQ real do ARM GIC
   atendendo o DWC2. Habilitar `GINT` muda a semântica de conclusão esperada pelo hardware pra depender
   de atendimento real de interrupção, que nunca acontece — todo transfer de controle dá timeout.
   Mantido desabilitado, com uma linha de log reportando o estado real do bit.

4. **Console corrompia durante toda a inicialização do USB**: `console_log_set_deferred()` (troca do
   `kprintf` de escrita direta/bloqueante pra ring buffer) só era chamado bem depois de
   `core_io_init()` — toda a inicialização pesada do USB/DWC2 (MMIO/DMA em sequência rápida) rodava com
   o console em modo direto/síncrono, byte a byte, na mesma janela de atividade de hardware. Fix:
   `console_log_set_deferred()` movido pra logo **antes** de `core_io_init()` em
   `src/cpu/emu68/bellatrix.c`.

5. **Baud da mini-UART divergia entre pontos de abertura**: `console_log_init()`/a bridge serial da
   Paula abriam a mini-UART em taxas diferentes (9600 vs 115200) em pontos diferentes do boot.
   Unificado pra 115200 em todo lugar.

6. **FIFO overrun na AUX mini-UART**: `miniuart_backend_write_byte()` escrevia incondicionalmente,
   sem checar o bit de "FIFO tem espaço" (LSR bit 5). Como `console_log_drain()` chama essa função em
   loop (até `CONSOLE_LOG_DRAIN_MAX=256` vezes por chamada) e o FIFO real da AUX só tem 8 bytes de
   profundidade, isso derrubava/corrompia bytes. Fix: checa o bit antes de escrever, retorna `false`
   se cheio (não-bloqueante — nunca usar spin-wait aqui, isso trava o step loop inteiro); o loop de
   `console_log_drain()` para no primeiro `false` em vez de descartar o byte e continuar.

Outros fixes menores mantidos: mascaramento de IRQ (`daifset`/`daifclr`) ao redor do par
checagem+escrita em `miniuart_backend_write_byte()` (defensivo, não comprovado como causa raiz mas
não tem custo); `usb_osal_msleep(5)` logo após semear o wakeup da root hub em `usb_hc_init()` —
observado como estabilizador possível durante a janela pesada de init USB, mas ainda não confirmado
como causa raiz.

Limpeza posterior: o diagnóstico visual "Pau de Cego", que pintava o framebuffer inteiro de vermelho
e logava essa ação, foi removido de `bellatrix_init()`. Ele era útil apenas para validar a cadeia VC4
durante bring-up e não deve permanecer no fluxo normal.

## Restrições de arquitetura que continuam valendo

- **PL011 nunca volta a ser usado para o console** — é reservado exclusivamente para o Bluetooth em
  todo build, mesmo com `BELLATRIX_BTSTACK=0`. Decisão de arquitetura anterior, não deste issue.
- O console mini-UART é nativo desde `setup_serial()` — não há handoff dinâmico de pino em nenhum
  ponto do boot.
- Nunca usar spin-wait bloqueante dentro de `miniuart_backend_write_byte()`/`console_log_drain()` —
  já causou uma regressão séria (loop de step travado, USB parecia ter parado de detectar
  dispositivos).

# Tentativas de log em tempo real pré-emulação (todas revertidas, nenhuma resolveu)

Objetivo: ter log real durante boot/USB/BT/launcher (antes da emulação M68K começar), sem reintroduzir
corrupção. Nenhuma das abordagens abaixo funcionou de forma confiável; todas foram revertidas de volta
ao modelo buffer-only (drena só a partir da emulação). Registradas aqui pra não serem retentadas sem
motivo novo:

1. **Drenar dentro de `bt_host_step()`/`usb_host_step()` a cada chamada** — arquitetura mais robusta em
   princípio (drenagem vira propriedade de "atender IO", cobre todo loop de pré-emulação de uma vez),
   mas reintroduziu corrupção porque o drain passou a coincidir em tempo real com rajadas de MMIO do
   DWC2/PL011.
2. **Gatear o log verboso do USB** (achando que era volume) — não resolveu; a corrupção persistiu com
   quase nenhum texto extra, e até apareceu mais cedo. **Provou que não é volume, é timing.**
3. **Barreira de memória (`dsb()`)** antes de tocar a mini-UART em `console_log_drain()` — não ajudou.
4. **Drenar uma vez logo após a rajada inicial de enumeração** (`bellatrix_usb_pump_events(4u)` em
   `usb_host_init()`) — melhorou mas não eliminou; o primeiro drain "de verdade" (dentro de
   `usb_host_step()`) ainda descarrega um backlog grande coincidindo com polling ativo.
5. **Throttle de ~1ms na função inteira** (replicando o intervalo que `PAL_Runtime_Poll()` já usa com
   segurança durante a emulação) — não resolveu: toda chamada que passava pelo throttle ainda fazia
   poll+drain na mesma invocação.
6. **Alternância estrita poll/drain** (nunca os dois na mesma chamada; para BT, só drenar quando
   `bt_hal_raspi3_io_activity()` mostra o PL011 parado por uma janela real) — também não resolveu.

**Conclusão desta rodada**: o mecanismo real por trás da corrupção durante inicialização pesada de
periférico (DWC2/PL011) ainda não foi entendido corretamente — nenhuma das teorias testadas (volume,
timing simples, barreira de memória, backlog) explica os resultados de forma consistente. Ver próxima
seção.

# Não-determinismo confirmado — ressalva importante para qualquer teste futuro

O mesmo binário (`776448b`, sem nenhuma mudança de código) produziu log limpo em alguns boots e
corrompido em outros. Isso significa que **um único teste "limpo" ou "sujo" não prova nada sozinho** —
qualquer fix candidato precisa ser testado em múltiplos boots consecutivos antes de ser considerado
confirmado ou descartado. Isso também explica por que várias das tentativas acima pareciam fazer
sentido logicamente mas "não funcionaram" — o teste que as invalidou pode ter simplesmente pego um boot
azarado, não necessariamente uma falha real da lógica testada.

**Para a próxima sessão**: se for retomar a investigação do log em tempo real ou do glitch residual,
começar por rodar o baseline atual (`776448b`) várias vezes seguidas pra medir a taxa de ocorrência real
do glitch antes de tentar qualquer fix novo — sem isso, é impossível diferenciar sinal de ruído.

# Direção recomendada para suporte de log

O estado mais limpo é separar explicitamente dois modos, como já sugerido em `ISSUE-0013`, em vez de
continuar misturando "debug log" e "raw Paula serial" na mesma política:

- **Modo log/debug**: a mini-UART pertence ao console/log em 115200 desde `setup_serial()`. `kprintf`
  nunca escreve diretamente depois do bootstrap curto; todo texto entra em um ring buffer. O drain deve
  ser feito por um único serviço de console, com budget pequeno e previsível por tick, sem rodar dentro
  de caminhos de MMIO/DMA sensíveis do DWC2/PL011. Se houver pressão, dropa log; nunca bloqueia e nunca
  reprograma a UART.
- **Modo raw serial**: a mini-UART pertence à ponte Paula/serial. `kprintf` deve ficar desligado ou
  redirecionado para buffer em memória/diagnóstico posterior; não deve misturar bytes arbitrários no
  stream raw.
- **Pré-emulação**: para logs antes do loop normal existir, preferir um buffer RAM grande com flush
  tardio quando o sistema entra no scheduler/launcher estável. Isso sacrifica tempo real na fase mais
  perigosa, mas preserva integridade. Tempo real só deve voltar depois que houver um "console pump" único
  e medido, separado de `usb_host_step()`/`bt_host_step()`.
- **USB/DWC2**: manter `GAHBCFG.GINT` mascarado enquanto Bellatrix usa polling cooperativo; o log
  permanente em `usb_hc_init()` deve informar o estado real do bit. Reavaliar IRQ de verdade só quando
  houver roteamento e handler SoC explícitos.

Prioridade de produto: **não corromper**. Logs mais próximos do tempo real são desejáveis, mas devem ser
tratados como otimização posterior com teste estatístico multi-boot, não como comportamento default da
fase pré-emulação.

# Contexto histórico (resumo dos becos sem saída, para quem for revisitar)

Esta issue passou por várias fases de investigação com teorias que pareciam plausíveis mas foram
descartadas por evidência real de hardware:

- **Bisect de commits antigos**: inicialmente pareceu que o commit `1415619` (remoção de um mecanismo
  de handoff de console do Bluetooth) era a causa raiz de uma regressão de meses atrás. Investigação
  posterior mostrou que a comparação real era o baud da mini-UART nunca ter batido com o PL011 nativo
  (9600 vs 115200) — não o commit em si. O bisect também sofreu de reconstrução manual de patches fora
  de sincronia em alguns pontos, o que introduziu resultados não confiáveis nessa fase.
- **TLSF/heap corruption**: hipótese de que uma regressão de layout de `.bss` expunha um bug latente no
  alocador TLSF upstream. Parcialmente verdade (item 2 acima, `SIZE_ALIGN`), mas não era a causa
  principal do serial.
- **Race de multicore, wait_idle no PL011, mailbox duplicada**: todas investigadas e descartadas ou
  encontradas como não-causais, no processo de isolar o problema real.

Essas investigações não são mais necessárias para entender o estado atual do código — o resumo das
causas raiz confirmadas (seção acima) é a referência que importa. Histórico bruto anterior (bisect
completo, teoria por teoria) foi removido deste arquivo para manter o documento navegável; se for
preciso consultar o raciocínio linha a linha de alguma tentativa específica, ver `git log -p` neste
arquivo ou o histórico de conversa da sessão de 2026-07-04.
