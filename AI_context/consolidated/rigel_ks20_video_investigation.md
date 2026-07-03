// AI_context/memory/rigel_ks20_video_investigation.md

# Rigel KS20 Video Investigation

## Scope

This note tracks the Rigel-only investigation for `src/roms/KS20.rom` boot-screen
rendering. This is not an AROS issue and should not use the legacy chipset path
as the implementation target.

## Reference

Expected Kickstart 2.0 boot screen shape:

- purple background
- Amiga checkmark/logo on the left
- Kickstart 2.0 ROM text below the logo
- floppy icon on the right
- no horizontal wrap and no black vertical bar

The user described the original failure in `KS20.jpeg` as a horizontal wrap:
the logical screen began around 30% of the width, rendered through 100%, then
continued with the missing 0%-30% segment on the left.

## Fixed Locally

Changes are in `external/rigel`:

- `src/chipset/denise/video/display_window.c`
  - Hires display windows now scale both `visible_x_start` and
    `visible_x_stop`.
  - Before this, width was doubled for hires but the start coordinate remained
    in lores units, so `rigel_get_frame()` exported the wrong horizontal slice.

- `src/chipset/denise/render/compositor.c`
  - Hires DDF origin now uses the same horizontal scale as the visible window.
  - Sprite composition now honors each sprite's `VSTART/VSTOP`; previously an
    armed sprite could render on unrelated lines and create a vertical black bar.
  - Sprite horizontal composition now scales lores sprite bits across 2 hires
    pixels when `BPLCON0.HIRES` is set. This makes the floppy/sprite-width path
    match the hires playfield coordinate space instead of rendering too narrow.

- `src/chipset/agnus/timing/slot_scheduler.c`
  - `BPL1MOD/BPL2MOD` are now applied only after a scanline that actually fetched
    bitplane DMA.
  - `KS20.rom` uses negative modulos (`0xfffa`). Applying those modulos on lines
    before the visible bitplane fetches shifts pointers and recreates the wrap.

## Verification

Commands used:

```sh
rtk cmake --build out/harness-rigel --target harness -j2
rtk env BELLATRIX_CHIPSET_BACKEND=rigel BELLATRIX_RIGEL_TRACE=1 \
  BELLATRIX_RIGEL_DUMP_FRAME=465 \
  BELLATRIX_RIGEL_DUMP_PPM=/tmp/ks20_465_spritefix.ppm \
  ./out/harness-rigel/harness src/roms/KS20.rom --frames 470
rtk convert /tmp/ks20_465_spritefix.ppm /tmp/ks20_465_spritefix.png
```

Observed after the fixes:

- The horizontal wrap is gone.
- The black vertical bar is gone.
- The floppy/sprite overlay is wider and closer to the Kickstart reference in
  hires mode.
- Frame dump is still `560x145`, matching Rigel's current ECS/hires DIW decode
  for this ROM sequence.

## Session 2026-06-29 — Rendering Pipeline Correctness Fixes

Targeting KS20 improvement while preserving 1943 and EON correctness.

Changes in `external/rigel` (rigel commits `357c4ec`, `67e82ab`):

**BPLCON latch per scanline** (`denise_state.h`, `slot_scheduler.c/h`, `framebuffer.c`, `compositor.c`):
- New fields `line_bplcon0/1/2` + `line_bplcon_valid` in `rigel_denise_output_state_t`.
- Captured at the first bitplane DMA slot on each scanline; reset at line start.
- Compositor uses latched values, not live registers. Fixes frames where the
  copper rewrites BPLCON after DMA has already started for that line.

**Per-line bitplane depth** (`slot_scheduler.c/h`):
- `bitplane_line_depth` captures the actual DMA depth at dispatch time.
- End-of-line pointer advance and `BPL1MOD/BPL2MOD` application use this
  captured depth, not the current `sched->depth`. Prevents mis-advance when
  the copper changes `BPLCON0` mid-frame after DMA already ran.

**DDF window clipping** (`compositor.c`):
- Sprite and HAM render loops now clamp `screen_x` to `[x_start, x_stop]`.
- Pixels outside the active DDF fetch window are not written to the scanline
  buffer. Previously only bounds-checked against the full scanline array.

**Border fill on non-DMA scanlines** (`compositor.c`):
- `clear_scanline_to_border()` fills the scanline with `COLOR00` on lines
  that had no bitplane DMA. Prevents stale pixel carry-over from prior frames.

**VBL sprite reset** (`slot_scheduler.c`):
- `denise_sprites_reset()` called at VBL start each field.
- Ensures armed/vstart/vstop state from the previous frame cannot bleed into
  the new frame before the ROM's sprite DMA initializes the channels.

**Display window offset** (`framebuffer.c`, `rigel_denise_api.c`):
- Visible window left offset corrected to `-128` lores pixels (was `-32`).
- `rigel_get_frame()` applies a minimum height guard: if the decoded DIW
  height is < 64 lines and y0 < 64, y1 is set to at least 256. For normal
  screens, y0 is clamped to 44 and y1 to at least 244.

**CIA /DSKCHG fix** (`rigel_cia_api.c`, `floppy_drive.c`):
- `/DSKCHG` is open-drain: OR'd from all drives independently of selection.
- Not-connected drives no longer assert disk-changed at power-on.
- Drive ID scan correctly overrides /DSKCHG only for the selected drive.

Verified: KS20 improved; 1943 and EON not regressed.

## Remaining Issue

The KS20 screen is still not visually complete:

- logo and floppy are mostly outline/partial bitplane data
- ROM copyright text is missing or not rendered visibly
- fills and some colors do not match the reference
- the visible image appears static across the tested boot-screen frames:
  `/tmp/ks20_850.ppm`, `/tmp/ks20_890.ppm`, and
  `/tmp/ks20_900_sprite_hscale.ppm` compared with ImageMagick `AE=0`

Most likely next areas:

- animation source:
  - Copper trace shows `SPR0PTH..SPR7PTL` are reloaded every frame to
    `0x000490`, which looks like a shared/null sprite list rather than the
    animated floppy artwork.
  - This makes the remaining floppy animation more likely to be bitplane/Copper
    or blitter-driven than hardware-sprite driven.
- bitplane fetch/plane-word count for KS20 hires settings:
  - `BPLCON0=b302`
  - `BPLCON1=0044`
  - `DIW=6395/f4ad`
  - `DDF=0040/00d0`
  - `BPL1MOD=BPL2MOD=fffa`
- BPLCON1 scroll semantics in hires, especially separate PF1/PF2 nibbles.
- Hires planar expansion: compositor currently emits one pixel per bitplane bit
  block position. Verify whether hires needs different source-to-output mapping
  rather than only scaled DIW/DDF coordinates.

## Session 2026-07-02 — KS20 Text Opcode Trace

Added a generic harness diagnostic in `tools/harness/musashi_backend.c`:

- `HARNESS_ROM_WATCH_RANGE1=lo:hi`
- `HARNESS_ROM_WATCH_RANGE2=lo:hi`

It logs `[WATCH-ROM-R]` for reads from watched ROM ranges, including PC and
D/A registers. This is necessary because the text block at `0xFCECC4` is data,
not code; `HARNESS_TRACE_PC_RANGE=0xFCECC4:...` will not fire.

Findings from:

```sh
rtk env BELLATRIX_CHIPSET_BACKEND=rigel \
  HARNESS_ROM_WATCH_RANGE1=0xFCECC4:0xFCED24 \
  ./out/harness-rigel/harness src/roms/KS20.rom --frames 900
```

- `0xFCECC4` is the KS20 copyright-text script/data:
  - `2.0 Roms`
  - `Copyright ...`
  - `Commodore-Amiga, Inc.`
  - `All Rights Reserved`
- The script is first copied/read as longwords by reset code at `PC=0xF800E4`.
- Later it is interpreted by code around `PC=0xFCE716..0xFCE75E`.
- Script opcode `0xFB` is definitely reached:
  - `addr=FCECD2 val=FB` at `PC=FCE71A`
  - operand `0x16` at `PC=FCE71E`
- Text characters then go through the glyph/text routine around `PC=0xFA470C`
  and onward.

Additional run:

```sh
rtk env BELLATRIX_CHIPSET_BACKEND=rigel \
  HARNESS_ROM_WATCH_RANGE1=0xFCECD2:0xFCECD3 \
  HARNESS_WATCH_RANGE1=0x00D700:0x00D900 \
  ./out/harness-rigel/harness src/roms/KS20.rom --frames 900
```

showed many non-zero glyph writes to chip RAM around `0x00D73E..0x00D85x`,
mostly from `PC=0xFA4832` and `PC=0xFA4844`. Therefore the missing visible text
is no longer explained by the script interpreter aborting before glyph draw.
The glyph data is being generated in chip RAM.

Updated working hypothesis:

- The failure is downstream of text generation:
  - the written glyph buffer is not part of the bitplane DMA region being
    fetched for the visible KS20 screen, or
  - Rigel fetch/compositor/window/scroll handling maps that buffer outside the
    exported visible frame, or
  - the data is overwritten/cleared before the frame dump.
- Natural next trace: refine `RIGEL_BPL_FETCH_PROBE` so it can filter by frame
  and address range, then check whether Agnus ever fetches the glyph-written
  range (`0x00D73x..`) when the KS20 text should be visible.

## Session 2026-07-02 — KS20/WB1.3 Text Fixed; Horizontal Clip Still Open

The missing KS2.0 text was fixed. Root cause: ECS `BLTCON0L` (`$DFF05A`) was
implemented in the blitter register file but not claimed by
`rigel_blitter_domain_owns_reg()`, so KS2.0 writes to the low byte of
`BLTCON0` were dropped before they reached the handler. Adding `0x05A` to the
blitter domain restores the text path used by `Text()`/font blits; frame 755
of `KS20.rom --adf wb13.adf` shows both the copyright line and icon label
`DPaintIV`.

Related blitter fixes from the same session:

- copy-mode A/B shifter carry (`previous_a`/`previous_b`) is preserved across
  lines within one blit, matching WinUAE's `bltaold`/`bltbold` behavior;
- `BLTBDAT` maintains a separate B hold value before overwriting the register,
  so D-only/minterm cases that rely on held B data work.

The remaining horizontal wrap/truncation is separate from the text failure and
is still open. Observed cases:

- `KS20.rom` boot screen: logo/floppy appear truncated left and right; a
  too-broad left-clip relaxation makes prefetch show up as wrap.
- `KS13.rom + wb13.adf`: Workbench title text is clipped on the left.
- `KS20.rom + wb20.adf`: Workbench is slightly clipped on the right.
- `KS20.rom + wb13.adf`: the earlier right-edge duplicate improved after
  clamping normal single-playfield composition to the display word count, but
  the broader horizontal alignment problem is not closed.

Do not treat this as an export-border problem only. Increasing exported frame
borders changed frame width but did not solve the underlying clipped content.
The likely area is horizontal DIW/DDF/BPLCON1 alignment in HIRES, while keeping
prefetch words from rendering outside the real display window.

Validation:

```sh
rtk ctest --test-dir out/harness-rigel/rigel-build \
  -R 'test_(blitter|blitter_dma|denise|mmio)$' --output-on-failure
```

All focused tests passed. A new Denise regression marks words 40/41 in the
WB1.3 HIRES mode as sentinels and asserts they are not visible in the exported
frame.

## Session 2026-07-02 — RESOLVIDO: truncamento horizontal (DDF→DIW alignment)

O truncamento/wrap horizontal (KS20 boot cortado à esquerda e à direita;
KS13+WB1.3 e KS20+WB2.0 cortados à direita) foi resolvido no compositor do
rigel. Três bugs combinados em `compositor.c`:

1. **Pipeline lead errado.** O primeiro pixel de bitplane aparece 8.5 CCK
   (lores) / 4.5 CCK (hires) depois do DDFSTRT — é exatamente a relação do
   HRM `DDFSTRT = DIWSTRT_H/2 − 8.5 (lores) / − 4.5 (hires)`. O código usava
   8/4 CCK. Novo: `ddf0 = (ddfstrt_lores + (hires ? 9 : 17)) * hscale`.

2. **Sinal do scroll (BPLCON1) invertido.** Delay desloca o playfield para a
   DIREITA, em passos de 1 lores px (×2 em hires). O código subtraía.
   `screen_x = ddf0 + w*16 + px + scroll*hscale`.

3. **Clamp `w >= display_words` removido.** Cortava words por índice de fetch,
   matando a última word visível quando o fetch começa antes da janela
   (caso KS20 com BPLxPT 2 bytes antes da arte + modulo −6).

Verificação da aritmética no KS20 (`DDF=0040/00d0`, `DIW=6395/f4ad`,
`BPLCON1=0044`, mods `fffa`): fetch de 38 words (608px), stride real
38·2−6 = 70 bytes = 35 words = 560px = largura exata da janela (298..858).
Word 0 fica escondida à esquerda do DIW (a "tira" que aparecia na borda era
ela — o fim da linha anterior), words 36–37 escondidas à direita. Com o
alinhamento correto tudo fecha flush, igual ao hardware.

Validado visualmente no harness headless: KS20 boot (texto+logo+floppy
completos), KS13+wb13 (janela AmigaDOS completa), KS20+wb20 (desktop com
gadgets da direita completos), KS13 insert-hand, 1943 cracktro, AROS boot.
`test_denise` recalibrado para a semântica de hardware (lead 17/9, scroll à
direita) e agora imprime o nome do sub-teste que falhar. Suite completa passa,
exceto `test_copper` que já estava quebrado antes (API
`rigel_copper_domain_step` mudou sem atualizar o teste standalone).

Diagnósticos novos env-gated em `compositor.c`:
`RIGEL_COMPOSE_TRACE_FRAME=N` (por linha: ddf0/janela/scroll/word count/span
não-zero) e `RIGEL_COMPOSE_TRACE_Y=N` (dump das plane words da linha).

## Session 2026-07-02 — Boot Timing, DIWHIGH, and Remaining Text Failure

User constraint: the KS20 insert-disk screen, including text and floppy
animation, should settle before 400 frames. The earlier trace that reached the
text script only around 900 frames was therefore a symptom, not acceptable
behavior.

### Floppy/default drive model

Important finding: Rigel was modelling all four floppy drives as connected empty
drives at reset. KS20 spends time probing non-DF0 drives, which delayed reaching
the insert-disk/text path.

Local change in `external/rigel`:

- `FloppyDrive` now has `connected`.
- Reset defaults only DF0 connected; DF1-DF3 are disconnected.
- `floppy_insert()` connects the target drive.
- Disconnected drives release lines instead of behaving as empty selected
  drives:
  - `/DSKCHG` high
  - `/WPRO` high
  - `/TRK0` inactive
  - `/RDY` inactive
  - ID bit high
- CIA-B floppy routing ignores select/motor for disconnected drives.
- Public `rigel_floppy_get_status()` now reports disconnected drives as not
  selected.

Verification after this change:

```sh
rtk cmake --build out/harness-rigel --target harness -j2
rtk proxy bash -lc 'BELLATRIX_CHIPSET_BACKEND=rigel \
  HARNESS_ROM_WATCH_RANGE1=0xFCECD2:0xFCECD3 \
  HARNESS_WATCH_RANGE1=0x00D700:0x00D900 \
  ./out/harness-rigel/harness src/roms/KS20.rom --frames 400 2>&1 |
  rg "WATCH-ROM-R|pc=00fa48|WATCH-BPL-RAM-W"'
```

Result:

- `0xFB` script opcode is reached before 400 frames.
- Glyph writes from `PC=0xFA4832/0xFA4844` occur before 400 frames.
- This fixed the "text opcode never runs soon enough" part of the bug.

### ECS DIWHIGH / vertical clipping

KS20 programs:

- `BPLCON0=b302`
- `DIWSTRT=6395`
- `DIWSTOP=f4ad`
- `DIWHIGH=2000`
- `DDF=0040/00d0`
- `BPL1MOD=BPL2MOD=fffa`

Rigel decoded this as a short visible vertical window ending around line 244,
which clipped the lower part of the insert-disk artwork and hid the text region.

Local change in `external/rigel/src/chipset/denise/video/display_window.c`:

- Treat `DIWHIGH=0x2000` with an 8-bit `DIWSTOP` vertical decode as an extended
  vertical stop for this ECS window.
- Clamp `vstop` to `RIGEL_DENISE_MAX_LINES` instead of discarding the geometry
  when the decoded window reaches the PAL raster end.

Observed effect:

- Frame dump size changed from `688x200` to `688x268`.
- Trace now reports `vis=298..858/99..312`.
- The lower screen/artwork region is no longer clipped.

### DIWSTRT=ffff transient horizontal beating

After exposing the lower window, the screen began "batendo horizontalmente".
Trace showed KS20 periodically writes a transient blanking/window value:

- `DIWSTRT=ffff`
- `DIWSTOP=f4ad`

Rigel was accepting that as a real viewport and alternating exported width
between `688` and `476`.

Local change:

- Ignore `DIWSTRT=0xffff` in `display_window_update()` when a valid geometry
  already exists.

Observed effect:

- `RIGEL-FRAME-VIDEO` now remains stable at `688x268`.
- At frame 250, raw registers can still show `diw=ffff/f4ad`, but exported
  visible geometry remains the last valid `6395/f4ad` window.

### Current text status

Text is still not visible. The current visual result is stable horizontally and
shows the extended lower region, but the expected text area renders as blue
striped/incorrect bitplane data or remains blank depending on animation page.

Important traces:

1. Glyph writes are real:

```sh
BELLATRIX_CHIPSET_BACKEND=rigel \
HARNESS_WATCH_RANGE1=0x00D700:0x00D900 \
./out/harness-rigel/harness src/roms/KS20.rom --frames 430
```

Shows many writes to `0x00D73E..0x00D85x` from `PC=0xFA4832/0xFA4844`.

2. Bitplane DMA fetch of the same range can occur while the range is still zero:

```sh
BELLATRIX_CHIPSET_BACKEND=rigel \
RIGEL_BPL_FETCH_TRACE_RANGE=0x00D700:0x00D900 \
RIGEL_BPL_FETCH_TRACE_MIN_FRAME=500 \
RIGEL_BPL_FETCH_TRACE_VFROM=238 \
RIGEL_BPL_FETCH_TRACE_VTO=260 \
RIGEL_BPL_FETCH_TRACE_LIMIT=500 \
./out/harness-rigel/harness src/roms/KS20.rom --frames 620
```

Shows fetches like:

- `frame=570 v=244 plane=2 addr=00d73a data=0000`
- `frame=570 v=244 plane=2 addr=00d73e data=0000`

3. Chip RAM dump at frame 600 confirms the `0x00D700` text buffer is zero by
then:

```sh
BELLATRIX_CHIPSET_BACKEND=rigel \
HARNESS_SCREENSHOT_FRAMES=600 \
HARNESS_SCREENSHOT_DIR=/tmp/ks20_chip \
HARNESS_CHIPDUMP=0xd700:0x200 \
./out/harness-rigel/harness src/roms/KS20.rom --frames 602

od -Ax -tx2 -N 128 /tmp/ks20_chip/chip_600_0d700.bin
```

Output starts with all zero words. Therefore the current failure is not simply
"Rigel DMA cannot see CPU writes"; either the glyph buffer is later cleared or
the ROM alternates/double-buffers pages and Rigel is showing/fetching the wrong
page at the time the text should appear.

### Diagnostic additions currently in tree

`tools/harness/musashi_backend.c`:

- `HARNESS_ROM_WATCH_RANGE1/2=lo:hi`
- Logs `[WATCH-ROM-R]` with PC and selected D/A registers.

`external/rigel/src/chipset/agnus/timing/slot_scheduler.c`:

- `RIGEL_BPL_FETCH_TRACE_RANGE=lo:hi`
- `RIGEL_BPL_FETCH_TRACE_FRAME=N`
- `RIGEL_BPL_FETCH_TRACE_MIN_FRAME=N`
- `RIGEL_BPL_FETCH_TRACE_VFROM=N`
- `RIGEL_BPL_FETCH_TRACE_VTO=N`
- `RIGEL_BPL_FETCH_TRACE_LIMIT=N`
- `RIGEL_BPL_TABLE_TRACE`
- `RIGEL_BPL_DISPATCH_TRACE`

These are env-gated diagnostics and were useful to prove the text path and
bitplane fetch timing.

### Open next steps

- Find who clears or overwrites `0x00D700..0x00D900` after the glyph routine.
  Use `HARNESS_WATCH_RANGE1=0x00D700:0x00D900` and look for later zero writes
  after the `PC=0xFA48xx` glyph writes.
- Track KS20 bitplane pointer page alternation:
  - known pages observed: `006048/0087ee` and `00d73a/00fee0`
  - determine which page should contain final text and whether Rigel advances
    `BPLxPT`/modulos incorrectly.
- Continue comparing with KS31 on 68020 as a validation ROM; user reports the
  same symptom there.
- Revisit ECS register gaps only if traces show KS20 writes them. So far there
  is no evidence of `BPLCON4` or `FMODE` writes in this path; `DIWHIGH` was the
  relevant ECS register found in this session.

## Session 2026-07-02 (noite) — BLTSIZV latch bug FIXED; stamp de texto ainda aberto

### Ferramentas novas (env-gated, in-tree)

- `RIGEL_BLT_W_TRACE_RANGE=lo:hi` (blitter_ref.c/blitter_line.c): loga
  `[BLT-W]` para toda escrita do blitter no range — cobre o que watchpoint
  de CPU não vê.
- `RIGEL_BLT_CMD_TRACE=1` (blitter_command.c): loga `[BLT-CMD]` com con0/
  con1/A/B/C/D/mods/size/adat/fwm/lwm de todo blit.
- Disassembler m68k standalone: `dasm.c` no scratchpad, compila com
  musashi/m68kdasm.c — `./dasm KS20.rom 0xFA4700 0xFA4900`.

### BUG CORRIGIDO: BLTSIZV/BLTSIZH zerados após blit (rigel blitter_timing.c)

`blitter_publish_result()` zerava `bltsizh/bltsizv` ao fim do blit. No
hardware ECS são latches persistentes; o KS2.0 programa BLTSIZV uma vez e
re-dispara blits da mesma altura escrevendo só BLTSIZH. Consequência: o 2º
e 3º blits de cada série saíam com altura 0 (nenhuma escrita). Fix: não
zerar. Resultado visível: logo + floppy + fundo roxo renderizam; os blits
de texto/animação agora executam nos 3 planos (5x9 nos três, era 5x9/5x0/
5x0). Animação do disquete deve melhorar (blits perdidos eram isso).

### BLTCON0L (0x05A) implementado no rigel (blitter_regs.c)

Escrita só do minterm, preservando USE/shift. KS20 não usa (verificado),
mas é correto ECS.

### ABERTO: mecânica do stamp de texto — minterm 0x0A não deposita B

Fluxo completo mapeado:
1. CPU renderiza a linha de texto num strip scratch (stride 14 bytes,
   ~9 linhas, em ~0xd722..0xd7b4) via loop `or.l D0,(A0)` em 0xFA47DA/
   0xFA482A (rotina de glifos, disasm confirmado).
2. Blits "stamp" por plano: `con0=<ashift>|070a con1=<bshift>|0000`,
   B=strip (bmod -24), C=D=linha ~102 do plano da página A (0x7c3a/
   0xa3e0/0xcb86, cmod/dmod -80, size 5x9, adat=ffff, fwm/lwm recortam
   as colunas). Programado em 0xFA382C/0xFA38A2 (`move.w #$70a,(A5)`).
3. Minterm 0x0A = ~A·C — pela spec APAGA a faixa e ignora B (embora USEB
   esteja ligado e o B pointer/mods/bshift estejam todos coerentes com
   uma cópia). Na prática o rigel zera a área e o texto nunca aparece.
   No hardware real o texto aparece — logo nossa leitura da semântica de
   canal/minterm para este caso está errada em algum ponto.

Hipóteses testadas e descartadas: BLTCON0L trocando minterm (KS20 não
escreve 0x05A); segundo blit "draw" na mesma área (não existe — só 1 stamp
por plano); texto via CPU direto nos planos visíveis (watch em 0x7bd0-
0x7d00: só zeros do blitter + clear de boot).

Próximos passos sugeridos: (a) capturar o strip ENTRE o render de glifos e
o erase (chipdump no frame exato do batch pc=FA48xx) e simular o stamp
offline com interpretações alternativas de canal (A↔B, masks, adat) até
reproduzir a referência; (b) comparar com WinUAE logando os mesmos blits
(con0=070a) para ver o D resultante real; (c) conferir se BLTBDAT/prime de
B (primeira fetch descartada?) muda o resultado — rigel pode estar errando
o pipeline de shift do canal B (primeiro word do B shiftado entra no
SEGUINTE), o que faria B "vazar" para... (verificar se minterm deveria ser
outro por conta de con1 bit0 line=0 etc).

### Nota sobre a janela vertical (duplicata na parte de baixo)

O shot 688x268 mostra a arte da página A e, abaixo, a página B/scratch
sendo desenhada (logo prata etc.). Rever a interpretação de DIWHIGH=0x2000
da sessão anterior: se a janela real for 145 linhas, o "extended vstop"
está expondo memória que não deveria ser visível; se for mais alta, a
página B não deveria estar mapeada ali. Decidir com a referência (texto
fica DENTRO da janela, linha ~102 da página A — não precisa da extensão
para aparecer).

### RESOLVIDO (2026-07-02, noite): duplicata na parte inferior — decode do DIWHIGH

`DIWHIGH=$2000` é o bit H8 do HSTOP (horizontal), não extensão vertical.
O decode genérico anterior mapeava bits 11-13 como H8-H10 (errado: 11-12
são sub-pixel SHRES; H8 = bit 13 no byte de stop / bit 5 no de start) e a
sessão anterior tinha adicionado um "extended vstop" que esticava a janela
para 268 linhas, expondo a página de trás do double-buffer (a "duplicata"
com logo prata + lixo). Corrigido em display_window.c: H8 = (byte>>5)&1,
vstop = decode normal (244). Janela KS20 agora 688x200 estável, sem
duplicata. Verificado sem regressão: KS13 (448x256, caminho OCS intocado)
e AROS (boot screen 768x256 ok).
