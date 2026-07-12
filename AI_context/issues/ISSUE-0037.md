---
id: ISSUE-0037
title: "Bare-metal: saida de video Amiga fica preta/instavel depois do launcher"
status: open
priority: high
type: bug
owner: agent
created_at: 2026-07-05
updated_at: 2026-07-05
tags:
  - baremetal
  - video
  - denise
  - framebuffer
  - raspberry-pi
  - regression
related_files:
  - src/machine/machine_rigel_step.c
  - src/machine/machine_rigel.c
  - src/cpu/emu68/bellatrix.c
  - src/cpu/musashi/musashi_backend.c
  - src/cpu/musashi/musashi_baremetal_config.h
  - cmake/bellatrix-variant.cmake
  - scripts/build.sh
  - external/rigel/src/core/rigel_denise_api.c
  - external/rigel/src/chipset/denise/output/framebuffer.c
  - external/rigel/src/chipset/denise/render/compositor.c
---

# Estado atual

## CAUSA RAIZ ENCONTRADA E CORRIGIDA (2026-07-05, tarde)

A "tela indo e voltando" com intervalo longo nao e bug de video: e o **strap do KS 1.3
refazendo o boot em loop** por causa de eventos fantasma de disk-change.

Cadeia completa:

1. Disassembly de `fe84e0-fe86ee` (KS13): a rotina que escreve `DMACON=0100/8100` e o boot
   strap. `A1 = A5+0x2C` e um `IOStdReq` do trackdisk.device; `A5+0x4C` e o `io_Actual`
   retornado por **TD_CHANGENUM** (cmd 13). O loop estavel em `fe8638` faz polling de
   TD_CHANGENUM esperando o contador mudar (= disquete inserido). Quando "muda", volta a
   `fe8524`: BPLEN off -> tenta ler boot block -> falha (sem disco) -> redesenha -> BPLEN on.
2. No QEMU/bare-metal, `[VIDEO-BOOT-DMA]` mostrou D2 (contador TD_CHANGENUM) incrementando
   **+2 por passada** (3, 5, 7, ... 0x27). Cada tentativa de boot gerava um par
   "inseriu/removeu" fantasma. No harness, D2 fica em 0 -> tela estavel.
3. Instrumentacao `[CHNG-R]`/`[PRB-W]` no bus flagrou a origem:

   ```text
   [PRB-W]  prb=f7 mtr=0 sel0=1 pc=00fe9694 idcnt=4
   [CHNG-R] pra=fc chng=0->1 pc=00fe969e idcnt=4 chgd=1 mtr=0
   ```

   O check de disk-change do trackdisk (step + leitura de /CHNG) cai logo depois de um ciclo
   motor on->off, que zera `id_count`. Com `id_count<32`, drive selecionado e motor off, o
   Rigel entrava em "drive ID mode" e colocava o **bit de ID (=1) na linha /CHNG**, mascarando
   o latch `disk_changed=1`. HIGH = "disco estavel" -> insercao fantasma; janela fecha -> LOW
   -> remocao fantasma.
4. **No hardware real o ID do drive e deslocado na linha /RDY (PA5), nunca em /CHNG (PA2)**
   (AHRM; vAmiga `Drive::driveStatusFlags` — o comentario no Rigel atribuia o comportamento
   ao vAmiga incorretamente).
5. O harness nao pisca por sorte de interleaving: quando o trackdisk le /CHNG la, `id_count`
   ja passou de 210 (cada write em PRB com drive deselecionado incrementa), entao a janela de
   ID ja fechou. Mesmo codigo, timing diferente -> bug latente que so o bare-metal expoe.

Fix aplicado em `external/rigel` (codigo compartilhado harness+bare-metal):

- `src/core/rigel_cia_api.c` (`cia_b_prb_update_floppy`): ID mode passou a sobrepor **/RDY**;
  /CHNG passou a refletir sempre o latch de disk-change (open-drain entre drives conectados).
- `src/core/rigel.c` (`rigel_sync_floppy_cia_lines`): mesma correcao.
- `src/floppy/floppy_drive.c` (`floppy_init`): drive vazio conectado nasce com change latch
  pendente (`disk_changed=1`, /CHNG LOW no power-on, como hardware real) — remove a transicao
  fantasma inicial que semeava D2=1.

Validacao:

- Testes do Rigel: 26/26 passam.
- Harness KS13 68000 sem disco (1000 frames): assinatura identica ao baseline (strap roda uma
  vez, BPLEN on estavel, D2=0, /CHNG cravado LOW).
- Harness KS13 68000 + Workbenc13.adf (2500 frames): boota ate o Workbench (copper escreve
  `A302`, PC no idle do exec). Caminho de insercao de disco intacto.
- Harness KS20 68000 sem disco (1000 frames): sem regressao, /CHNG estavel.
- QEMU bare-metal Musashi 68000 KS13 (launcher/USB/BT off, 7 min, >1280 frames):
  - strap roda **uma unica vez** (`[VIDEO-BOOT-DMA]`: 2x `0100` + 1x `8100`, `d2=0` estavel —
    assinatura identica ao harness; antes: dezenas de passadas com d2=3,5,...,0x27);
  - `[CHNG-R]`: 2 amostras (init + transicao legitima para LOW), sem flapping;
  - tela do hand estavel do frame ~401 ate o fim: `dmacon=03d0 bplen=1`,
    `sig=b81898af nonbg=73/1024` constante nos frames 512/768/1024/1280
    (antes: sig alternava a cada ~30 frames).

Nota: a oscilacao dos low bits do BPLCON0 (`0200/2200` vs `0302/2302`) observada antes era
consequencia do mesmo loop de strap (estado de boot diferente a cada passada), nao causa.

Nota 2: o avanco de chipset em lotes fixos de 512 CCK foi removido durante a investigacao
para eliminar suspeitos, e **permanece removido**: com o fix do /CHNG o bare-metal ficou
estavel usando apenas o quantum baseado em deadline (`machine_next_quantum()`:
`rigel_get_next_deadline`/`next_bus_change`, cap de 1 frame PAL, min 8 CCK). Nao reintroduzir
o lote de 512 CCK sem nova motivacao de performance medida.

## Atualizacao 2026-07-05

Log do Pi real com a instrumentacao `[VIDEO-BM]` mostrou que o presenter bare-metal esta recebendo e
copiando frames para o framebuffer VC4 (`fb=1920x1080 pitch=3840 zero_copy=0`). O conteudo, porem, e
apenas fundo:

```text
[VIDEO-BM] frame=16 src=256x256 ... bplcon0=0200 dmacon=0000 diw=0000/0000 ddf=0000/0000 color00=0444 px=00444444/00444444
[VIDEO-BM] frame=64 src=256x256 ... bplcon0=0200 dmacon=0200 diw=0000/0000 ddf=0000/0000 color00=0888 px=00888888/00888888
[F-LINE-TRAP] pc=1fba00fc ir=ffff opcode_bytes=ffff ffff
[F-LINE-TRAP] pc=1fba0000 ir=ffff opcode_bytes=ffff ffff
```

Conclusao: este nao e o primeiro problema de Denise/presenter. A CPU 68k esta entrando em execucao de
`0xffff` antes de programar uma tela Amiga real. `BPLCON0=0200`, `DMACON=0200`, `DIW/DDF=0`, `flags=0`
e pixels todos iguais a `COLOR00` significam fundo cinza, sem bitplanes/copper ativos.

Fix aplicado para teste: `src/cpu/musashi/musashi_backend.c` agora replica explicitamente no backend
Musashi a janela baixa `0x000000-0x1fffff` que o caminho Emu68/JIT ja mapeia por MMU em
`bellatrix_init()`.

Motivo: o ROM do log reportou `ISP=0x11114ef9`; em bus 24-bit isso cai em `0x114ef9`, dentro da faixa
`1MB..2MB`. Antes, Musashi so tratava `0x000000-0x0fffff` como chip RAM e deixava `0x100000-0x1fffff`
cair no bus/chipset, que retornava open-bus `0xff`. Qualquer stack/exception nessa faixa podia
corromper controle de fluxo e gerar exatamente `ir=ffff`.

O diagnostico `[VIDEO-BM]` permanece, mas foi reduzido apos as primeiras 16 frames para uma amostra a
cada 256 frames, para limitar ruido no console.

Proximo criterio:

- se `[F-LINE-TRAP]` desaparecer e `DIW/DDF/DMACON` passarem a ser programados, o bug principal era a
  janela baixa ausente no Musashi;
- se `[F-LINE-TRAP]` persistir, capturar o novo `pc/ir/opcode_bytes` e correlacionar com os registros
  `[VIDEO-BM]`;
- se os registradores de video ficarem corretos mas a tela real continuar preta, voltar para presenter
  bare-metal/VC4.

## Builds bare-metal com modelo Musashi variavel

Adicionado suporte a selecionar o modelo 68k no build bare-metal Musashi:

```sh
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_MUSASHI_CPU=68000 ./scripts/build.sh
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_MUSASHI_CPU=68020 ./scripts/build.sh
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_MUSASHI_CPU=68040 ./scripts/build.sh
```

Valores validos: `68000`, `68010`, `68ec020`, `68020`, `68030`, `68040`. Default permanece `68040`.
O script tambem aceita `HARNESS_CPU` como fallback quando o launcher/TUI ja exporta essa variavel.

O boot runtime agora loga o modelo efetivo:

```text
[BELA] CPU backend: musashi (68000)
```

Isso permite testar se o `F-LINE-TRAP ir=ffff` e especifico do caminho `68040`/FPU/cache ou se tambem
ocorre com um 68000 simples. As builds `68040` default e `68000` foram validadas localmente.

## Resultado do teste 68000 no Pi real

Com `BELLATRIX_MUSASHI_CPU=68000`, o boot avancou alem do ponto do teste `68040`:

```text
[BELA] CPU backend: musashi (68000)
[VIDEO-BM] frame=256 ... bplcon0=0200 dmacon=0200 diw=0000/0000 ddf=0000/0000 color00=0888
[Z2] board 'bellatrix.fastram' assigned base=00200000 size=00800000
[Z2] all boards configured
[KBD] CPU read CIA-A SDR=0x74 pc=00fe528a
[VIDEO-BM] frame=512 ... flags=0x08 bplcon0=0200 dmacon=02d0 diw=0581/40c1 ddf=0038/00d0 color00=0fff
```

Achados:

- O `F-LINE-TRAP ir=ffff` observado no `68040` nao apareceu nesse trecho do `68000`.
- A Fast RAM Z2 foi configurada corretamente (`base=00200000 size=00800000`).
- O guest programou `DIW/DDF` e o frame tem `flags=0x08` (`RIGEL_FRAME_COPPER_ACTIVE`), entao o
  fluxo CPU/ROM/Copper esta avancando.
- Ainda nao ha evidencia clara de bitplane DMA ativo no ponto capturado: `BPLCON0=0200` e
  `DMACON=02d0` nao incluem `BPLEN=0x0100`; os pixels amostrados continuam iguais a `COLOR00`.

Conclusao atual: a regressao principal do `68040` e provavelmente especifica do modelo CPU/FPU/cache
do Musashi ou de instrucoes 020+/040; o problema de "tela indo e voltando" no `68000` parece ser uma
etapa posterior de video/boot, nao a mesma falha fatal `ir=ffff`.

Proximo teste util: capturar mais alguns `[VIDEO-BM]` depois do frame 512 no `68000`. Se
`BPLCON0/DMACON` nunca habilitarem bitplanes, investigar por que o boot sem disco fica apenas em
background/copper. Se habilitarem e a tela ainda oscilar, voltar ao presenter bare-metal.

## Reproducao no QEMU com KS13 e launcher desabilitado

Comando usado:

```sh
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_MUSASHI_CPU=68000 \
BELLATRIX_LAUNCHER=0 BELLATRIX_USBSTACK=1 BELLATRIX_BTSTACK=0 \
KICKSTART=src/roms/KS13.rom DISPLAY_MODE=none NO_TUI=1 \
./run.sh qemu
```

Depois, para evitar gastar o timeout recompilando, o QEMU foi chamado diretamente com a imagem
instalada.

Resultado: o comportamento de "aparece e some/volta" e reproduzivel no QEMU so com `KS13.rom`, sem
launcher. O ponto-chave nao e o presenter/VC4; os proprios registradores de video alternam:

```text
[VIDEO-BM] frame=295 ... flags=0x04 bplcon0=1200 depth=1 dmacon=03f0 bplen=1 diw=2c81/f4c1 ddf=0038/00d0
[VIDEO-BM] frame=304 ... flags=0x0c bplcon0=1200 depth=1 dmacon=03d0 bplen=1 diw=2c81/f4c1 ddf=0038/00d0
[VIDEO-BM] frame=417 ... flags=0x08 bplcon0=1200 depth=1 dmacon=02d0 bplen=0 diw=2c81/f4c1 ddf=0038/00d0
[VIDEO-BM] frame=477 ... flags=0x08 bplcon0=0200 depth=0 dmacon=02d0 bplen=0 diw=0581/40c1 ddf=0038/00d0
[VIDEO-BM] frame=478 ... flags=0x08 bplcon0=2200 depth=2 dmacon=02d0 bplen=0 diw=0581/40c1 ddf=0038/00d0
[VIDEO-BM] frame=564 ... flags=0x08 bplcon0=0200 depth=0 dmacon=03d0 bplen=1 diw=0581/40c1 ddf=0038/00d0
[VIDEO-BM] frame=566 ... flags=0x08 bplcon0=0200 depth=0 dmacon=02d0 bplen=0 diw=0581/40c1 ddf=0038/00d0
```

Leitura:

- O primeiro Workbench/KS display entra por volta do frame 295 (`depth=1`, `bplen=1`, DIW/DDF
  programados).
- Depois `BPLEN` cai (`dmacon=02d0`) e/ou `BPLCON0` alterna entre depth 0/2 enquanto a tela fica em
  fundo/cor sólida.
- Como isso acontece em QEMU/headless e os logs mostram mudanca nos registradores do chipset, a
  oscilacao nao parece ser bug de framebuffer VC4. O proximo alvo deve ser a origem dessas escritas:
  CPU/ROM escrevendo `DMACON/BPLCON0`, ou algum reset/reinit interno do chipset/memoria que publica
  estados errados.

Proximo passo recomendado: instrumentar escritas CPU/Copper em `BPLCON0` e `DMACON` com PC/origem
(`CPU` vs `Copper`) para saber quem desliga `BPLEN` e quem alterna `depth`.

## Comparativo harness vs bare-metal: divergencia real, mas nao causa final confirmada

O harness nao apresenta a oscilacao observada no bare-metal. A comparacao nao deve usar numero absoluto
de frame como evidencia, porque QEMU/bare-metal e harness avancam CPU/chipset em ritmos diferentes.
O comparativo confiavel e causal:

- quais PCs da Kickstart escrevem `DMACON/BPLCON0`;
- quais ponteiros de Copper (`COP1LC/COP2LC`) a CPU instala;
- quais valores a Copper list escreve em `BPLCON0`.

Instrumentacao adicionada:

```text
[VIDEO-W-CPU] reg=096/100 ... pc=...
[VIDEO-W-COPPER] reg=100 write=... cop_pc=... beam=... frame=... cyc=...
[VIDEO-COPPTR-CPU] reg=080/082/084/086/088/08a ... cop1=...->... cop2=...->...
```

Achado principal: havia uma divergencia real de dispatch de bus no backend Musashi bare-metal.

- Harness: somente `0x000000-0x0fffff` e chip RAM; `0x100000-0x1fffff` nao e chip RAM e cai no
  bridge/open-bus se nenhum dispositivo responder.
- Musashi bare-metal, apos o workaround do `F-LINE`/stack, estava tratando `0x000000-0x1fffff` como
  RAM baixa diretamente via `mem->chip_ram + addr`.

Isso fez a CPU enxergar uma area baixa continua de 2MB que o DMA/Rigel nao trata como chip RAM real
(`BELLATRIX_CHIP_RAM_SIZE` continua 1MB e a callback DMA usa `bellatrix_chip_read16`). O resultado era
boot/copper list divergente:

```text
# Antes do alinhamento com o harness:
[VIDEO-W-COPPER] reg=100 write=0200 cop_pc=00d168 ...
[VIDEO-W-COPPER] reg=100 write=2200 cop_pc=00d19c ...
[VIDEO-W-COPPER] reg=100 write=0200 cop_pc=00d1a4 ...
[VIDEO-BM] ... bplcon0=0200 depth=0 dmacon=02d0 bplen=0 ...
```

Depois de limitar o caminho rapido Musashi novamente ao tamanho real de chip RAM, o QEMU bare-metal
passou a seguir a assinatura esperada do harness:

```text
[VIDEO-W-COPPER] reg=100 write=0302 cop_pc=00d168 ...
[VIDEO-W-COPPER] reg=100 write=2302 cop_pc=00d19c ...
[VIDEO-W-COPPER] reg=100 write=0302 cop_pc=00d1a4 ...
```

O `cop_pc` absoluto ainda pode diferir do harness, mas os valores criticos escritos pela Copper list
voltaram a bater. O estado de frame tambem ficou mais saudavel antes da entrada da lista dinamica:

```text
[VIDEO-BM] frame=256 ... bplcon0=1302 depth=1 dmacon=03d0 bplen=1 ...
```

Conclusao revisada: o workaround de low RAM 2MB no backend Musashi bare-metal era uma divergencia real e
precisou ser removido/alinhado com o harness, mas ele **nao explica sozinho** a tela aparecer/sumir de
forma espacada. Depois do alinhamento, o QEMU voltou a produzir a assinatura esperada de Copper/boot
(`BPLCON0=1302/2302/0302` em vez de `0200/2200/0200`), mas ainda e preciso isolar o sintoma visual.

O estado `DMACON` com `BPLEN` limpo tambem nao deve ser tratado isoladamente como causa final: o harness
passa por uma escrita equivalente da Kickstart em `pc=00fe8552` e nao apresenta a mesma falha visual.
Portanto, comparar apenas "frame N" ou apenas o valor final de `DMACON` no fechamento do frame pode levar
a falso positivo. O comparativo confiavel continua sendo causal: writes de CPU/Copper, ponteiros de
Copper, assinatura do frame produzido por Rigel e o caminho de apresentacao.

A abordagem correta para memoria continua: nao fazer a CPU Musashi enxergar `0x100000-0x1fffff` como
chip/low RAM anonima. Se precisarmos de slow RAM nesse range, ela deve virar um recurso explicito e
coerente com o mapa de memoria/DMA, nao um alias direto de `chip_ram`.

## Hipotese atual: presenter bare-metal expondo fase de clear/copy

Nova diferenca concreta entre harness e Pi real:

- no harness SDL, `PAL_Video_Resize()` troca o framebuffer host para o tamanho do frame apresentado;
- no raspi3 bare-metal, `PAL_Video_Resize()` e stub e o presenter continua desenhando dentro do
  framebuffer fisico existente, normalmente `1920x1080`;
- antes do ajuste, `machine_present_frame_from_rigel()` limpava o framebuffer fisico inteiro para
  `src[0]` em todo frame e so depois copiava a imagem Amiga escalada.

Isso pode produzir uma tela que "bate" no hardware real mesmo quando o frame Rigel esta correto: o HDMI
esta lendo o mesmo framebuffer enquanto a CPU pinta tudo de fundo e depois redesenha a area ativa. O
harness/SDL tende a esconder isso porque atualiza uma textura e apresenta no final, e normalmente tambem
usa um buffer menor.

Ajuste experimental aplicado:

- `[VIDEO-BM]` agora inclui assinatura amostrada do frame Rigel: `sig=<hash> nonbg=<amostras>/<total>`.
  A ideia e distinguir "Rigel esta alternando para fundo" de "o frame Rigel esta estavel, mas a saida
  fisica pisca".
- o presenter bare-metal so limpa o framebuffer inteiro quando geometria, posicao, tamanho do framebuffer
  ou cor de fundo mudam. Em frames normais, ele sobrescreve a area ativa diretamente sem expor uma fase
  de clear full-screen.
- `[VIDEO-COPPTR-CPU]` foi reduzido para logar apenas mudancas reais de ponteiro ou `COPJMP`, evitando
  afogar o serial/QEMU com reescritas identicas.

Resultado local parcial:

```text
[VIDEO-BM] frame=214 ... bplcon0=1302 depth=1 dmacon=03f0 bplen=1 ... sig=3c328245
[VIDEO-BM] frame=223 ... bplcon0=1302 depth=1 dmacon=03d0 bplen=1 ... sig=24a524a5
[VIDEO-W-CPU] reg=096 before=03d0 write=0100 size=2 pc=00fe8552 ...
[VIDEO-BM] frame=336 ... bplcon0=1302 depth=1 dmacon=02d0 bplen=0 ... sig=24a524a5
```

O QEMU local ainda nao chegou rapido o bastante na janela > frame 500 dentro do timeout curto, mas ate
o frame 336 a assinatura do frame fica estavel quando a Kickstart limpa `BPLEN`. Isso reforca que
`BPLEN=0` no fim do frame nao e, por si so, a explicacao do piscar espacado.

## Atualizacao: comparativo valido na faixa 500+

O teste anterior ate frame 336 era insuficiente. Refeito com launcher e USB desabilitados, KS13 direta,
e QEMU deixando passar de frame 800.

Resultado: na faixa critica, o frame retornado por `rigel_get_frame()` ja alterna:

```text
[VIDEO-BM] frame=804 ... dmacon=03d0 bplen=1 ... sig=a8e329c5 nonbg=0/1024
[VIDEO-BM] frame=806 ... dmacon=02d0 bplen=0 ... sig=be53f8ef nonbg=71/1024
[VIDEO-BM] frame=833 ... dmacon=03d0 bplen=1 ... sig=a8e329c5 nonbg=0/1024
[VIDEO-BM] frame=835 ... dmacon=02d0 bplen=0 ... sig=b81898af nonbg=73/1024
```

Isso descarta a hipotese de que o sintoma seja apenas a fase de clear/copy do presenter. O presenter
ainda deve evitar clear full-screen por robustez/performance, mas a oscilacao principal ja esta no frame
Rigel produzido pelo caminho bare-metal/QEMU.

Desassemblagem KS13 da rotina:

```text
00fe854a: move.w  #$100, $dff096.l   ; clear BPLEN
...
00fe861a: move.w  #$8100, $dff096.l  ; set BPLEN
00fe8622: lea     ($2c,A5), A1
...
00fe864a: cmp.l   ($4c,A5), D2
00fe864e: beq     $fe8638
```

Harness vs QEMU/bare-metal:

- harness passa pela rotina e fica no polling estavel;
- QEMU/bare-metal retorna e repete `BPLEN off/on`, acompanhando a batida visual.

Instrumentacao `[VIDEO-BOOT-DMA]` mostrou uma divergencia de mapa de memoria:

```text
# harness
pc=00fe8622 ... a4=001558 a5=c014b6 a6=c00276 d2=00000000 a5_04=00c01e5e a5_4c=00000000

# QEMU antes de habilitar slow RAM C00000
pc=00fe8622 ... a4=002e20 a5=0018b6 a6=000676 d2=0000001b a5_04=0020055e a5_4c=00000000
```

O harness tem slow RAM compartilhada em `0xC00000-0xD7FFFF`; o target Emu68 legacy nao tinha
`BellatrixMemory.slow_ram` habilitado e ainda expunha uma janela low/slow legada. Foi feito um teste
habilitando slow RAM `0xC00000-0xD7FFFF` no target Emu68 legacy, usando backing fisico
`0xffffff9000c00000`.

Esse teste aproximou parte do estado:

```text
pc=00fe8622 ... a4=001558 a5=c014b6 a6=c00276 ... a5_04=00c01e5e
```

Mas **nao resolveu** a oscilacao. O QEMU continuou repetindo a rotina e, pior, a Copper list passou a
escrever `0200/2200/0200` em `BPLCON0`, enquanto o harness escreve `0302/2302/0302`:

```text
# QEMU apos slow RAM C00000
[VIDEO-W-COPPER] reg=100 write=0200 cop_pc=00b8a0 ...
[VIDEO-W-COPPER] reg=100 write=2200 cop_pc=00b8d4 ...
[VIDEO-W-COPPER] reg=100 write=0200 cop_pc=00b8dc ...

# harness
[VIDEO-W-COPPER] reg=100 write=0302 cop_pc=00b8a0 ...
[VIDEO-W-COPPER] reg=100 write=2302 cop_pc=00b8d4 ...
[VIDEO-W-COPPER] reg=100 write=0302 cop_pc=00b8dc ...
```

Conclusao revisada: a divergencia de slow RAM e real, mas ainda nao e a causa final isolada. O proximo
alvo deve ser comparar o conteudo da Copper list e os writes que a constroem no chip RAM (`0x00b8a0`
em harness, mesmo `cop_pc` no QEMU) para descobrir por que os low bits de `BPLCON0` (`0x0102`) somem no
caminho bare-metal/QEMU.

Depois da estabilizacao do console mini-UART (ISSUE-0036), o launcher bare-metal aparece e funciona,
mas a **tela Amiga** depois do handoff para a emulacao nao fica funcional no hardware real. No QEMU a
tela parece aparecer, sumir por um periodo e voltar em loop.

Importante: o problema relatado nao e o framebuffer/launcher inicial. O launcher desenha corretamente,
logo a cadeia HDMI/VC4 basica esta viva. A falha esta no caminho pos-launcher:

1. `launcher_run()` retorna;
2. `core_chipset_init()` inicializa/usa Rigel;
3. o backend 68k roda;
4. frames da Denise precisam chegar ao framebuffer VC4 via `machine_present_frame_from_rigel()`.

Houve avancos recentes no video do harness, mas esses avancos nao garantem o caminho bare-metal. O
harness usa SDL/textura e pode redimensionar/recriar o buffer sem envolver o framebuffer VC4 real. No
bare-metal, `machine_present_frame_from_rigel()` escreve diretamente nos globals do Emu68:

```c
extern uint16_t *framebuffer;
extern uint32_t pitch;
extern uint32_t fb_width;
extern uint32_t fb_height;
```

# Hipoteses atuais

## H1: presenter bare-metal copia frame valido para framebuffer errado/instavel

No Pi real observado anteriormente, o framebuffer inicial era `1920x1080`. O Rigel geralmente produz
frames Amiga bem menores (ex.: 320x256, 640x256, etc.). O caminho bare-metal cai no presenter/copia
quando `g_rigel_zero_copy_video == false`, e chama `PAL_Video_Resize(frame.width, frame.height, 16)`
quando a dimensao do frame diverge dos globals. Hoje `PAL_Video_Resize()` no raspi3 e stub que retorna
0 sem alterar os globals; portanto o presenter continua centralizando/escalando dentro do framebuffer
existente. Isso pode estar correto, mas precisa ser confirmado por log de frame real.

## H2: Denise/Rigel produz frame vazio ou sem bitplane DMA

Se `BPLCON0`, `DMACON`, flags de frame ou dirty lines indicarem ausencia de bitplanes/copper, o
problema nao e o VC4; e boot/chipset/guest nao programando a tela Amiga.

## H3: regressao de memoria/autoconfig afeta boot 68k antes de a tela Amiga estabilizar

Mudancas recentes em Z2 Fast RAM/autoconfig fizeram a janela de Fast RAM responder apenas depois de a
board ser configurada. Isso e correto para evitar duplicacao na memlist, mas pode mudar sintomas do
boot bare-metal. Se o trace de video mostrar registradores sem programacao ou repeticao de estados de
reset, investigar CPU/autoconfig antes de mexer em Denise.

# Instrumentacao adicionada

`src/machine/machine_rigel_step.c` ganhou log limitado, apenas fora do harness:

```text
[VIDEO-BM] frame=... src=WxH pitch=... fmt=... flags=...
           dirty=... full=... fb=WxH pitch=... zero_copy=...
           bplcon0=... dmacon=... diw=.../... ddf=.../...
           color00=... px=.../...
```

A instrumentacao loga as primeiras 16 apresentacoes e depois uma amostra a cada 64 frames. Ela foi
propositalmente colocada no presenter, nao no compositor, para responder a pergunta pratica: "o frame
que deveria ir para a tela Amiga existe e com que conteudo?"

# Como interpretar o proximo log

- `dirty=0`, `flags=0`, `bplcon0=0000` por varios frames: Rigel/guest ainda nao programou video;
  investigar boot 68k, reset/autoconfig/ROM.
- `flags` com `0x08` (Copper) e `dmacon` com DMA habilitado, mas `px` sempre igual e tela preta:
  investigar bitplane fetch/chip RAM ou compositor produzindo fundo somente.
- `src` plausivel e `px` variando, mas tela real preta/instavel: investigar presenter bare-metal,
  cacheabilidade/framebuffer VC4, `PAL_Video_Flip()` e OSD.
- `fb` mudando inesperadamente ou `pitch` inconsistente: investigar globals do framebuffer Emu68 e
  qualquer tentativa de resize/realloc.

# Proximo passo

1. Validar no Pi real (flash + boot KS13 sem disco): esperar hand screen estavel; conferir
   `[CHNG-R]` sem flapping e `[VIDEO-BOOT-DMA]` com passada unica do strap.
2. Commitar o fix no repo do rigel (rigel_cia_api.c, rigel.c, floppy_drive.c) — codigo
   compartilhado, vale para harness e bare-metal.
3. Sub-issue remanescente (nao coberta por este fix): `F-LINE-TRAP ir=ffff` no build Musashi
   **68040** bare-metal — investigar separadamente (provavelmente FPU/cache/modelo 040).
4. Depois da validacao no Pi, remover/reduzir a instrumentacao temporaria:
   `[VIDEO-BM]`/`[VIDEO-W-CPU]`/`[VIDEO-BOOT-DMA]`/`[VIDEO-COPPTR-CPU]` (machine_rigel_bus.c,
   machine_rigel_step.c, machine_rigel_trace.c), `[CHNG-R]`/`[PRB-W]` (machine_rigel_bus.c) e
   o evento COPPER_WRITE em external/rigel/copper_exec.c.

## Protocolo futuro de medicao do presenter (rascunho consolidado)

Nao atribuir baixo FPS a zero-copy, VSync ou buffer ownership sem primeiro
confirmar o caminho ativo. No bare metal atual, zero-copy so e selecionado para
framebuffers de ate `1024x312`; o framebuffer observado de `1920x1080` usa
`zero_copy=0`. Alem disso, `PAL_Video_Flip()` no Raspberry apenas atualiza o OSD
e nao espera VSync nem executa page flip. Portanto a hipotese de Core 2 esperando
o scanout nao descreve esse baseline, e o profile anterior apontou Agnus como o
dominio dominante do Rigel.

Se a apresentacao voltar a ser suspeita, comparar modos controlados e medir
separadamente:

1. avanco normal do Rigel sem apresentar o frame;
2. apresentacao/flip sem composicao de pixels;
3. render em buffer privado cacheavel seguido de copia linear;
4. zero-copy multi-buffer somente depois de existir scanout/page flip
   assincrono real;
5. `render_ns`, `copy_ns`, `cache_clean_ns`, `submit_ns` e qualquer
   `acquire_wait_ns` real.

Uma melhora sem apresentacao acusa o presenter; uma melhora sem drawing acusa
Denise/composicao; ambos isolados rapidos acusam sincronizacao. Dirty-line copy
e buffers adicionais so devem virar implementacao apos essa separacao medida.

## Regressão KS20 corrigida em 2026-07-06

O Rigel `d2abef6` fez o KS2.0 repetir indefinidamente o scan de drive-ID: o
valor `id_data=0xffffffff`, calibrado para a antiga saída invertida de `/CHNG`,
passou a representar resposta ativa de drive externo na saída não invertida de
`/DSKRDY`. DF0 interno não possui shifter de ID; a linha deve permanecer HIGH e
o Kickstart decodifica `0x00000000`.

Corrigido em Rigel `cee4e0d`, com drive desconectado retornando bit 0. Validado
em KS20 strap, KS13 strap e KS13+WB1.3. Lição: toda mudança de polaridade exige
revisar valores anteriormente calibrados para a polaridade oposta.
