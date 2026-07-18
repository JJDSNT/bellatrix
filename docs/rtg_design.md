# Bellatrix RTG — arquitetura e plano

## Direção atual (decisão de 2026-07-18)

### Nome visível e ativação no harness

Com `HARNESS_RTG=1`, a placa se chama `Bellatrix RTG` e o driver P96 no
guest se chama `bellatrix.card`. O harness anuncia a intenção imediatamente:

```
[HARNESS] RTG enabled: Bellatrix RTG (bellatrix.card), Zorro III 8MB
```

Mais tarde, `[RTG-DBG]` comprova a execução do loader/driver e
`[RTG] enable=1 ...` comprova que o sistema abriu um modo no scanout. São
etapas distintas: registrar a placa não significa que uma tela já foi aberta.

O wrapper `run.sh` preserva um `HARNESS_RTG` fornecido explicitamente. Houve
um bug em que carregar o perfil salvo do launcher (`BELLATRIX_RTG=0`)
sobrescrevia `HARNESS_RTG=1 ./run.sh harness`, fazendo o próprio log informar
`RTG disabled`; essa precedência estava errada e foi corrigida.

Na TUI de `run.sh`, a opção aparece como
`[R] Bellatrix RTG (P96 framebuffer)`; a tecla `R` alterna `ON/OFF` e a seleção
é emitida como `BELLATRIX_RTG`, sendo convertida em `HARNESS_RTG` no harness.

### Caminho rápido da VRAM no harness

Depois que o guest atribui a base Z3, registradores e ROM continuam no caminho
MMIO com efeitos colaterais. No primeiro acesso ao framebuffer, somente a faixa
de VRAM alinhada a página é promovida a uma região direta do backend CPU:

```
40000000-40002fff  registradores + DiagArea/card ROM (MMIO)
40003000-407fffff  VRAM linear (DIRECT)
```

Assim, clears, cópias e desenho do P96 deixam de atravessar o bridge e o loop
byte a byte para cada acesso. O primeiro acesso ainda é concluído pelo caminho
MMIO que instala o mapeamento; os seguintes usam a memória direta. Reset remove
a região antes de uma nova enumeração. O log confirma a promoção com
`[RTG] direct VRAM mapped: ...`.

O VSync do SDL não era o gargalo observado: uma execução real confirmou
`requested_vsync=off active_vsync=off` sem eliminar o desenho progressivo. O
profiler atribuiu cerca de 98,7% do tempo ao `cpu_run`. O hook de instrução do
Musashi, antes instalado mesmo sem diagnóstico ativo, passou a ser opt-in. No
mesmo boot de 1.000 frames isso elevou a vazão aproximada de 24–25 para 27–31
milhões de ciclos guest/s. `HARNESS_CPU_QUANTUM=4096` reduz adicionalmente o
número de chamadas ao Musashi, mas permanece opção de laboratório porque um
quantum maior altera a granularidade temporal do chipset.

## Plano de aceleração dirigido pelo código do AROS

A lista completa, incluindo recursos não exercitados pelo AROS atual, vive em
[`rtg_acceleration_matrix.md`](rtg_acceleration_matrix.md). Este documento
mantém arquitetura e sequência; a matriz é o checklist implementado/não
implementado operação por operação.

Pesquisa local em `external/aros/arch/m68k-amiga/hidd/p96gfx/` confirmou o
fluxo real, sem depender de inferência sobre buffering:

- `p96gfx_bitmapclass.c::FillRect` tenta `BoardInfo.FillRect` quando o bitmap
  está na VRAM; se o callback não deixa `AROSFlag` como tratado, executa
  `HIDD_BM_FillMemRect*` pela CPU.
- `p96gfx_hiddclass.c::CopyBox` tenta `BlitRectNoMaskComplete` quando origem e
  destino estão na VRAM; ao receber não tratado, cai no superclass/software.
- `p96gfx_rtg.c::P96GFXRTG__Init` preenche callbacks ausentes com
  `RTGCall_Default`, que limpa `AROSFlag` e força esse fallback.
- nossa `bellatrix.card` não instala callbacks de desenho e ainda anuncia
  `BIF_NOBLITTER`; logo clears, layers e movimentos de janela percorrem o
  68040 emulado e podem permanecer visíveis por vários frames.
- `SetPanning` é chamado ao mostrar/trocar a tela para selecionar
  `VideoData`, largura e offsets. Ele permite page flip quando há bitmaps
  distintos, mas não transforma o desenho normal do Wanderer em double buffer.

### Fase A — medir os fallbacks antes de acelerá-los

1. Instalar callbacks-probe para `FillRect`, `BlitRect`,
   `BlitRectNoMaskComplete`, `InvertRect`, `BlitTemplate`, `BlitPattern` e
   `DrawLine`.
2. Cada probe registra contagem, formato, dimensões, máscara/minterm e se os
   `RenderInfo.Memory` estão dentro da VRAM; inicialmente limpa `AROSFlag` para
   manter o fallback correto.
3. Produzir um resumo por operação, não log por chamada, e capturar um boot do
   AROS até o desktop. Isso determina a ordem pelo volume real, não por palpite.

**Executada em 2026-07-18.** Os callbacks-probe chamam seus respectivos
`*Default`, portanto preservam `AROSFlag=0` e o resultado software. O log é
amostrado na primeira chamada e em potências de dois. No boot
`aros.rom + aros.adf`, até 1.600 frames, observamos:

```
FillRect                 count >= 16; primeira operação 640x480 CLUT mask ff
BlitTemplate             count >= 1;  128x8 CLUT mask ff
BlitRectNoMaskComplete   count >= 2;  16x16 CLUT opcode 0x0c
```

O clear inicial de toda a tela é explicitamente um `FillRect` recusado pela
card e executado em software. Isso explica diretamente o preenchimento visível
linha a linha e torna `FillRect` o primeiro alvo da Fase C. Não foram observados
`InvertRect`, `BlitRect`, `BlitPattern` ou `DrawLine` nessa janela curta; isso
não os declara desnecessários, apenas reduz sua prioridade inicial.

### Fase B — command ABI síncrona e segura

Adicionar ao register file um bloco de comando com opcode, offsets de
origem/destino relativos à VRAM, pitches, coordenadas, largura/altura, pen,
máscara, minterm e formato. O host valida overflow, pitch, formato e limites
antes de tocar a memória. A primeira versão é síncrona: escrever `COMMAND`
termina a operação antes do retorno do callback; `WaitBlitter` continua no-op.
Isso evita fila, IRQ e condições de corrida enquanto o contrato é estabilizado.

### Fase C — operações de maior retorno

1. **FillRect:** CLUT, R5G6B5 e A8R8G8B8; máscara `0xff` primeiro. Só então o
   callback deixa `AROSFlag=1`. Máscaras parciais não suportadas retornam ao
   software.
2. **BlitRect:** cópia no mesmo bitmap, incluindo overlap com semântica de
   `memmove` por linha e direção vertical correta.
3. **BlitRectNoMaskComplete:** começar por COPY entre dois `RenderInfo` na
   VRAM; minterms restantes continuam recusados até terem oráculos.
4. **InvertRect:** operação simples e frequente, respeitando máscara/formato.
5. Só depois avaliar `BlitTemplate`, `BlitPattern` e `DrawLine`, guiado pelas
   contagens da Fase A.

**Primitivas implementadas:** `FillRect` CLUT, RGB565 e ARGB32 com máscara
`0xff` usa uma command ABI síncrona (`DST`, pitch, XY, WH, color, format/mask,
`COMMAND` e `STATUS`). `BlitRectNoMaskComplete` aceita o minterm `0x0c`, que o
`modetable` do AROS confirma como `Copy`, e executa cópia overlap-safe entre
`RenderInfo` em VRAM nos mesmos três formatos. A `.card` e o host validam
ponteiros, formatos, pitches e bounds; qualquer rejeição chama o callback
`Default`. Máscaras parciais de `FillRect` continuam explicitamente em software.
`BlitRect` reutiliza o mesmo motor para COPY dentro de um único `RenderInfo`,
com máscara `0xff`; sobreposição vertical e horizontal é preservada.

`BlitTemplate` transporta a máscara 1-bit por uma janela de upload da board,
em vez de o host acessar diretamente Chip/Fast RAM. O desenho mantém a command
ABI independente do backend CPU e cobre JAM1/JAM2, INVERSVID e os três formatos.

`BIF_NOBLITTER` só será removido e `BIF_BLITTER` anunciado quando FillRect e os
casos de cópia prometidos estiverem testados. Cada callback não suportado deve
limpar `AROSFlag`, preservando o fallback do AROS em vez de produzir corrupção.

### Fase D — apresentação e buffering

Instrumentar `SetPanning` para distinguir troca real de `VideoData` de simples
mudança de offset. Se o AROS usar bitmaps alternados, o registrador `PAN` já é o
ponto de page flip e deve ser aplicado atomicamente no VBlank. Se ele desenhar
no bitmap visível, buffers duplos/triplos criados apenas pelo host capturariam
os mesmos estados parciais e não corrigiriam a causa. Shadow/coalescing fica
como opção posterior, nunca como substituto da aceleração P96.

### Oráculos e critérios

- testes unitários host para bounds, overlap, pitches, máscaras e três formatos;
- teste ABI m68k que confirma offsets e convenção de registradores dos callbacks;
- integração AROS exigindo breadcrumbs/contadores de cada operação acelerada;
- screenshots determinísticas antes/depois de clear, scroll e janela;
- benchmark fixo reportando ciclos guest/s, frames até modo ativo e tempo de
  uma sequência de FillRect/BlitRect; nenhum ganho será aceito apenas por
  esconder frames intermediários.

O RTG do Bellatrix será uma placa P96 de framebuffer linear, portátil entre os
backends. O driver guest e o contrato de scanout não conhecerão SDL, VideoCore
ou mailbox:

```
AmigaOS/AROS + P96
        │
        ▼
bellatrix.card (guest m68k)
        │  VRAM linear + registradores mínimos
        ▼
bellatrix.rtg (board Z3 EXTERNAL)
        ├── harness: scanout → SDL/screenshot
        └── Raspberry: scanout → presenter disponível
                         (o caminho VC4/Emu68 é futuro e independente)
```

O ponto de partida comportamental é o `MiSTer.card.asm` do
[Minimig-AGA MiSTer](https://github.com/MiSTer-devel/Minimig-AGA_MiSTer/tree/MiSTer/extra/rtg_driver).
Ele prova que um P96 útil pode operar com VRAM direta e poucos controles:
endereço do bitmap, formato, enable, largura, altura, stride e paleta. Blitter,
sprite, interrupção VBL e clocks reais não fazem parte do primeiro marco.

`external/VideoCore.card` não é a base desta fase. Ele permanece como referência
futura para uma saída Raspberry, que poderá inclusive ser resolvida pelo próprio
Emu68. O contrato RTG deve continuar funcionando sem ele no harness.

## O que já existe e será preservado

- `src/machine/expansions/rtg/`: board Z3, VRAM linear, registradores, conversão
  CLUT/R5G6B5/A8R8G8B8 para RGBA e acesso ao frame.
- `cards/bellatrix.card`: `FindCard`, `InitCard` e callbacks P96 de framebuffer
  simples.
- DiagArea/CardLoader: a cadeia foi validada até a biblioteca entrar na LibList,
  `FindCard` encontrar a placa e `InitCard` executar.
- Harness: ativação por `HARNESS_RTG=1`, tick de frame e preferência do RTG nas
  screenshots.
- `board_registry`: o RTG já foi migrado para uma board Z3 EXTERNAL e não depende
  das registries Zorro antigas.

O laboratório anterior não produziu scanout funcional, mas produziu evidência
útil. Seu último bloqueio conhecido ocorreu depois de `InitCard`: o guest não
chamou `SetSwitch`, `SetGC` nem `SetPanning`. Portanto a próxima investigação é
seleção/configuração P96, não implementação de VC4.

Estado atualizado após a correção Z3: o AROS já percorre toda a cadeia e programa
um scanout `640x480 CLUT`, stride 640, pan 0 e enable 1. A frase anterior é
preservada como transição histórica; a correção detalhada está abaixo.

A janela atual é Z3 de **8 MB** (8180 KB úteis após registradores/ROM). A ampliação
de 4 para 8 MB acompanha o modelo MiSTer e permite comportar o maior modo da tabela
AROS atual (`1440x900x32`, cerca de 5,2 MB) sem reduzir a Fast RAM Z2.

## Matriz mínima de contrato P96

| Aspecto | MiSTer.card | bellatrix.card atual | AROS p96gfx.hidd |
|---|---|---|---|
| VRAM | 8 MB linear fixa | ~8 MB linear via AutoConfig Z3 | usa `MemoryBase/MemorySize` |
| Descoberta | endereços fixos | `FindConfigDev` + magic | procura `*.card`, chama LVO 5/6 |
| Formatos iniciais | CLUT, 16, 24 e 32-bit | CLUT, R5G6B5, A8R8G8B8 | cria um modo por classe de profundidade |
| `SetSwitch` | escreve enable, retorna inverso | escreve enable, retorna inverso | chama após panning; retorno não usado nesse fluxo |
| `SetGC` | largura, altura e stride | largura e altura | fornece `ModeInfo` da tabela interna |
| `SetPanning` | publica endereço físico | publica offset relativo + formato/stride | fornece bitmap, width e offsets |
| VBL | callback no-op | callback no-op | interrupção não é requisito sem flag VBL |
| Blitter/sprite | desabilitados | desabilitados | fallback em software/CPU |
| Layout `BoardInfo` | ABI P96 | `_Static_assert` nos offsets usados | acessa offsets fixos em bytes |

`WaitVerticalSync` era busy-wait no Bellatrix. Isso poderia deadlockar o harness,
pois o contador VBL só avança no loop host que ficaria bloqueado dentro do callback.
Ele agora segue o MiSTer e é no-op até existir uma fonte de sincronização que não
dependa do chamador bloqueado.

## Plano de execução

### Fase 0 — congelar o contrato e criar oráculos

1. Comparar callback a callback `MiSTer.card.asm`, `bellatrix.card` e o consumidor
   AROS `p96gfx.hidd`.
2. Documentar a semântica exata dos registradores, especialmente formato, retorno
   de `SetSwitch`, stride e offset relativo escrito por `SetPanning`.
3. Adicionar testes host do register file e do scanout com padrões pequenos para
   CLUT, RGB565 e 32-bit, incluindo pan, stride e limites de VRAM.
4. Remover a premissa obsoleta de que RTG Z3 reduz Fast RAM Z2.

**Pronto quando:** os testes provarem o contrato sem inicializar AmigaOS/AROS e
os valores escritos pela `.card` tiverem correspondência explícita com o modelo
MiSTer.

Estado em 2026-07-18: o núcleo `rtg_scanout`, seus testes, a matriz mínima, os
asserts de ABI e o roteamento Z3 estão concluídos. O AROS programa o primeiro modo
e o presenter do harness prefere o scanout RTG quando ativo. A fase seguinte deve
produzir conteúdo/desktop não uniforme e validar visualmente a janela SDL.

### Correção de diagnóstico: a suposição de mode matching estava errada

O histórico abaixo dizia que a cadeia chegava a `InitCard`, mas não a `SetGC`, e
tratava mode matching, monitor default ou VBL como hipóteses principais. Essa
conclusão ficou errada depois da migração do RTG de Z2 para Z3.

O backend Musashi continha um retorno antecipado que classificava **todo** acesso
acima de `0x00ffffff` como open bus. O AutoConfig Z3 funcionava e atribuía
`0x40000000`, mas as leituras seguintes da DiagArea, da card, dos registradores e
da VRAM eram descartadas antes de `machine_dispatch`. O log decisivo foi:

```
Read boot ROM base=40000000
da_Config=f0                 # open bus (0xff), não a ROM
```

Depois de encaminhar janelas Z3 EXTERNAL configuradas antes do fallback open bus,
a mesma ROM passou imediatamente pela cadeia completa:

```
da_Config=90
CardLoader → FindCard → InitCard
SetColorArray → SetGC 640x480 → SetPanning → SetSwitch(enable=1)
```

Portanto, a ausência histórica dos callbacks não era evidência de rejeição de
modo pelo P96. Era uma quebra de roteamento host introduzida/manifestada quando a
board foi movida para o espaço de 32 bits. Endereços Z3 sem owner continuam open
bus; somente uma janela EXTERNAL configurada é encaminhada.

Durante a mesma auditoria, asserts iniciais de layout da `BoardInfo` também
falharam. Isso não revelou uma incompatibilidade: os valores esperados haviam
sido calculados 10 bytes adiante por contagem duplicada de uma tabela `MAXMODES`.
Uma medição com a toolchain m68k confirmou que a struct já coincidia com os
offsets do AROS (`PixelClockCount=254`, `SetSwitch=282`, `SetGC=294`,
`SetPanning=298`). Os asserts corrigidos permanecem como oráculo de ABI.

### Fase 1 — primeiro caminho P96 comprovado no harness

1. Alinhar `bellatrix.card` ao conjunto mínimo comprovado do MiSTer, sem portar
   seus endereços físicos fixos: `MemoryBase` e `RegisterBase` continuam vindos
   do AutoConfig; `SetPanning` publica offset relativo à VRAM.
2. Começar com um modo conservador e formatos CLUT/RGB565; ampliar somente após o
   primeiro scanout.
3. Validar inicialmente com AmigaOS + Picasso96 instalado por disco, separando a
   ABI `.card` do fluxo precoce específico do `p96gfx.hidd` do AROS.
4. Instrumentar uma única vez a sequência `FindCard → InitCard → SetGC → SetDAC
   → SetPanning → SetSwitch`, sem logs por pixel/acesso de VRAM.
5. Apresentar o frame RTG continuamente na janela SDL, não apenas em screenshot.

**Pronto quando:** Workbench/P96 produzir um desktop RTG visível e uma screenshot
não uniforme no harness.

### Fase 2 — AROS e ArosOne

1. Concluído: cadeia DiagArea/CardLoader, descoberta e seleção de modo AROS base.
2. Bootar mídia AROS adequada e obter conteúdo RTG não uniforme no harness.
3. Validar visualmente a apresentação SDL e a troca Denise ↔ RTG.
4. Abrir o desktop da distro ArosOne RTG-only e exercitar modos 16/32-bit.

**Pronto quando:** ArosOne renderizar no harness em resolução superior a PAL.

### Fase 3 — backend Raspberry

Consumir o mesmo estado de scanout no presenter disponível no hardware. A escolha
entre presenter Bellatrix, integração Emu68 ou `VideoCore.card` não altera a ABI
guest nem bloqueia as fases 0–2.

## Restrições de desenho

- RTG continua sendo board de expansão; não entra na Denise.
- O driver guest não contém código SDL, VC4 ou mailbox.
- Endereços físicos do MiSTer (`0x02000000`, `0x00b80100`, `0x27000000`) não são
  copiados. A semântica é reutilizada, não o mapa físico do FPGA.
- Sem blitter e sprite de hardware até medições justificarem aceleração.
- O primeiro alvo é correção e observabilidade, não quantidade de modos.
- O histórico abaixo permanece como registro do laboratório que levou a esta
  arquitetura.

---

## Histórico do laboratório iniciado em 2026-07-03

> Esta seção descreve o desenho e o estado observados durante o primeiro
> experimento. Alguns detalhes, como a caracterização original como Zorro II e a
> ligação antecipada ao VC4, foram superados. Eles são mantidos para não apagar o
> raciocínio e as evidências da investigação.

Status: fase 2 em andamento (2026-07-03). Issue: AI_context ISSUE-0033.
DiagArea + CardLoader residência CONFIRMADA funcionando (host-side
instrumentation); FindCard/p96gfx ainda não confirmado — ver ISSUE-0033
"Implementação fase 2" para o estado exato e o próximo passo.

## Arquitetura

Um único driver guest e uma única "placa" com dois backends de host:

```
AROS p96gfx (ROM, driver com source em external/aros .../hidd/p96gfx)
      │  procura libraries "*.card" na LibList do exec
      ▼
bellatrix.card (m68k, nosso; romtag no ROM de expansão, como o ODFS)
      │  programa registradores MMIO da placa
      ▼
board Zorro II "bellatrix.rtg"  (src/machine/expansions/rtg/)
      ├── backend harness   → VRAM em buffer host, blit p/ SDL + screenshots
      └── backend baremetal → VRAM blitada para o framebuffer VC4
                              (mesmo caminho de vídeo já usado pela Denise)
```

- `external/VideoCore.card` é **referência** da API P96 (BoardInfo, ModeInfo,
  ResolutionsList) — não é usado diretamente: ele fala mailbox do Pi via
  Emu68; nossa placa abstrai isso dos dois lados.
- O caminho uaegfx/romvector do UAE (traps em 0xF00FF60) foi descartado:
  exigiria interceptação de A-line no JIT do Emu68 no baremetal.

## Board "bellatrix.rtg" (Zorro II)

Autoconfig: mfr 0x07DB, prod 0x10, janela **4MB** (AC_SIZE_4MB).
Convive com fast RAM Z2 de 4MB (não cabem 8MB fast + VRAM no espaço Z2;
quando `HARNESS_RTG=1`, o fast reduz para 4MB — ver ISSUE-0032 para
mover a folga para Z3 futuramente).

Layout da janela (offsets a partir da base do board; redesenhado na
fase 2 para caber a DiagArea+card, que juntos passam de 9KB):

| Offset | Tamanho | Uso |
|---|---|---|
| 0x0000–0x00FF | 256B | registradores |
| 0x0100–0x2FFF | 11.75KB | ROM: DiagArea+CardLoader em 0x100, card hunk em 0x2000 |
| 0x3000–0x3FFFFF | ~4MB | VRAM linear (MemoryBase = base+0x3000) |

Registradores (32-bit, big-endian; RO = read-only):

| Reg | Offset | Descrição |
|---|---|---|
| ID | 0x00 | RO, magic 0x42525447 'BRTG' |
| VERSION | 0x04 | RO, versão da spec (1) |
| VRAM_OFF | 0x08 | RO, offset da VRAM na janela (0x1000) |
| VRAM_SIZE | 0x0C | RO, bytes de VRAM |
| ENABLE | 0x10 | 1 = RTG dono da saída (SetSwitch); 0 = Denise |
| MODE_W | 0x14 | largura em pixels |
| MODE_H | 0x18 | altura em pixels |
| FORMAT | 0x1C | RGBFTYPE (p96gfx_rtg.h): 1=CLUT, 10=R5G6B5, 6=A8R8G8B8 |
| BYTES_PER_ROW | 0x20 | stride |
| PAN | 0x24 | offset do framebuffer visível dentro da VRAM |
| PAL_INDEX | 0x28 | índice de paleta (escrita reseta latch) |
| PAL_DATA | 0x2C | 0x00RRGGBB; escrita incrementa PAL_INDEX |
| VBLANK | 0x30 | RO, contador de frames (poll barato p/ WaitVerticalSync) |

Semântica: a placa não tem blitter (BIB_NOBLITTER — p96gfx faz fallback
por CPU na VRAM, que é RAM linear). Sprite de hardware: não (fase 1).

## bellatrix.card (m68k)

- Compilado com a toolchain docker já usada (lide/ODFS):
  `scripts/build-rtg-rom.sh`, embutido no harness via CMake
  (`rtg_rom_data`, mesmo padrão de `lide_rom_data`).
- Implementa o conjunto que p96gfx_card.c chama: FindCard, InitCard
  (popula ResolutionsList com modos: 640x480, 800x600, 1024x768 em
  CLUT/16/32bpp), SetSwitch, SetGC, SetPanning, SetColorArray, SetDAC,
  CalculateBytesPerRow, CalculateMemory, GetCompatibleFormats,
  SetMemoryMode, SetWriteMask/SetClearMask/SetReadPlane (no-ops),
  WaitVerticalSync (poll VBLANK), SetInterrupt (no-op), GetPixelClock/
  ResolvePixelClock (tabela fixa 60Hz), WaitBlitter (no-op).
- FindCard localiza o board via expansion.library (FindConfigDev
  mfr 0x07DB / prod 0x10), preenche MemoryBase/MemorySize/RegisterBase.

## Residência (fase 2): DiagArea própria, sem depender do lide

`p96gfx.hidd` roda em `residentpri -10` — antes do boot de disco — então
a card precisa estar na LibList antes disso. Solução:
`cards/bellatrix.card/bootrom/cardldr.S` é um loader estilo Chainloader
do lide (mesmo padrão, GPL-2.0 do lide.device como referência): vive na
própria DiagArea do board RTG (`AC_TYPE_DIAGVALID`, InitDiagVec=0x0100),
é copiado para RAM e `InitResident`ado pelo AROS automaticamente
(RTF_COLDSTART); seu próprio `Init:` então relocaliza (`_relocate`,
também copiado do lide) o hunk do `bellatrix.card` a partir de
offset 0x2000 e o `InitResident`a por conta própria.

**Dois bugs reais corrigidos aqui** (não teóricos — confirmados via
instrumentação): `ripple_bus_owns()` do lide reivindicava QUALQUER board
Z2 configurado, não só o seu; e `defs.i` tinha os bits de
DAC_BUSWIDTH/DAC_WORDWIDE errados (nibble baixo em vez de alto). Ver
ISSUE-0033 "Implementação fase 2" para os detalhes e o estado exato
(DiagArea+CardLoader confirmados funcionando; FindCard do p96gfx ainda
não confirmado).

## Fases

1. **Harness — descoberta guest**: board + DiagArea + CardLoader
   confirmados. Falta confirmar FindCard/SetSwitch do p96gfx e ver
   pixels na tela via RTG.
2. **Harness — saída ao vivo**: hoje só screenshot; adicionar blit
   contínuo pro SDL quando ENABLE=1.
3. **Baremetal**: backend VC4 do board — blit VRAM→framebuffer Pi e
   arbitragem Denise×RTG pelo ENABLE.
4. Opcional: VRAM em Z3 (ISSUE-0032), hardware sprite, blitter accel.
