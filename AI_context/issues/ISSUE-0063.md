---
id: ISSUE-0063
title: "Investigação: gap de performance bare-metal vs harness — modelo de execução CPU↔Rigel"
status: doing
priority: high
type: research
owner: agent
created_at: 2026-07-17
updated_at: 2026-07-17
tags: [performance, synchronization, jit, mmio, profiling, rigel, emu68, execution-model]
blockers:
  - "Fase 1/2 (dump do profiler, toggles de diagnóstico) exigem boot em Pi 3B real + captura serial — aguardando execução do Jaime"
related_files:
  - problema.md
  - docs/uae_references.md
  - docs/rigel_suggestions_for_rigel_team.md
  - src/machine/machine_rigel_step.c
  - src/machine/machine_rigel_bus.c
  - src/machine/machine_rigel_internal.h
  - src/cpu/emu68/bellatrix_profile.h
  - src/cpu/emu68/bellatrix_profile.c
  - src/cpu/emu68/bellatrix.c
  - src/launcher/btscan.c
  - src/cpu/cpu_bridge.c
  - src/runtime/core_chipset.c
  - external/rigel/src/chipset/chipset.c
  - external/rigel/src/chipset/agnus/agnus.c
  - external/rigel/src/chipset/agnus/timing/slot_scheduler.c
  - external/rigel/AI_context/dma_slot_timing.md
  - AI_context/consolidated/emu68_routing_vs_synchronization.md
  - AI_context/consolidated/history/ISSUE-0048.md
  - AI_context/consolidated/history/ISSUE-0050.md
---

# Resumo

`problema.md` (documento de análise trazido pelo Jaime, incluindo uma
proposta de "Bellatrix Runtime Execution Model") levanta a hipótese de que
o gargalo de performance atual é um lockstep fino entre CPU e Rigel,
causado por sincronização a cada bloco JIT traduzido (fronteira de
`MainLoop`). Investigação de código nesta sessão mostrou que essa hipótese
específica **não bate com a implementação atual** — mas revelou um
candidato mais concreto e testável. Esta issue rastreia a investigação
completa até uma conclusão mensurável.

# Problema

O harness (Musashi/emu68 rodando em host x86, `build_harness_rigel`) entrega
uma performance percebida como aceitável. O bare-metal no Pi 3B não — mas
não está estabelecido se a causa é falta de potência bruta do Pi, overhead
de sincronização CPU↔Rigel, ou algo específico do caminho de vídeo/áudio.
`docs/uae_references.md` (port TeensyUAE rodando Amiga OCS completo em
full-speed num Cortex-M7 single-core, mais fraco que o Pi 3B em vários
eixos) é evidência forte de que a resposta não é simplesmente "o Pi não tem
potência" — o gargalo é provavelmente arquitetural ou uma regressão
mensurável.

## O que já foi descartado (verificado por leitura de código, não por
suposição)

1. **"Sync a cada bloco JIT = lockstep fino"** — falso para o caminho de
   ciclos de CPU. `bellatrix_machine_advance()` (`machine_rigel_step.c:838`)
   só acumula em `s_cpu_approx`; o avanço caro (`rigel_step()`) só dispara
   quando o acumulado cruza `s_quantum`, calculado por
   `machine_next_quantum()` (`machine_rigel_step.c:619`) via
   `rigel_get_next_observable_deadline()`. Isso já é essencialmente o
   modelo "deadline-based" / "hybrid" que `problema.md` propõe como solução
   — já implementado.
2. **Lockstep multicore via Reactor** — também não confirmado.
   `bellatrix_bridge_publish_cpu_cycles()` (`cpu_bridge.c:255`) é publish
   lock-free; `core_chipset_wait_caught_up()` só dispara para uma allowlist
   pequena de registradores críticos (INTENA/INTREQ/DMACON/BLTSIZE/COPJMP),
   não em todo acesso.

## Candidato atual (não confirmado, precisa medição)

Todo acesso MMIO em single-core (`bellatrix_machine_read`/`_write`,
`machine_rigel_bus.c:690-724`) chama `machine_flush_for_bus()`
sincronamente, que roda o `machine_quantum_step()` **completo**
(`machine_rigel_step.c:640`) — incluindo drain de serial, drain de
teclado, tick de áudio, checagem de frame/IRQ/hblank, trace — mesmo para um
flush parcial de poucos ciclos. Isso roda por **escrita de registrador**,
não por bloco JIT. Um Copper list ou blitter setup toca dezenas de
registradores por linha/frame. Hipótese: esse custo fixo por acesso MMIO,
desprezível num host x86 rápido (explicando por que o harness "funciona"),
domina no Pi 3B (A53, mais lento por instrução) — coerente com o gap
residual já registrado em ISSUE-0048 ("mmio_flush in-game").

Segunda hipótese, independente, sobre a proposta de contrato
`cpu_run_until`/`exit_reason` de `problema.md`: `RIGEL_MIN_QUANTUM` é 8
ciclos (`machine_rigel_internal.h:95`). Um bloco JIT do Emu68 não tem
tamanho máximo conhecido/limitado; se rodar mais que isso antes de reportar
progresso, o deadline já foi ultrapassado antes de o Bellatrix saber. Isso
seria um problema de **precisão de entrega de IRQ**, não de throughput —
diferente do argumento original do documento, mas potencialmente real.

## Achado crítico (2026-07-17, bloqueia a Fase 1):
## `BELLATRIX_PROFILE=1` trava o boot — isolado e confirmado

Ao tentar executar a Fase 1, o boot travou de forma determinística e
reproduzível logo após `[BOOT] Adjusting memory blocks` /
`[BOOT] Trimming to ...` (`emu68/src/aarch64/start.c:1281-1330`) — **antes**
de qualquer inicialização do Bellatrix (`[BELA]`), antes do BTStack, antes
do M68K/JIT começar. Confirmado em três ambientes independentes:

1. QEMU (`tests/qemu-headless.sh`, `BUILD=0 KICKSTART=src/roms/KS13.rom`),
   travado por 900s sem nenhum progresso além dessa linha.
2. Pi 3B real, duas tentativas manuais do Jaime (reflash + boot), ambas
   travando no mesmo ponto exato, sem se recuperar sozinho (precisou reset
   manual).
3. Isolamento decisivo: build idêntico, **sem** `BELLATRIX_PROFILE=1`,
   testado no QEMU — passa direto por esse ponto, chega em `[BELA]`,
   inicializa BTStack (PatchRAM download, polling normal), tenta EMMC
   (timeout esperado — QEMU não tem SD montado) e entra em loop estável de
   reactor/BT-IRQ.

**Conclusão**: `BELLATRIX_PROFILE=1` — a flag que liguei para a Fase 1,
infra de profiling nunca antes exercida em sessão nenhuma — quebra o boot
de forma determinística. Não é "single-core quebrado" em geral (hipótese
do Jaime, razoável dado o sintoma, mas descartada pelo isolamento acima)
nem uma regressão pré-existente no HEAD (o build sem a flag funciona). É
específico dessa flag.

**Por que é surpreendente**: `BELLATRIX_PROFILE` só deveria afetar código
Bellatrix-específico (`bellatrix.c`, `bellatrix_profile.c`,
`core_chipset.c`, `cpu_bridge.c`) via `#if BELLATRIX_PROFILE_ENABLED` —
não deveria tocar `emu68/src/aarch64/start.c`, que roda antes de qualquer
coisa Bellatrix. Hipótese não confirmada: o struct global `BellatrixProfile
g_bprof` (~256 slots de hotspot MMIO + vários buckets, alguns KB de BSS)
pode estar deslocando o layout de memória/pools o suficiente pra
interferir com o cálculo de `top_of_ram`/trim de blocos de memória do
device tree logo depois — mas isso é especulação, não verificado.

**Ação**: reverti a flag como padrão de teste — a imagem correta pro
Jaime usar agora é a build SEM `BELLATRIX_PROFILE`
(`emu68/install-bellatrix-rigel/Emu68.img`, buildada 2026-07-17 04:02,
sem a flag). A Fase 1 (dump do profiler) fica bloqueada até esse bug ser
raiz-causado e corrigido — vira um pré-requisito novo, não estava previsto
no escopo original desta issue.

### Mecanismo exato (encontrado por diff dos dois logs de boot)

`emu68/src/aarch64/start.c:1228` tem
`#if !defined(PISTORM) && !defined(BELLATRIX)` — um ramo **genérico** do
Emu68 upstream (não deveria compilar no Bellatrix nunca) que imprime
"Adjusting memory blocks" e segue um fluxo de carregamento
ELF/HUNK/trim-de-memória incompatível com o resto da inicialização
Bellatrix. O `#else` (linha 1342) é o ramo Bellatrix-específico, que
imprime "Loading ROM from ... size 524288" e segue vivo.

Diff linha-a-linha dos dois boots (QEMU, mesmo `-initrd KS13.rom`) mostra
que o build com `BELLATRIX_PROFILE=1` caiu no ramo genérico (trava) e o
build sem a flag caiu no ramo certo (funciona) — ou seja, `BELLATRIX`
**não estava definido** para essa unidade de compilação no build com a
flag, apesar de `cmake/bellatrix-variant.cmake:44` (`add_compile_definitions(BELLATRIX ...)`) ser incondicional.

**CONFIRMADO**: rebuild limpo (`BELLATRIX_PROFILE=1 ... ./scripts/build.sh
clean`) resolve — boot idêntico ao saudável (Loading ROM → BELA → BTStack
→ EMMC probe → reactor loop estável), testado no QEMU. Causa raiz é cache
do CMake ficando stale entre toggles de opção — alternar
`BELLATRIX_PROFILE` sem `clean` deixa `BELLATRIX` sem ser definido em pelo
menos `start.c`. Não é bug de código do Bellatrix, é fragilidade do
processo de build.

**Investigação adicional (a pedido do Jaime, "veja exatamente por que essa
fragilidade")**: refiz a sequência de toggle (clean com profile=1 → sem
clean pra profile=0 → sem clean de volta pra profile=1), inspecionando
`emu68/build-bellatrix-rigel/CMakeFiles/Emu68.elf.dir/flags.make` a cada
passo. **`-DBELLATRIX` e `-DBELLATRIX_PROFILE=1` permaneceram corretos em
toda a sequência**, e o boot no QEMU confirmou saudável no final. Ou seja:
**alternar a flag sem `clean` não é, em si, o problema** — uma linhagem de
build que começa limpa se mantém consistente através de toggles
subsequentes sem clean.

**Conclusão revisada**: o travamento original não veio de "qualquer
toggle sem clean quebra" — veio do diretório `emu68/build-bellatrix-rigel`
já estar desatualizado/inconsistente **antes desta sessão começar**,
provavelmente acumulado ao longo de várias sessões anteriores sem um
rebuild completo (coerente com os dois bugs de include pré-existentes que
só apareceram quando o toggle forçou recompilação total de arquivos que
não eram tocados há tempo — ver "O que foi feito"). Não é uma fragilidade
estrutural do processo de build; é risco de estado velho acumulado num
diretório de build de longa duração.

**Regra prática**: se o diretório de build já existe há muitas sessões
sem um rebuild completo, prefira `clean` antes de confiar num toggle de
flag. Uma vez com uma base limpa, toggles subsequentes são seguros.

**Fase 1 desbloqueada**: a imagem `emu68/install-bellatrix-rigel/Emu68.img`
(buildada 2026-07-17 04:16, `BELLATRIX_PROFILE=1`, com `clean`) está pronta
pra flashar e capturar o dump do profiler.

**Correção**: cheguei a suspeitar que o toggle "MMIO profiling" da TUI
(`tools/launcher/tui.go`) não tivesse efeito algum (por só prefixar
`BELLATRIX_PROFILE=1` como env var na frente de um comando QEMU) —
**errado**, o Jaime corrigiu. Aquela função (`qemuCommand()`, linha 724)
é só um preview de texto renderizado na tela (`View()`, linha 716), não o
caminho de execução real. O fluxo de verdade é `./run.sh` → TUI só
coleta a seleção num tmpfile (`load_launcher_selection`) → `run.sh`
(linha 607-608) chama `setup.sh`+`build.sh` de verdade, exportando
`BELLATRIX_PROFILE` corretamente (`run.sh:590`). O toggle funciona; só o
preview de texto é cosmeticamente enganoso. Não é a causa do bug de boot.

## Achado principal (confirmado em código, não especulação):
## Rigel avança CCK-a-CCK, sem pular ciclos ociosos

Números já medidos em hardware real (`AI_context/consolidated/history/
ISSUE-0050.md`, gate de 2026-07-13, fonte de alimentação boa,
`throttled=00000000` confirmado): chipset a **~760 K CCK/s** (PAL CCK ≈
3,546 MHz → **~21% do realtime**, o "25%" que o Jaime lembrava de cabeça
bate dentro da margem de arredondamento), **Emu68 ≡ Musashi** (mesmos
CCK/s e fps) — ou seja, **o gargalo já estava provado como chipset-bound e
independente de backend de CPU antes desta sessão**. `~1,08 µs/CCK`
(~1300 ciclos ARM por CCK) dentro de `rigel_step_until`. Gap para 50 fps:
4,7×.

Seguindo o pedido do Jaime de comparar o pipeline do Rigel com a
referência UAE (`docs/uae_references.md`, seção "Scheduler de eventos"):
li o código de `rigel_step_until` até o fundo.

- `rigel_step()` (`external/rigel/src/core/rigel.c:306`) chama
  `rigel_chipset_step()` (`external/rigel/src/chipset/chipset.c:57`), que
  chama `rigel_agnus_step()` (`external/rigel/src/chipset/agnus/agnus.c:107`).
- O comentário no próprio código já documenta a escolha: *"Slot loop
  drives beam CCK-by-CCK, dispatches copper and blitter (Approach C)"*.
- `agnus_slot_scheduler_step_until()`
  (`external/rigel/src/chipset/agnus/timing/slot_scheduler.c:925`) é
  literalmente:
  ```c
  for (i = 0; i < cycles; i++)
      agnus_slot_scheduler_step(sched, ctx, line_clocks, frame_lines);
  ```
  **Sem exceção para ciclos "ociosos"** — todo CCK do quantum (mesmo os
  ~228 de uma scanline inteira, mesmo em zona de blank sem DMA) paga o
  custo cheio de `agnus_slot_scheduler_step()`
  (`slot_scheduler.c:799-920`): resolução de dono do slot, disparo do
  slot, `rigel_beam_domain_step()`, e **`rigel_denise_step()` chamado a
  cada CCK** (não por linha).
- `external/rigel/AI_context/dma_slot_timing.md` (doc interno do próprio
  Rigel) já documenta essa decisão como intencional: existiam três
  abordagens candidatas (A = cycle-step aproximado, B = slot-step puro, C
  = "event-driven sobre slots"), e "Approach C" foi escolhida como alvo
  arquitetural — só que "event-driven" ali significa que `next_deadline`
  vira preciso (`agnus_slot_scheduler_next_event()` já existe e sabe achar
  o próximo slot não-livre), **não** que o loop principal pula ciclos sem
  evento. O loop em si nunca pula — ele resolve isso via correção, mas
  sempre percorre.

Isso é exatamente o oposto do UAE 0.6.9 referenciado em
`docs/uae_references.md`: UAE antigo (não cycle-exact) decide por
**linha** (`decide_line`/`decide_fetch`), não por CCK — o custo por linha
é ~1 chamada de decisão, não ~228-284 chamadas de step. A troca é
precisão vs. velocidade: o Rigel escolheu cycle-exact (necessário pra
truques de Copper/sprite multiplexado que jogos de demoscene tipo Battle
Squadron usam), o UAE 0.6.9 escolheu velocidade. **Isso não é bug — é uma
decisão arquitetural documentada do lado do Rigel**, mas agora está
identificada como candidata número 1 pro custo estrutural de ~1,08 µs/CCK,
à frente do `mmio_flush` (que ainda é real, mas secundário — o número do
ISSUE-0050 já mostrava Emu68 ≡ Musashi, o que aponta pro chipset, não pro
caminho MMIO específico de cada backend).

Registrado como sugestão formal em `docs/rigel_suggestions_for_rigel_team.md`
(não é decisão de arquitetura do Bellatrix — é uma pergunta pro lado do
Rigel: dá pra pular CCKs "silenciosos" — sem DMA, sem sprite, sem mudança
de estado de Denise — sem perder cycle-exactness onde ela importa?).

# Objetivo

Determinar, com medição e não especulação, a classe do gargalo bare-metal
(potência bruta / overhead de sincronização MMIO / vídeo-áudio / overrun de
quantum / outra) e, só então, decidir se algum redesenho arquitetural
(contrato `cpu_run_until`, flush parcial mais barato, etc.) é justificado.
Performance é consequência da medição, não o ponto de partida.

# Tracker de fases

- [x] **Fase 0 — Build de sanidade.** `BELLATRIX_PROFILE=1` compila (depois
      de corrigir dois bugs pré-existentes de include faltando, ver "O que
      foi feito") — **mas trava o boot**. Ver "Achado crítico" abaixo.
      Bloqueia toda a Fase 1 até resolver.
- [ ] **Fase 1 — Profiler bare-metal.** Bootar a imagem no Pi 3B com carga
      representativa (Battle Squadron, que já tem repro headless
      documentado) e capturar dump por serial (escrever `0x01` em
      `0xDFFF04`; resetar contadores com `0x02` logo antes de começar a
      medir, pra boot/launcher/pairing não poluir a janela). Isolar:
      `dispatch_read`/`dispatch_write`, `region_*`, `chipset_step_time`,
      `lock_wait`, stats multicore (`wakeups`, `backlog_cck_max`,
      `critical_mmio_backlog_*`). Confirmar que o `chipset_step_time`
      domina o total — é o que os números do ISSUE-0050 (Emu68 ≡ Musashi,
      chipset-bound) já sugerem.

      **Confundidor identificado (2026-07-17, imagem tem BTSTACK+USBSTACK
      habilitados por necessidade — launcher não tem bypass, precisa de
      input físico pra selecionar mídia):** em single-core,
      `PAL_Runtime_Poll()` (`src/host/raspi3/pal_core.c:322-346`) chama
      `bt_host_step()`/`usb_host_step()` **inline no mesmo core** que
      CPU+chipset (throttlado a ~1ms, mas real). Isso é um candidato
      alternativo pra explicar a paridade Emu68≡Musashi do ISSUE-0050 (os
      dois pagariam o mesmo imposto de IO, não só o mesmo custo de
      chipset). Não precisa instrumentar nada novo: o heartbeat
      `[HOST-IO]` (`bellatrix.c:354`, sempre ativo, independente de
      `BELLATRIX_PROFILE`) já imprime `usb=XXus bt=YYus` (pico por poll).
      Uma scanline PAL ≈ 64µs — se `usb=`/`bt=` chegarem nessa ordem de
      grandeza, é um contribuinte real a separar do loop CCK do Rigel.
      **Ação**: grep tanto `[BPROF]` quanto `[HOST-IO]` na mesma captura.
      **[BLOQUEADO — requer hardware real, ver `blockers`]**
- [ ] **Fase 2 — Modo diagnóstico à la UAE.** Toggles temporários
      (render off / blitter instantâneo / Copper granularidade reduzida /
      áudio off), medir FPS a cada corte. Com o achado do loop CCK-a-CCK
      abaixo, a expectativa agora é que desligar `rigel_denise_step`
      (chamado por CCK dentro do slot loop) sozinho já mova a agulha.
      **[BLOQUEADO — requer hardware real para medir FPS]**
- [x] **Fase 3 (revisada) — Comparação de pipeline Rigel vs. UAE.** Pedido
      explícito do Jaime. Lido `rigel_step` → `rigel_chipset_step` →
      `rigel_agnus_step` → `agnus_slot_scheduler_step_until` até o fundo.
      **Confirmado**: o slot loop do Rigel roda CCK-a-CCK sem pular ciclos
      ociosos (`for (i = 0; i < cycles; i++) agnus_slot_scheduler_step(...)`,
      `slot_scheduler.c:925`), chamando `rigel_denise_step()` a cada CCK —
      o oposto do UAE 0.6.9, que decide por linha. Decisão arquitetural
      documentada do próprio Rigel (`external/rigel/AI_context/
      dma_slot_timing.md`, "Approach C"), não um bug. Ver seção "Achado
      principal" acima. Registrado como sugestão em
      `docs/rigel_suggestions_for_rigel_team.md`.
- [ ] **Fase 3b — Experimento cirúrgico no `machine_flush_for_bus`.**
      Rebaixada de prioridade (era a hipótese nº1, agora é secundária —
      Emu68≡Musashi no ISSUE-0050 já aponta pro chipset, não pro caminho
      MMIO específico do backend). Ainda vale medir via Fase 1 antes de
      descartar de vez.
- [ ] **Fase 4 — Overrun de quantum (`RIGEL_MIN_QUANTUM`).** Usar
      `advance_stats.cpu_cycles_max` (já existe em
      `bellatrix_profile.h:34-38`) comparado ao tamanho de quantum vigente
      no mesmo instante, para confirmar ou descartar se blocos JIT
      estouram deadlines de 8 ciclos antes de reportar. Só se confirmado,
      considerar o contrato `cpu_run_until`/`exit_reason` de `problema.md`
      — e pelo motivo certo (precisão de IRQ), não por throughput.

# O que foi feito

- Leitura completa de `problema.md` e `docs/uae_references.md`.
- Verificação em código de que o modelo atual já faz acumulação/deadline
  (não lockstep por bloco), invalidando a premissa central do documento.
- Identificação do candidato secundário: `machine_flush_for_bus` chamando
  `machine_quantum_step` completo por acesso MMIO.
- Confirmado por grep: `cpu_run_until`/`exit_reason`/`cpu_budget_t` não
  existem em lugar nenhum do código — seria trabalho novo, não uma
  correção de algo quebrado.
- Confirmado que `BELLATRIX_PROFILE` (bare-metal only, `CNTPCT_EL0`) existe
  e nunca foi usado/documentado numa sessão anterior — nenhuma menção em
  `AI_context/`.
- Build de sanidade com `BELLATRIX_PROFILE=1`: falhou 2x por bugs
  pré-existentes não relacionados à flag (não introduzidos nesta sessão,
  expostos porque a mudança de define forçou rebuild completo em vez de
  incremental):
  - `src/cpu/emu68/bellatrix.c` chamava `launcher_save_bt_report()` sem
    incluir `launcher/btscan.h` (a função foi movida de `launcher.h` pra
    `btscan.h` em algum commit anterior, sem atualizar o include). Corrigido.
  - `src/launcher/btscan.c` chamava `PAL_Time_ReadCounter()` sem incluir
    `host/pal.h`. Corrigido.
  - Build passou depois das duas correções:
    `emu68/install-bellatrix-rigel/Emu68.img` pronto pra flash.
- **Achado principal**: leitura de `rigel_step`/`rigel_chipset_step`/
  `rigel_agnus_step`/`agnus_slot_scheduler_step_until` em
  `external/rigel/` confirmou que o chipset avança CCK-a-CCK sem pular
  ciclos ociosos — decisão arquitetural documentada do Rigel ("Approach
  C" em `dma_slot_timing.md`), não bug do Bellatrix. Isso é o oposto do
  modelo por-linha do UAE 0.6.9 referenciado em `docs/uae_references.md`,
  e bate com os números já medidos em hardware (ISSUE-0050: chipset-bound,
  backend irrelevante, ~21% do realtime). Ver seção "Achado principal"
  acima.
- Sugestão registrada em `docs/rigel_suggestions_for_rigel_team.md` sobre
  pular CCKs sem evento no slot loop.

# O que falta fazer

Ver tracker de fases acima. Fases 1, 2 e 3b dependem de boot em Pi real —
a Fase 3 (comparação de pipeline) já foi concluída sem hardware. Próximo
passo prático: o Jaime flashar `emu68/install-bellatrix-rigel/Emu68.img`
e capturar o dump do profiler (Fase 1) e/ou FPS com Denise-step
desabilitado temporariamente (Fase 2) pra confirmar quantitativamente o
achado do loop CCK-a-CCK antes de propor qualquer mudança no Rigel.

# Decisões tomadas

- Não adotar a reescrita de "execution model" de `problema.md` como está —
  a premissa de lockstep por bloco JIT está refutada pelo código atual
  (ver `AI_context/consolidated/emu68_routing_vs_synchronization.md`, que
  já documenta a separação roteamento vs. sincronização).
- Medir antes de redesenhar: seguir a ordem do tracker de fases, não pular
  direto para um contrato `cpu_run_until` sem confirmar que ele resolve um
  problema real medido.
- **Regra explícita do Jaime**: qualquer otimização de performance no
  Rigel (em particular o event-skip do slot loop CCK-a-CCK, seção 11 de
  `docs/rigel_suggestions_for_rigel_team.md`) não pode regredir a
  qualidade/precisão do Rigel. Tem que ser opcional (flag de compilação ou
  modo em runtime), com o comportamento cycle-exact atual permanecendo o
  padrão. Não é uma troca "vale a pena", é um requisito — Copper/sprite
  multiplexado (Battle Squadron e software estilo demoscene) depende de
  cycle-exactness.

# Critérios de aceite

- [ ] Dump do profiler capturado em hardware real com pelo menos um caso
      de carga pesada (Battle Squadron ou equivalente).
- [ ] Classe do gargalo identificada por eliminação (Fase 2) ou por
      medição direta (Fase 1/3/4), com números, não impressão.
- [ ] Decisão registrada sobre se `cpu_run_until`/`exit_reason` é
      necessário, e por qual motivo específico.

# Observações

`problema.md` continua sendo uma referência útil para o princípio geral
("Bellatrix deveria possuir o modelo de execução, não herdar do backend")
mesmo com a premissa de lockstep refutada — o princípio arquitetural é
consistente com o que já está em `CLAUDE.md` ("a chipset owns observable
time"). O valor do documento agora é motivacional/orientador, não
diagnóstico.

# Log de execução

- 2026-07-17: Issue criada após sessão de discussão sobre `problema.md` +
  `docs/uae_references.md`. Achados de código registrados acima.
- 2026-07-17: Build de sanidade `BELLATRIX_PROFILE=1` — 2 falhas por bugs
  de include pré-existentes, ambos corrigidos (`bellatrix.c`, `btscan.c`).
  Build passou, imagem pronta.
- 2026-07-17: A pedido do Jaime, comparado o pipeline interno do Rigel
  (`rigel_step_until`) com o modelo do UAE 0.6.9 referenciado em
  `docs/uae_references.md`. Confirmado: slot loop CCK-a-CCK sem event-skip,
  decisão arquitetural documentada do próprio Rigel, não bug. Rebaixada a
  prioridade da hipótese de `mmio_flush` (fase 3→3b) porque ISSUE-0050 já
  mostrava Emu68≡Musashi (chipset-bound, não backend-bound). Jaime também
  citou de memória "chipset a 25% do tempo esperado" para o problema de
  áudio — bate com o número medido do ISSUE-0050 (~21% do realtime,
  760 K CCK/s de ~3,55 M CCK/s do PAL), mesma causa raiz provável.
- 2026-07-17: Jaime rodou a imagem `BELLATRIX_PROFILE=1` no Pi real — boot
  travou logo após "Adjusting memory blocks", bem antes de qualquer coisa
  do Bellatrix. Investigação inicial confundida por uma imagem de 14/07
  ainda no cartão SD (build antigo, sem a flag, chegava até "opcode 00fc"
  bem mais tarde no boot — sintoma do ISSUE-0038, não confirmado se ainda
  existe no HEAD atual). Depois de reflash com a imagem certa de hoje, o
  Jaime confirmou 2x a trava no mesmo ponto exato — não era reboot
  espontâneo, eram duas tentativas manuais dele. QEMU reproduziu a mesma
  trava de forma determinística (900s sem progresso). Isolamento: build
  idêntico sem `BELLATRIX_PROFILE` passa direto por esse ponto no QEMU.
  Causa raiz identificada: **a flag em si**, não single-core (hipótese do
  Jaime, descartada), não regressão pré-existente. Ver "Achado crítico"
  acima. Fase 1 bloqueada até corrigir.
