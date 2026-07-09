# Emu68 Public API Adapter

## O que esta sendo feito

Estamos iniciando uma API publica Bellatrix/Emu68, mas sem transformar o Emu68
em uma biblioteca embutivel completa de uma vez.

O primeiro corte cria:

- um contrato publico em `src/cpu/emu68/emu68_api.h`;
- um adapter em `src/cpu/emu68/emu68_api_adapter.c`;
- registro desse adapter no build por `cmake/bellatrix-variant.cmake`;
- um patch minimo no Emu68, `patches/0021-emu68-public-bus-dispatch.patch`, que
  faz o fault/MMIO path existente chamar o dispatcher da API publica antes de
  cair no fallback `bellatrix_bus_access()`.

## Por que esta sendo feito

Hoje o Bellatrix depende de pontos internos do Emu68 para integrar bus, IRQ,
estado, invalidação e avanço temporal. Isso funciona, mas cria acoplamento forte:

- o Bellatrix precisa conhecer detalhes do live path do Emu68;
- mudancas no JIT/fault path podem quebrar a integracao sem contrato claro;
- fica dificil evoluir para execucao por janela e sincronizacao multicore limpa;
- fica dificil diferenciar "API publica suportada" de "detalhe privado atual".

A API publica cria uma fronteira explicita, com uma superficie pequena e honesta.

## Para que serve

Serve para permitir que o Bellatrix consuma o Emu68 como backend de CPU/JIT por
um contrato progressivo:

- bus externo por callbacks;
- IRQ por nivel;
- estado minimo inspecionavel/aplicavel;
- invalidacao de codigo;
- eventos basicos;
- estatisticas leves para validar uso do dispatcher;
- futuramente execucao por janela e sync boundary real.

O objetivo pratico e reduzir o acoplamento direto com `vectors.c`,
`ExecutionLoop.c`, caches internas e estado global, sem quebrar o boot atual.

## Escopo desta fase

Esta fase nao tenta resolver tudo.

Dentro do escopo:

- criar o header publico;
- criar o adapter Bellatrix-owned;
- conectar o bus publico ao hook ja existente do patch `0002`;
- sinalizar `EMU68_BUS_SYNC_REQUIRED` em writes criticos de custom/CIA;
- manter fallback para o caminho legado;
- validar build e patch verification;
- documentar a fronteira em `docs/`.

Fora do escopo por enquanto:

- `run_cycles()` real;
- `step()` real;
- HLE;
- multi-instancia real;
- API publica de FPU/MMU;
- snapshot completo;
- remover o live path existente.

## Decisao

A API publica inicial do Emu68 para Bellatrix fica no lado Bellatrix:

- `src/cpu/emu68/emu68_api.h`
- `src/cpu/emu68/emu68_api_adapter.c`
- registro no build por `cmake/bellatrix-variant.cmake`

O submodulo `emu68/` continua sendo alterado apenas por patches em `patches/`.
Nao adicionar arquivos diretamente ao submodulo como fonte de verdade.

## Por que

O projeto ja tem uma fronteira de build correta:

- `patches/0001-add-bellatrix-variant-cmake.patch` registra `VARIANT=bellatrix`;
- `emu68/CMakeLists.txt` inclui `../cmake/bellatrix-variant.cmake`;
- `cmake/bellatrix-variant.cmake` lista fontes, includes e defines Bellatrix.

Logo, fontes novas de integracao pertencem a `src/` e ao `cmake/` do Bellatrix,
nao ao tree upstream do Emu68.

## Patch Emu68 Necessario

`patches/0021-emu68-public-bus-dispatch.patch` e o patch novo de bus publico
desta fase. Ele altera `emu68/src/aarch64/vectors.c` para:

- incluir `cpu/emu68/emu68_api.h`;
- chamar `emu68_api_dispatch_bus_access(...)` no path de fault/MMIO;
- cair para `bellatrix_bus_access(...)` se a API nao estiver inicializada ou nao
  suportar aquele acesso.

Isso preserva o live path existente e reduz risco.

`patches/0003-bellatrix-execution-loop.patch` tambem participa da API agora:
alem do progresso Bellatrix existente, o bloco de progresso do `MainLoop()`
chama `emu68_api_dispatch_quantum_progress(...)`. Sem janela ativa isso e
no-op; com uma janela armada por `emu68_run_cycles()`, o loop retorna quando o
orcamento expira ou quando `emu68_request_stop()` e observado.

## Estado Real da API

Funcional nesta fase:

- `emu68_api_version()`
- singleton `emu68_create()` / `emu68_destroy()`
- `emu68_set_bus()`
- log de boot `[EMU68-API] v1 bus registered` quando o bus publico e registrado
- trace opt-in `BELLATRIX_EMU68_API_TRACE=1` para imprimir primeira leitura,
  primeira escrita e primeiro `sync-required` via dispatcher publico
- autodump opt-in `BELLATRIX_EMU68_API_AUTODUMP=1` para imprimir uma linha de
  stats no primeiro `sync-required`, util em QEMU quando nao ha ferramenta guest
  para escrever em `0xDFFF08`
- callbacks de bus 8/16/32 via fault path de `vectors.c`
- `EMU68_BUS_SYNC_REQUIRED` para writes criticos de custom/CIA, ainda como
  sinal/evento sem parar execucao
- `emu68_get_stats()` / `emu68_reset_stats()` para diagnostico leve
- controle temporario `0xDFFF08`: write `1` dump, `2` reset, `3` dump+reset
- `emu68_set_irq_level()`
- `emu68_get_state()` / `emu68_set_state()`
- `emu68_invalidate_code_range()` / `emu68_invalidate_all_code()`
- eventos basicos para stop, invalidate e sync-required
- `emu68_run_cycles()` como janela cooperativa sobre o `MainLoop()` vivo,
  quando `__m68k_state` ja foi inicializado
- `emu68_step()` como janela minima de 8 ciclos estimados

Ainda nao funcional como contrato real completo:

- HLE callbacks
- multi-instancia real
- bootstrap publico independente de `M68K_StartEmu()` / estado global
- uso do backend Emu68 inteiro pelo loop generico `CpuBackend`

Motivo: o Emu68 atual ainda nasce pelo `M68K_StartEmu()` global e o boot normal
continua entrando no `MainLoop()` continuo. A janela cooperativa e livewired no
loop real, mas ainda nao substitui o bootstrap global nem transforma o Emu68 em
uma biblioteca multi-instancia.

## Relacao com `wip/emu68-liveness`

O branch `wip/emu68-liveness` e uma linha de diagnostico/bisseccao de liveness,
nao uma base de API publica. Ele altera principalmente:

- `patches/0003-bellatrix-execution-loop.patch`
- `patches/0009-bellatrix-boot-config.patch`
- `cmake/bellatrix-variant.cmake`
- issues de diagnostico

Nao misturar automaticamente essas alteracoes com a API. Se for integrar o branch,
fazer uma avaliacao separada porque ele desfaz parte das protecoes de `x12/v28`
que existem no branch atual.

## Build Validado

Validado com:

```bash
rtk timeout 120s bash scripts/build.sh
rtk timeout 180s env BELLATRIX_LAUNCHER=0 bash scripts/build.sh
rtk timeout 120s bash scripts/setup.sh --verify
rtk timeout 180s env BELLATRIX_CPU_BACKEND=emu68 BELLATRIX_HDMI_AUDIO=1 BELLATRIX_USBSTACK=1 BELLATRIX_BTSTACK=0 BELLATRIX_LAUNCHER=0 bash scripts/build.sh
```

Resultado: `emu68/install-bellatrix-rigel/Emu68.img` gerado com sucesso.

Tambem validado com uma execucao curta no QEMU sem launcher:

```bash
rtk timeout 35s qemu-system-aarch64 \
  -M raspi3b -accel tcg,tb-size=64 \
  -kernel emu68/install-bellatrix-rigel/Emu68.img \
  -dtb emu68/install-bellatrix-rigel/bcm2710-rpi-3-b.dtb \
  -serial null -serial stdio -display none \
  -append enable_cache -initrd src/roms/KS13.rom
```

Resultado observado: boot Emu68/Bellatrix, log
`[EMU68-API] v1 bus registered`, entrada no JIT, overlay desligado e Z2 Fast RAM
configurada. QEMU TCG e lento, entao isso e sanity check de inicializacao, nao
validacao funcional completa dos contadores.

Validacao especifica da API com `src/roms/aros.rom`:

```text
[EMU68-API] v1 bus registered
[EMU68-API] first bus write addr=000000e4 size=4 value=03fbdec0 pc=00000000
[EMU68-API] first bus read addr=00f00000 size=2 value=00000000 pc=00f8011c
[EMU68-API] first sync-required addr=00bfe001 size=1 value=00000000 pc=00f80144
```

Esses logs confirmam que:

- o singleton da API publica foi criado no boot;
- o fault path passa pelo dispatcher publico para write;
- o fault path passa pelo dispatcher publico para read;
- o sinal `EMU68_BUS_SYNC_REQUIRED` esta sendo produzido em acesso CIA.

Esses logs de primeira ocorrencia dependem de build com
`BELLATRIX_EMU68_API_TRACE=1`; builds normais mantem apenas os contadores e o
controle `0xDFFF08`.

Para validar stats sem guest tool, usar build com
`BELLATRIX_EMU68_API_AUTODUMP=1`. Isso imprime uma linha
`[EMU68-API] first-sync stats ...` no primeiro acesso que retornar
`EMU68_BUS_SYNC_REQUIRED`. O controle explicito `0xDFFF08` continua sendo o
caminho guest-facing para dump/reset.

Validado em QEMU/AROS:

```text
[EMU68-API] first-sync stats bus_r=1 bus_w=2 sync=1 err=0 unhandled=0 bad_size=0 stop=0 inv=0
```

O mesmo run chegou ao serial do AROS com lista de resident modules,
`ROMInfo: 1MiB ROM detected` e autoconfig da Z2 Fast RAM. O timeout cortou a
saida depois disso, entao ainda nao tratar como boot completo de Workbench.
