---
id: ISSUE-0024
title: "KS20: texto não renderiza — boot screen sem texto; após o boot, requesters e nomes de ícones sem texto"
status: open
priority: high
type: bug
owner: unassigned
created_at: 2026-07-02
updated_at: 2026-07-02
related_files:
  - tools/harness/screenshot.c
  - external/rigel/src/chipset/denise/render/compositor.c
  - external/rigel/src/chipset/denise/output/rgb.h
tags:
  - ks20
  - denise
  - rendering
  - text
  - blitter
related_files:
  - src/chipset/denise/denise.c
  - src/chipset/agnus/blitter.c
  - src/chipset/agnus/agnus.c
---

# Investigação 2026-07-02 (sessão screenshot + desmontagem)

**Ferramenta nova**: screenshots headless — `HARNESS_SCREENSHOT_FRAMES=N,M`
+ `HARNESS_SCREENSHOT_DIR=dir` escreve `shot_<N>.ppm` (tools/harness/
screenshot.c). Pixel do frame rigel é [R,G,B,A] por byte, alpha=0.
Também existe `BELLATRIX_RIGEL_DUMP_FRAME/PPM/PALETTE` (precisa
`BELLATRIX_RIGEL_TRACE=1`) que imprime bplcon/diw/ddf e os 32 COLORxx.

**Achado 1 — cores do boot screen: ordem de bitplanes invertida (hires).**
No frame 1200 do boot KS20 (sem disco): COLOR00=0414 (roxo), COLOR01=0EA8
(bege), COLOR04=0238 (azul) — corretos vs referência (ks20_reference.png).
Mas o render mostra bege↔azul trocados = **índice 1 ↔ índice 4** = ordem
dos bitplanes invertida (bit0↔bit2) no caminho de composição HIRES
(bplcon0=B302, 3 planos). Índices simétricos (0, 7) são invariantes — por
isso KS1.3/WB nunca denunciou. Suspeitos: rigel 67e82ab (bplcon latch por
scanline / hires window alignment).

**Achado 2 — o boot screen é um script de desenho na ROM.** Em ROM offset
0x4ecc4 (0xFCECC4): opcodes `FB len "texto"` (string), `FD x y` (posição),
`FE/FC` (cor/forma), seguidos dos polígonos do logo e disquete. O strap
interpreta e desenha via graphics.library; texto = Text() → blitter.
Logo+disquete aparecem, texto não → interpretador roda; falha é específica
do caminho de glifos (blit de fonte) ou de composição do plano do texto.

**Achado 3 — O TEXTO NUNCA É ESCRITO NOS BITPLANES (não é bug da Denise).**
Dump dos 3 planos exibidos (BPL1=0x6048, BPL2=0x87EE, BPL3=0xAF94, stride
70, 145 linhas — bate exato com DIW 0x6395/0xF4AD e o espaçamento 0x27A6
entre planos) renderizados separadamente: logo e disquete presentes,
NENHUM texto em nenhum plano. A falha está no caminho de desenho (blitter/
CPU) ou o interpretador do script pula o opcode de texto.

**Achado 4 — copper list decodificada (0x29FB0)**: paleta correta; rainbow
do logo = copper animando COLOR04 (reg 0x188) por scanline (0x0F02→0x0FF0);
BPLxPT = 6048/87EE/AF94; janela de 145 linhas visíveis.

**Achado 5 — blitter ativo (48k escritas de reg em 1300 frames)**: loop do
interpretador em pc=0xF9F87E faz blits cookie-cut (minterm CA, BLTADAT=
0x8000 = edge/line dos polígonos) e em pc=0xFA02B0 blits com A em 0x0002xxxx.
Os D-pointers atingem os planos exibidos (0x6xxx) E uma segunda região em
0x2xxxx (ex.: D=0x2AF8A) que NÃO é exibida — identificar o que é (segundo
buffer? estruturas?). Região 0x20000-0x30000 dumpada não parece bitmap
coerente em stride 70.

**Achado 1 revisado**: o gradiente das faixas do logo APARECE no render
(azul→verde em vez de vermelho→amarelo) — então índice 4 é exibido e a
hipótese "ordem de planos invertida" não fecha (teria cor constante).
Análise por cor: 0EA8→azulado e rainbow F,x,0→0,x,F parecem swap R↔B, mas
COLOR02 0xA76 renderiza SEM swap (marrom-cinza correto). Pendente análise
pixel-a-pixel (comparar composição esperada da copper list vs shot PPM).

**Ferramentas novas**: `HARNESS_CHIPDUMP=hexaddr:hexlen` (junto com
HARNESS_SCREENSHOT_FRAMES) escreve chip_<frame>_<addr>.bin; scripts
ppm2png.py e plane2png.py (render de bitplane 1-bit) no scratchpad da
sessão — considerar mover para tools/.

**Próximos passos**: (a) identificar quem desenha o texto: breakpoint/trace
no interpretador do script da ROM (opcode 0xFB em 0x4ECC4/0xFCECC4) — o
opcode é executado? chama Text() do graphics? (b) o que é o bitmap em
0x2xxxx que recebe blits mas não é exibido; (c) fechar a análise de cores
(Achado 1 revisado) com comparação pixel-a-pixel.

# Sintoma

Com KS2.0 (KS20.rom), o texto não aparece em vários contextos:

1. **Boot screen**: a tela de boot aparece, mas sem o texto.
2. **Pós-boot (Workbench)**: requesters abrem sem o texto; os nomes dos
   ícones no desktop/janelas também não são desenhados.

Gráficos em geral (janelas, bordas, ícones) renderizam — o que falta é
especificamente o texto, o que sugere um caminho comum de desenho de glifos
(blitter em modo texto/font rendering, ou algum modo de bitplane/máscara que
o KS2.0 usa e o KS1.3 não).

# Contexto

- KS1.3/Workbench 1.3 e AROS renderizam texto normalmente — o problema é
  específico do KS2.0.
- Houve rodada de "KS20 rendering fixes" em 2026-06-29 (ver AI_context/log
  dessa data e commit `285dcd2`-adjacente) — verificar se este sintoma é
  regressão dessas mudanças ou gap pré-existente.
- Relacionado mas distinto de [[ISSUE-0023]] (stall de boot do KS20 com ISO):
  este issue é sobre renderização e reproduz sem CD.

# Hipóteses iniciais

- Blitter: minterm/canal (B = fonte de glifos, máscaras FWM/LWM, shift) usado
  pelo `Text()` do graphics.library V37 pode exercitar um caso não coberto.
- Denise: prioridade/planos ou modo (e.g. escrita em plano único via
  blit cookie-cut) que o KS2.0 usa para texto.
- Verificar com btrace (`0x0004` chipset) uma chamada de Text() no boot
  screen e comparar os registradores do blitter contra WinUAE.

# Reprodução

Boot KS20.rom (harness ou hardware) — o boot screen já demonstra o problema,
sem precisar de disco.
