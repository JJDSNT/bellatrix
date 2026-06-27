---
id: ISSUE-0008
title: "Hardware sprite rendering"
status: review
priority: medium
type: bug
owner: agent
created_at: 2026-06-23
updated_at: 2026-06-26
tags:
  - denise
  - sprites
  - rendering
  - rigel
  - workbench
related_files:
  - external/rigel/src/chipset/agnus/mmio/agnus_mmio.c
  - external/rigel/src/chipset/agnus/dma/sprite_dma.c
  - external/rigel/src/chipset/denise/render/compositor.c
  - external/rigel/src/core/rigel_denise_api.c
  - src/machine/machine_rigel.c
  - src/machine/machine_rigel_step.c
---

# Issue: Amiga hardware sprites (mouse pointer, player ship) never appear on screen

## Status: PARTIAL (2026-06-26)

Sprite 0 (mouse pointer) e Happy Hand funcionando em hardware — commits `4847e26`
(Rigel) e `b69ed3c` (Bellatrix) resolveram os 6 problemas identificados.

**Ainda sem validação sistemática:**
- Sprites 1-7 (outros canais além do pointer)
- Attached sprite pairs (16-color sprites)
- Sprite/playfield priority (BPLCON2) com múltiplos sprites ativos
- CLXDAT/CLXCON (collision detection)

## Update 2026-06-23: post-commit summary

Committed fixes:

- Rigel submodule commit `4847e26`:
  `denise: fix hardware sprite pointer rendering`
- Bellatrix repo commit `b69ed3c`:
  `machine: stabilize Amiga pointer rendering`

Final confirmed problem chain:

1. Sprite 0 pointer routing was incomplete. `SPR0PTH`-`SPR7PTL` writes were not
   owned by Agnus MMIO, so sprite DMA did not reliably receive the base pointer.
2. Sprite DMA treated idle lines before `VSTART` as real fetches. This advanced
   the current sprite pointer through chip RAM before the beam reached the
   sprite's visible line.
3. Sprite DMA did not preserve a separate base pointer for frame reload. Once
   the current pointer reached the terminator or walked through data, the next
   frame did not restart from `SPRxPT`.
4. The sprite-list terminator `0000/0000` was not treated as a terminator. The
   channel could continue scanning later chip RAM and encounter plausible
   sprite-looking data in the same frame.
5. Sprite pixels were clipped by the DIW/playfield horizontal window. This is
   wrong for Amiga hardware sprites: DIW constrains bitplanes, while sprites
   may be visible in border/overscan.
6. Host frame export initially exposed either too little or too much horizontal
   range: too little clipped the pointer; too much showed a large blank band at
   the left of Workbench. The committed viewport is monitor-like: keep a small
   border before the DIW instead of exporting the whole internal raster.
7. The two-Amiga-pointer symptom was not framebuffer residue and not the host
   cursor. Direct trace showed consecutive frames alternating old/new sprite
   positions. Root cause: host mouse deltas updated `JOY0DAT` immediately,
   potentially mid-frame, while AmigaOS/Workbench samples mouse movement on
   VBL and rewrites sprite data from that stable frame cadence. Latching host
   movement and applying it only once per frame made sprite `hstart` monotonic
   in the synthetic test instead of alternating.
8. A separate compositor bug made the first sprite line use a different
   horizontal scale when `BPLCON0.HIRES` changed around the line. Sprite overlay
   now uses a sprite-specific scale derived from active video geometry, not the
   instantaneous bitplane scale for that line.

Important validation:

- Focused tests passed after the final changes:
  `ctest --test-dir out/harness -R 'test_(denise|sprites|priority|ham|dualpf)' --output-on-failure`.
- Synthetic mouse movement before the final input latch produced alternating
  sprite positions, e.g. `hstart=124/new/124/new`.
- After `bellatrix_machine_mouse_motion()` was changed to accumulate deltas
  and `machine_mouse_frame_tick()` became the only place that applies them to
  `JOY0DAT`, the trace became monotonic:
  `hstart=124,125,127,128,130,131,133...`.
- After separating `sprite_hscale`, frame 826 showed the first and following
  sprite rows aligned at the same base X (`y=42 hstart=133 dst_px=266...`,
  `y=43 hstart=133 dst_px=266...`), instead of the first row being drawn at
  half-scale.

Remaining caveats:

- User-visible result is clearly improved but not perfect yet. There are still
  pointer control/viewport details to tune.
- Battle Squadron / broader hardware sprite cases still need a fresh regression
  pass with `RIGEL_SPRITE_DMA_TRACE=1`.
- The SDL harness now hides the host cursor by default and leaves relative
  mouse/grab/capture opt-in (`BELLATRIX_SDL_RELATIVE_MOUSE=1`). This is a UI
  debugging choice, not the root cause of the two Amiga pointers.

## Update 2026-06-23: WB1.3 pointer root cause

The Workbench 1.3 pointer problem was not only the DIW crop. The full chain
had three separate failures:

1. Sprite pixels were initially cropped by the DIW/playfield horizontal window.
2. Exporting the full internal `x=0..visible_x_stop` range made WB1.3 appear
   with a huge empty left band because its DIW is `vis=252..892/5..261`.
3. Most importantly, sprite DMA had only a current pointer. After sprite 0
   reached the terminator, the internal pointer kept walking through chip RAM
   and never returned to the `SPR0PTH/PTL` base on the next frame. The pointer
   therefore drew once early in boot and vanished by the stable Workbench frame.
4. After adding per-frame reload, sprite DMA still kept scanning chip RAM after
   the `0000/0000` sprite-list terminator inside the same frame. On live output
   this could look like two Amiga pointers because the channel could encounter
   another plausible control/data sequence later in the field.

Final fix:

- `sprite_dma_channel_t` now keeps `base_ptr` separately from current `ptr`.
- writes to `SPRxPTH/PTL` update `base_ptr` and reset `ptr` to that base;
- `sprite_dma_frame_start()` restores every sprite channel to `base_ptr` at
  VERTB and clears `armed/fetch_ctrl/vstart/vstop/w0`;
- `sprite_dma_slot()` treats a fetched control pair of `0000/0000` as a
  terminator: the channel becomes idle until the next VBL reload or explicit
  `SPRxPT` write, and the zero control pair is not delivered to Denise as an
  active full-frame sprite;
- frame export uses a monitor-like horizontal viewport: if the DIW start is
  large, export begins 32 pixels before `visible_x_start`, preserving a small
  border for sprites without exposing the whole blanking range.
- The SDL harness hides the host cursor by default but does not grab/capture
  the mouse by default.

## Description

Original report: in the Musashi harness, clicking on a Workbench 1.3 icon
landed at the wrong position relative to where the visible mouse cursor
appeared to be. Investigation showed the visible "cursor" the user was
tracking was the *host* SDL cursor, not the Amiga's own hardware-sprite
pointer (sprite 0) — and that the Amiga's sprite pointer was not rendering
at all. The same symptom reproduces with Battle Squadron: enemy ships
(bitplane/blitter objects) render correctly, but the player's own ship
(believed to be hardware sprite 0, the same mechanism as the WB1.3 pointer)
never appears.

## Two confirmed bugs, both fixed

### 1. `SPR0PTH`-`SPR7PTL` never routed to Agnus

`external/rigel/src/chipset/agnus/mmio/agnus_mmio.c`,
`rigel_agnus_mmio_has_reg()` listed `BPL1PTH`-`BPL6PTL` (0x0E0-0x0F6) but
*not* `SPR0PTH`-`SPR7PTL` (0x120-0x13E). Writes to those addresses fell through to
`RIGEL_DOMAIN_UNKNOWN` and were stored as a raw register value only —
`sprite_dma_set_ptr_hi/lo()` was never called, so every sprite's DMA fetch
pointer stayed at its reset value forever.

Fix: added the `AGNUS_SPR0PTH`-`AGNUS_SPR7PTL` range check.

### 2. Sprite DMA pointer advanced during the idle wait for VSTART

`external/rigel/src/chipset/agnus/dma/sprite_dma.c`, `sprite_dma_slot()`.
On real hardware, a sprite's DMA channel is idle on every line between the
initial control-word fetch and `VSTART`. The Rigel implementation fetched and
advanced the pointer on *every* line unconditionally.

Fix: added an `idle = ch->armed && vpos < ch->vstart` gate.

## Third confirmed bug: output viewport used the DIW left edge

Workbench 1.3 exposed a separate compositor/output bug. Sprite pixels had
`dst_px=25..35`, while the decoded display-window left edge was `x_start=129`;
every pixel reported `cropped=1 drawn=0`.

Fix direction implemented:

- `compositor.c`: initialize full internal scanline to `COLOR00`; remove sprite
  overlay's `x_start/x_stop` crop.
- `rigel_denise_api.c`: `rigel_get_frame()` now exposes monitor-like viewport:
  32 pixels before `visible_x_start` through DIW right edge.
- `framebuffer.c`: zero-copy host target copy uses the same viewport rule.
- `pal.h`, `pal_posix.c`, `machine_rigel.c`, `machine_rigel_step.c`: harness
  avoids Rigel zero-copy presentation and supports dynamic SDL video resize.

## Still open: broader sprite/playfield priority regression

Battle Squadron/player-ship still needs a fresh run with `RIGEL_SPRITE_DMA_TRACE=1`
to verify whether its remaining invisibility is priority, attach state, palette
mapping, or another viewport assumption.

## Diagnostic trace added (safe on bare metal)

Two trace points exist, gated by `RIGEL_ENABLE_STDLIB_ENV` and `RIGEL_SPRITE_DMA_TRACE=1`:

- `external/rigel/src/chipset/agnus/dma/sprite_dma.c`: `[SPRITE0-DMA]`
- `external/rigel/src/chipset/denise/render/compositor.c`: `[SPRITE0-PIXEL]`, `[SPRITE0-WRITE]`

## Novo sintoma confirmado: nave some quando o fundo scrolla (Battle Squadron)

Screenshot de referência: `battle.jpeg` (frame=2749).

A nave do jogador (sprite 0) estava visível mas desaparece no momento em que
o fundo começa a scrollar. O fundo é bitplane-based, atualizado via copper list
(bitplane pointers + BPLCON1 a cada frame). Root cause provável: a copper list
que faz o scroll também reescreve `SPRxPT` — e nesse momento o `base_ptr` do
sprite 0 é zerado ou sobrescrito, fazendo o canal ficar idle no próximo frame.

Diagnóstico necessário: rodar com `RIGEL_SPRITE_DMA_TRACE=1` e capturar o
momento em que o scroll começa. Filtro:

```bash
RIGEL_SPRITE_DMA_TRACE=1 ./build_harness_rigel/harness src/roms/KS13.rom \
  --adf src/disks/battle.adf --frames 3000 2>&1 | \
  grep -E "SPRITE0-(DMA|WRITE|PIXEL)|SPR0PT|COPJMP|frame=27[0-9][0-9]"
```

O que procurar no log:
- Última linha `SPRITE0-DMA` antes do desaparecimento: qual `ptr` e `vstart/vstop`
- Se `ptr` zera ou salta para endereço inesperado → copper list está sobrescrevendo `SPRxPT`
- Se `ptr` está correto mas `SPRITE0-WRITE drawn=0` → problema de prioridade ou viewport

## Next steps

1. Investigar Battle Squadron sprite-on-scroll (ver acima) com `RIGEL_SPRITE_DMA_TRACE=1`.
2. Se priority é a causa, checar `denise->sprites.attached_mask` /
   `denise_sprite_is_attached()`.
3. Double check `DMACON` `SPREN` é setado durante os frames com atividade.
4. Decidir se Rigel precisa de campos separados para display viewport vs DIW/playfield.
5. Ver ISSUE-0014 para regressão de mouse QEMU.
6. Ver ISSUE-0016 para o bug de shift horizontal do 1943 (mesmo território de viewport).
