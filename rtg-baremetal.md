# RTG bare-metal — plano de integração via Emu68 VideoCore.card

> **Decisão (2026-07-18):** no alvo bare-metal (Raspberry Pi 3), o RTG não usa a
> nossa `bellatrix.card`. Usa a **`VideoCore.card` do Emu68**
> (`external/VideoCore.card`), que fala direto com o VideoCore do Pi. O harness
> continua com a `bellatrix.card` (framebuffer P96 portátil → SDL). São dois
> drivers para dois alvos, com uma VRAM/estado de scanout conceptualmente comum.
>
> A análise antiga deste arquivo (portar a nossa card para bare-metal) está
> preservada no fim como **Histórico**; a premissa dela foi superada por esta
> decisão, mas o mapeamento das 4 peças continua útil como referência do lado
> Bellatrix.

## Correção de entendimento: o que é o Framethrower

O `Framethrower_Denise` (PiStorm) **não** é onde mora a arbitragem de software.
É **hardware**: um RP2040 que amostra o barramento RGA da **Denise física** (via
PIO) e converte para um datastream **MIPI CSI-2**, que o Raspberry recebe pela
interface de câmera (**unicam**). Ele existe para trazer o vídeo do silício
Amiga real para dentro do Pi.

A arbitragem de verdade mora no **HVS do VC4** (Hardware Video Scaler), que
compõe dois planos:

- **plano RTG** — framebuffer que a `VideoCore.card` programa a partir da VRAM;
- **plano vídeo nativo** — o que o `unicam` capturou do Framethrower.

Consequência para o Bellatrix: **não temos Denise física nem Framethrower.** A
nossa Denise é emulada em software e já sai num framebuffer
(`machine_rigel.c:226`, RGB565). Toda a cadeia Framethrower→MIPI→unicam resolve
um problema que não temos. Portanto a arbitragem Denise×RTG no Bellatrix é
**mais simples** que a do PiStorm: os dois "planos" já são buffers em RAM.

- **Reutilizável** da `VideoCore.card`: o setup do HVS/planos (`src/vc4.c`,
  `src/vc6.c`) — como programar um plano de framebuffer e compor planos.
- **Não** reutilizável: `src/unicam.c` (captura CSI-2) e o firmware do
  Framethrower inteiro — só se aplicam a Denise de silício.

## Viabilidade de runtime — verificada (não é hipótese)

Emu68 pinado em **v1.0.7** (`305f686`, 2025-12-08). Dependências da
`VideoCore.card`, conferidas no código:

| Dependência | Obrigatória? | Situação no Bellatrix |
|---|---|---|
| `devicetree.resource` | **Sim** (`main.c:122`, `return 0` se faltar) | **Já fornecida.** `emu68/src/boards/devicetree.c` é um board Z3 ROM que expõe a resource ao guest; está no build (`emu68/CMakeLists.txt:160`). ✅ |
| `unicam.resource` | Não (`main.c:300`, `return 1` mesmo se ausente) | Só para captura Framethrower. Pulada. ✅ |
| VC4/HVS/mailbox físicos | Sim | Card mapeia via device tree sintetizada do Pi real; presente num boot Pi3. |

Ou seja, **a v1.0.7 já satisfaz a dependência mandatória** — não é preciso
bumpar o Emu68 para o RTG VideoCore.

## Conflito real de arquitetura: dono do HVS

O único obstáculo estrutural é o **modelo de display**, não a dependência de
runtime:

- **Bellatrix hoje**: framebuffer linear alocado pelo firmware (mailbox); Denise
  e launcher escrevem RGB565 nele; o firmware faz o scanout. Simples, não
  programa o HVS.
- **VideoCore.card**: programa a display list do HVS diretamente (planos, kernels
  de escala), ignorando o framebuffer simples do firmware.

Quando o RTG estiver ativo, a `VideoCore.card` assume o HVS; a saída da Denise
emulada precisa então (a) parar, ou (b) virar um segundo plano do HVS. Isso é a
Etapa 3.

## Plano em etapas

### Etapa 1 — build (mecânica, gated, reversível)

- `git submodule update --init` dos aninhados de `external/VideoCore.card`
  (`devicetree.resource`, `unicam.resource`) — hoje vazios; necessários para o
  source tree, mesmo com unicam desligado em runtime.
- Construir `VideoCore.card` (+ `unicam.resource` como dep de link). O
  `CMakeLists.txt` da card linka contra os alvos `amiga`/`devicetree`/`unicam`
  do ecossistema **Emu68-tools** e compila `-m68040`. Duas opções:
  1. adicionar Emu68-tools (ou só a `amiga` support lib) e construir a card lá;
  2. reproduzir o link `amiga` com o toolchain docker `m68k-amigaos` que já
     construímos a `bellatrix.card` (`emu68/build-scripts/build-m68k-amigaos`).
- Gate por flag de build nova `BELLATRIX_RTG_VIDEOCORE` (estilo
  `BELLATRIX_BTSTACK`). Entregar a `.card` por disco (LibList) ou embutida em ROM
  (padrão `lide_rom_data`/`rtg_rom_data`).
- **Não** toca em runtime nem no HVS. Só produz o artefato e o coloca na build
  bare-metal certa.

### Etapa 2 — carga e runtime

- Guest carrega a `VideoCore.card`, acha `devicetree.resource`, mapeia o espaço
  físico do VC4. Critério: log `[VC] InitCard ready` (`main.c:981`) num boot Pi3
  real.
- Confirmar que a device tree sintetizada pelo board do Emu68 expõe os nós que a
  card procura (mailbox, HVS).

### Etapa 3 — arbitragem Denise × RTG no HVS (o trabalho de verdade)

Opções, a decidir com medição:

- **(a) Switch**: com RTG habilitado, a `VideoCore.card` é dona do HVS e a saída
  da Denise emulada é suprimida; ao desabilitar, restaura o framebuffer do
  firmware. Mais simples; sem overlay.
- **(b) Composição de dois planos**: alimentar o plano "vídeo" do HVS com o
  buffer da Denise emulada (em vez do unicam) e o plano RTG com a VRAM,
  reusando o mecanismo de composição da `vc4.c`. Permite Denise e RTG juntos.

Referência de composição multi-plano: `external/VideoCore.card/src/vc4.c` /
`vc6.c` — **não** o Framethrower.

## Matriz de builds (alvo desta linha de trabalho)

| Build | Driver RTG | Saída |
|---|---|---|
| Harness (POSIX/SDL) | `bellatrix.card` | `bellatrix_rtg_get_frame()` → SDL |
| Bare-metal Pi3 | `VideoCore.card` (gated por `BELLATRIX_RTG_VIDEOCORE`) | HVS do VC4 |

A composição completa dos artefatos oficiais — boards por perfil, USB, BTStack,
áudio HDMI e a política pendente do launcher — está documentada em
`docs/release_profiles.md`. Em particular, `VideoCore.card` só acompanha os
perfis `emu68` e `musashi_68040`; `musashi_68000` permanece sem RTG e sem o board
de devicetree.

## Implementação Bellatrix do switch HVS

O Emu68 publica `/emu68/bellatrix-native-fb` com endereço, largura, altura e
pitch do framebuffer RGB565 produzido por Rigel/Denise. A `VideoCore.card`
detecta essa propriedade, constrói uma display list HVS nativa e reutiliza a
lógica histórica de `SetSwitch()`:

- modo nativo: display list apontando para o framebuffer Rigel;
- modo RTG: `vc4_ActivePlane`, mantido pela P96/`VideoCore.card`.

O caminho original `unicam.resource` continua inalterado e tem precedência
quando presente, preservando PiStorm/Framethrower. No Bellatrix não há captura
CSI: o framebuffer de software substitui somente a fonte do plano nativo.

Com `BELLATRIX_RTG=1`, a build deixa o driver em
`emu68/install-bellatrix-rigel/VideoCore/VideoCore.card`. Ele ainda precisa ser
copiado para `LIBS:Picasso96/` no volume de sistema Amiga.

---

## Histórico (premissa superada) — portar a `bellatrix.card` para bare-metal

Confirmado — o toggle da TUI hoje é um no-op no bare metal. Rastreei o caminho
completo:

O que o toggle faz hoje: a TUI grava BELLATRIX_RTG no env file e passa
BELLATRIX_RTG=1 HARNESS_RTG=1 pro comando (tui.go:727). No perfil harness
funciona: run.sh:429/502 mapeia BELLATRIX_RTG→HARNESS_RTG, e
tools/harness/main.c:1076 chama bellatrix_rtg_register(). No perfil bellatrix,
run.sh:582 exporta a variável para o build — mas scripts/build.sh nunca lê
BELLATRIX_RTG, então ela morre ali.

O que falta para o path bare metal (4 peças):

1. Build: rtg.c + rtg_rom_data (o blob gerado por build-rtg-rom.sh) só existem no
   tools/harness/CMakeLists.txt. O cmake/bellatrix-variant.cmake compila a
   expansão lide mas não a rtg — precisaria replicar o padrão do lide_rom_data.
2. Registro: bellatrix_rtg_register(m) só é chamado no harness. No bare metal o
   ponto natural é machine_rigel.c (mesmo padrão do lide_cdrom_register), gated
   por um define de build.
3. Frame tick: bellatrix_rtg_frame_tick() é chamado no loop do harness; no bare
   metal precisa de um hook por VBL.
4. Saída de vídeo: no bare metal a saída atual é o rigel compondo direto no
   framebuffer VC4 em RGB565 (machine_rigel.c:228). O backend precisaria escanear
   a VRAM do board para o VC4 quando o RTG ligasse.

Duas ressalvas (ainda válidas como referência): perf no Emu68 (VRAM tem que ser
mmu-mapeada direto, não via fault) e ordem (resolver OpenScreen antes da saída).

> A decisão de 2026-07-18 tornou este caminho secundário: em vez de portar a
> nossa card e reimplementar a saída VC4, usamos a `VideoCore.card` do Emu68, que
> já resolve a saída VC4 de forma comprovada. As 4 peças acima continuam válidas
> caso um dia se queira a nossa card unificada nos dois alvos.
