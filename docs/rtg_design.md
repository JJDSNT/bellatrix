# Bellatrix RTG — design

> **⚠️ Superseded / lab notes (correction 2026-07-17).** This describes a
> Bellatrix-specific P96 RTG board that **never reached a working state** — it
> was a lab. The **target for RTG is Emu68's `VideoCore.card`**, not this board.
> Nothing here is a live plan; the code under `src/machine/expansions/rtg/` is
> dormant and slated for eventual removal (deletion deferred only because its
> blast radius is large — harness `HARNESS_RTG`, `screenshot.c`, the
> `rtg_rom_data` Docker ROM build, and the `cards/bellatrix.card` tree). This
> document is **kept for design reference** in case RTG is ever revived on top of
> `VideoCore.card`. For the current, English boards/expansions architecture see
> [`expansions_and_boards.md`](expansions_and_boards.md). The status line below
> is historical and no longer accurate.

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
