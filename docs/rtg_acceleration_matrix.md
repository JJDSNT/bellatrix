# Bellatrix RTG — matriz de aceleração P96

Esta é a lista permanente das acelerações possíveis. Ela combina a interface
`BoardInfo` usada pelo AROS local, a documentação de desenvolvimento do P96 e
recursos citados no histórico público do P96. Uma operação não precisa ter
aparecido na telemetria atual do AROS para permanecer nesta matriz.

Estados: `não` = fallback CPU; `probe` = observado sem acelerar; `parcial` =
somente casos listados; `sim` = contrato coberto; `futuro` = fora do primeiro
blitter 2D.

## Primitivas 2D da BoardInfo

| Operação | Estado | AROS 1600f | Escopo mínimo Bellatrix | Casos posteriores |
|---|---:|---:|---|---|
| `WaitBlitter` | parcial | barreira | no-op enquanto comandos forem síncronos | status/fence para async |
| `FillRect` | parcial | ≥16; primeiro 640×480 | **implementado:** CLUT, RGB565 e ARGB32, máscara `ff`, bounds | máscaras parciais |
| `InvertRect` | parcial | 0 | **implementado:** três formatos, máscara completa | máscaras CLUT parciais |
| `BlitRect` | parcial | 0 | **implementado:** mesma VRAM, COPY, overlap-safe, três formatos, máscara `ff` | máscaras parciais |
| `BlitRectNoMaskComplete` | parcial | ≥2, 16×16, minterm `0c` | **implementado:** COPY overlap-safe entre `RenderInfo` em VRAM, nos três formatos | demais minterms |
| `BlitTemplate` | parcial | ≥1, 128×8 | **implementado:** upload 1-bit, JAM1/JAM2, INVERSVID, três formatos, máscara `ff` | COMPLEMENT, máscaras parciais |
| `BlitPattern` | parcial | 0 | **implementado:** 16×(1<<Size), offsets, JAM1/JAM2, INVERSVID, três formatos | COMPLEMENT, máscaras parciais |
| `DrawLine` | probe | 0 | sólida, COPY | padrões e draw modes |
| `BlitPlanar2Chunky` | não | não instrumentado | planar→CLUT | máscaras/interleaved |
| `BlitPlanar2Direct` | não | não instrumentado | planar→RGB565/ARGB32 | formatos adicionais |

Cada callback possui um `*Default` CPU no P96. Um caso não implementado deve
chamar esse default; nunca deve alegar sucesso parcial. `RGBFormat` vem do
argumento do callback, não deve ser inferido de `RenderInfo`.

## Apresentação e composição

| Recurso | Estado | Papel |
|---|---:|---|
| `SetPanning` / page flip | parcial | troca endereço/offset visível; aplicar atomicamente no VBlank quando houver troca real de bitmap |
| double buffering P96 | guest | depende de bitmaps alternados; não corrige desenho direto no front buffer |
| triple buffering | futuro | política de aplicação/fila de flips, não substitui primitivas rápidas |
| screen dragging/split | futuro | requer panning/split, memória off-screen e possivelmente VBlank |
| VBlank interrupt | não | necessário para waits/flips precisos; não para blitter síncrono inicial |
| hardware sprite | não | evita save/restore do softsprite; inclui imagem, posição e cores |
| palette/panning no VBlank | não | reduz flicker durante troca de paleta/viewport |
| PIP/video overlay | futuro | scaling, formato e chroma key; fora do desktop básico |
| mode mixing/dual palette | futuro | múltiplos formatos/paletas na mesma saída |

## Execução e transporte

| Recurso | Estado | Decisão |
|---|---:|---|
| VRAM CPU direta | sim | `base+0x10000..+0x7fffff`, sem bridge byte a byte; 64 KB iniciais reservam ABI/ROM |
| command ABI síncrona | parcial | FillRect, BlitCopy, InvertRect e BlitTemplate usam validação e `STATUS`; `COMMAND` termina antes do retorno |
| command queue | não | somente após comandos síncronos e fences corretos |
| async blits | futuro | requer `WaitBlitter`, status, ordering e proteção contra leitura prematura |
| dirty rectangles | não | presenter/shadow remoto; VRAM direta exige marcação explícita |
| shadow framebuffer | não | opcional para transporte, não para esconder fallback CPU |

## Ordem de implementação

Este bloco é o tracker de execução. Ao concluir uma etapa, registrar aqui o
commit, os testes e a evidência guest; itens sem evidência permanecem parciais.

- [x] `FillRect` CLUT/full-mask, incluindo o clear 640×480 observado (`5d761d1`).
- [x] `FillRect` RGB565 e ARGB32 (`0cac68d`).
- [x] `BlitRectNoMaskComplete` e `BlitRect` COPY overlap-safe (`0cac68d`).
- [x] `InvertRect`, três formatos e máscara completa (`21a1bc1`; unitário,
  ainda sem chamada no workload AROS).
- [x] `BlitTemplate`: upload portátil da máscara 1-bit, JAM1/JAM2,
  INVERSVID, três formatos e fallback (unitário + boot AROS com ADF).
- [x] `BlitPattern`: padrão 16×(1<<Size), offsets, JAM1/JAM2, INVERSVID e
  três formatos (unitário; ainda sem chamada no workload AROS).
- [ ] **ATUAL:** `DrawLine`: linha sólida COPY; depois padrões e demais draw modes.
- [ ] `BlitPlanar2Chunky`: planar→CLUT.
- [ ] `BlitPlanar2Direct`: planar→RGB565/ARGB32.
- [ ] Sprite, VBlank/page flip e depois fila/async.
- [ ] Screen dragging, overlay e mode mixing após o desktop básico.

## Critério para mudar o estado

Uma célula só passa a `parcial` ou `sim` com validação de bounds/overflow,
oráculo unitário por formato, fallback comprovado, integração guest e benchmark.
A ausência de artefato visual não basta.

## Fontes

- Código AROS local: `external/aros/arch/m68k-amiga/hidd/p96gfx/`, especialmente
  `p96gfx_card.c`, `p96gfx_bitmapclass.c`, `p96gfx_hiddclass.c` e `p96gfx_rtg.c`.
- [P96 Driver Development](https://wiki.icomp.de/wiki/P96_Driver_Development):
  primitivas oficiais, callbacks `Default`, `RGBFormat`, máscaras e fallback.
- [Histórico atual do P96](https://wiki.icomp.de/wiki/P96): async blits,
  fill/invert/copy/minterms, template, pattern, lines, sprites, dragging e PIP.
- [Picasso96 2.0 no Aminet](https://aminet.net/package/driver/video/Picasso96):
  histórico público de blitter, sprite, lines, pattern, PIP e double buffering.
