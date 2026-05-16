# Issue: Denise — Rendering, Bitplanes, Sprites

## Contexto

Denise é responsável por renderizar bitplanes e sprites em um framebuffer.
A implementação evoluiu de singleton/batch para instância explícita com rendering
raster-time. A tela de boot do KS1.3 ainda não aparece.

## Decisões Arquiteturais

- **Instância explícita** — `Denise *d` (não singleton), inicializado por `BellatrixMachine`
- **Rendering raster-time** — `agnus_step()` chama `denise_render_line()` quando
  `bitplanes_line_ready()` é verdadeiro
- **Framebuffer via globals Emu68** — `extern uint16_t *framebuffer; extern uint32_t pitch;`
  (já alocados pelo mailbox VC4 antes de `bellatrix_init()`)
- **Paleta LE16 RGB565** — convertida em `denise_write(COLOR_xx)`

## Modelo de Rendering

### Ciclo por linha (agnus_step):
```c
bitplanes_step(&s->bitplanes, s);        // 1. latcha estado da linha atual
if (bitplanes_line_ready(&s->bitplanes)) {
    denise_render_line(s->denise, s, &s->bitplanes);  // 2. renderiza
    bitplanes_clear_line_ready(&s->bitplanes);
}
```

### `denise_render_line()`:
- Calcula `vstart/vstop` de `agnus->diwstrt/diwstop`
- Linhas fora da display window → descartadas
- Itera sobre DDF words: lê `bpl[n]`, decodifica pixels
- Aplica paleta RGB565
- Escreve no `framebuffer`

### Escala 2×
Display Amiga 320×256 → framebuffer RPi em resolução maior. Offset centralizado.

## Bug Sprint 17: VBL Batch vs Raster-Time

### Problema
O modelo anterior (Sprint 07-10) rodava o Copper em batch no VBL e depois renderizava
frame inteiro. Copper podia escrever `BPLCON0=0x2302` (enable) e `BPLCON0=0x0302`
(disable) no mesmo batch → `bitplanes_begin_line` capturava `nplanes=0` em todas as linhas.

**Log do sintoma**:
```
[BPL-LINE-BEGIN] ... nplanes=2    ← Agnus captura estado correto
[DENISE-ENTRY]  ... ready=0 nplanes=0   ← Denise vê nplanes=0
```

### Fix (Sprint 17)
- Removido loop VBL batch (128 steps de Copper por linha artificial)
- VBL agora só: loga, dispara VERTB, reload Copper, flip framebuffer
- `agnus_step()` avança `copper_step`, `bitplanes_step` e chama `denise_render_line`
  ao longo do frame

## Bug: `nplanes=0` com `line_ready=0` (Sprint 17)

`bitplanes_begin_line`: quando `nplanes=0`, deve setar `line_ready=1` imediatamente
(render de background) e retornar. Antes: deixava `line_ready=0` → Denise nunca
recebia o path de background.

Fix: early return com `line_ready=1` quando `nplanes=0`.

## Sprites — Infraestrutura (Sprint 21)

### DeniseSprites
- `Denise` embeds `DeniseSprites sprites`
- `denise_write_reg()` roteia `SPRxPTH/PTL/POS/CTL/DATA/DATB`
- `denise_sprites_dma_request_mask()` — expõe demanda por sprite ao arbiter
- `denise_sprite_dma_service()` — recebe words do chip RAM
- `denise_sprite_begin_line()` — prepara fetch contract por linha

### Agnus DMA para Sprites
Em `agnus_step()`, por nova linha raster:
```c
denise_sprite_begin_line(s->denise, vpos);
// arbiter pode conceder AGNUS_DMA_REQ_SPRITE0..7
// → lê chip RAM → feed DeniseSprites words
```

### Composição de Pixels — Pendente
`denise_sprite_begin_line` e DMA grants existem.
**Composição de pixels de sprite no `denise_render_line()` ainda não implementada.**
Sprites são necessários para o cursor "Happy Hand" (Phase 5).

## Framebuffer

### Acesso via globals Emu68
```c
extern uint16_t *framebuffer;
extern uint32_t  pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
```
Alocados pelo VC4 mailbox antes de `bellatrix_init()`. Sem alocação própria.

### "Pau de Cego" — Diagnóstico
Sprint 10 sugeriu pintar framebuffer com cor sólida em `bellatrix_init()` para
confirmar VC4 ativo. Não confirmado implementado em produção.

## Estado da Tela de Boot — Investigação KS1.3

Veja `issue_harness_ks13_boot_screen.md` para a investigação completa.

**Resumo do bloqueador** (Sprint 26):
- Display setup callback chain completa (14 steps via VBL)
- Bitplane pointers `0xa572/0xc4b2` corretamente configurados
- Copper list válida com geometry correto
- **Buffers de bitplane permanecem zero** porque KS1.3 está preso em loop
  esperando LOF=0 (`btst #$6, $dff002`) e Agnus sempre reporta LOF=1.

**Fix necessário**: `beam.lof = 0` em modo PAL não-interlace.

## Registros Denise

| Registro | Dono | Descrição |
|----------|------|-----------|
| BPLCON0 | Denise | nplanes, mode, HIRES |
| BPLCON1 | Denise | bitplane scroll |
| BPLCON2 | Denise | sprite/playfield priority |
| BPL1MOD | Agnus | modulo planos ímpares |
| BPL2MOD | Agnus | modulo planos pares |
| COLOR00-31 | Denise | paleta RGB565 |
| SPRxPTH/L | Denise (via DMA) | sprite pointers |
| SPRxPOS/CTL/DATA/DATB | Denise | sprite state |

## Estado Atual

### O que Funciona
- Rendering raster-time integrado com agnus_step
- Paleta LE16 RGB565
- Display window (DIW) respeitada
- Bitplane decodificação com nplanes correto
- DMA grants para sprites (state feeding funciona)
- Escala 2× e centramento no framebuffer

### Bug Pendente (Bloqueador)
- **LOF=1 permanente** em Agnus: impede KS1.3 de rodar o bitmap producer.
  `beam.lof` deve ser 0 em modo PAL normal não-interlace.

### Não Implementado
- Sprite pixel compositing em `denise_render_line`
- Attached sprite pairs
- Sprite/playfield collision registers (CLXDAT/CLXCON)
- WAITs de Copper mid-scanline (raster bars) — para Phase 5+

## Arquivos Relevantes
- `src/chipset/denise/denise.h/.c` — struct Denise, render_line, write_reg
- `src/chipset/denise/sprites.h/.c` — DeniseSprites, DMA handshake
- `src/chipset/agnus/agnus.c` — agnus_step: bitplanes_step + render_line call
- `src/chipset/agnus/bitplanes.h/.c` — BitplaneState, line latch, DMA service
