# Bellatrix RTG — estudo de performance de renderização P96/SDL

## Conclusão executiva

O gargalo atual não é uma única primitiva P96. O caminho do harness faz trabalho
redundante em três etapas para todo frame:

1. `bellatrix_rtg_scanout_render()` converte a VRAM inteira para RGBA8888;
2. `machine_present_frame_from_rigel()` converte RGBA8888 para RGB565, escala e
   centraliza por CPU;
3. `PAL_Video_Flip()` envia novamente toda a superfície RGB565 com
   `SDL_UpdateTexture()` e apresenta.

Em 640×480 CLUT isso movimenta aproximadamente 4 MB por apresentação e executa
duas passagens por 307.200 pixels. A 60 Hz são cerca de 240 MB/s, sem contar o
trabalho do guest, SDL/GPU e cache misses. Em 1920×1080 o mesmo desenho passa de
1,6 GB/s de tráfego lógico por segundo.

Esses valores são estimativas obtidas do número de pixels e bytes percorridos,
não medições de largura de banda. O perfil existente (`HARNESS_PERF_TRACE=1`)
agrega o custo de apresentação em `present_ticks`; P0 abaixo propõe separar esse
valor por estágio para validar a atribuição do gargalo.

A percepção de desenho linha a linha não significa que SDL esteja apresentando
scanlines individualmente. `SDL_RenderPresent()` troca um backbuffer composto.
O Bellatrix, porém, converte a VRAM frontal viva enquanto o guest a modifica;
frames sucessivos capturam estágios sucessivos do desenho. Mais FPS torna essa
progressão mais visível, mas não a cria.

## Caminho atual auditado

### Guest e VRAM

- VRAM direta evita MMIO byte a byte para acessos normais do 68k.
- As primitivas implementadas reduzem o custo guest de fill, copy, template,
  pattern, line e conversões planares.
- As primitivas conhecem seus retângulos, mas ainda não publicam regiões sujas.
- Escritas diretas do 68k na VRAM não passam pelas callbacks do acelerador.

### Scanout

`src/machine/expansions/rtg/rtg_scanout.c` sempre percorre a imagem completa:

| Formato guest | Leitura | Buffer intermediário |
|---|---:|---:|
| CLUT | 1 byte/pixel + palette | RGBA, 4 bytes/pixel |
| RGB565 | 2 bytes/pixel + expansão | RGBA, 4 bytes/pixel |
| A8R8G8B8 | 4 bytes/pixel + swizzle | RGBA, 4 bytes/pixel |

### Presenter e SDL

`src/machine/machine_rigel_step.c` lê novamente o RGBA, converte cada pixel para
RGB565 e escreve na superfície PAL. O cálculo de `src_x`/`src_y` contém divisões
por pixel/linha mesmo no caso comum sem escala útil. Depois,
`src/host/posix/pal_posix.c` chama `SDL_UpdateTexture()` para a textura inteira,
`SDL_RenderClear()`, `SDL_RenderCopy()` e `SDL_RenderPresent()`.

A textura já é `SDL_TEXTUREACCESS_STREAMING`, mas não é atualizada por
`SDL_LockTexture()`. A documentação SDL classifica `SDL_UpdateTexture()` como
relativamente lenta para atualizações frequentes e recomenda lock/unlock para
texturas streaming.

## O que implementações maduras fazem

### Amiberry (código sincronizado com WinUAE)

O Amiberry v8 combina quatro mecanismos relevantes:

1. **RTG zero-copy:** quando possível, a superfície host aponta para o mesmo
   backing da memória RTG e a cópia intermediária é omitida.
2. **Write-watch/invalidation:** páginas/retângulos alterados pelo P96 são
   convertidos em dirty rectangles do renderer.
3. **Uploads parciais:** SDL atualiza somente os retângulos acumulados. Regiões
   verticais contíguas são fundidas; acima de 32 retângulos, muda para full frame.
4. **Heurística de frame completo:** quando cerca de 80% das páginas estão
   alteradas, deixa de insistir em cópias parciais.

O código também evita cópia quando input/output compartilham backing, pula OSD
sem alterações, evita clear redundante quando o quad cobre a saída e deixa o
renderer/GPU cuidar de escala. A versão 8 documenta ainda SIMD para o hot loop
bitplane-to-chunky e renderer OpenGL sem shader como padrão de desempenho.

### FS-UAE

O FS-UAE expõe separadamente o formato de vídeo fornecido pelo CPU e o formato
interno da textura OpenGL, porque casar esses formatos reduz conversão e custo
de upload. A lição para o Bellatrix é não fixar RGB565 na fronteira SDL se o
scanout já produziu RGBA compatível com uma textura host.

### MiSTer

O driver MiSTer P96 programa um framebuffer que o pipeline FPGA consome. Não há
uma cadeia SDL comparável nem motivo para copiar VRAM→RGBA→RGB565 por frame. A
lição transferível é manter a ABI P96 separada do presenter e permitir que cada
alvo consuma o framebuffer no formato mais próximo de seu hardware.

## Plano recomendado

### P0 — medir antes e depois

Adicionar tempos e bytes separados para:

- scanout/conversão;
- composição/escala CPU;
- lock ou upload SDL;
- `SDL_RenderPresent`/espera de VSync;
- frames solicitados, convertidos, enviados e efetivamente apresentados;
- pixels/bytes dirty e quantidade de retângulos;
- contadores por primitiva P96 e fallback.

Critérios iniciais: p50/p95 por estágio, MB/s copiados e porcentagem da tela
alterada. `HARNESS_SDL_VSYNC=0` mede throughput; VSync ligado mede pacing.

### P1 — remover a segunda conversão

Criar uma textura SDL no formato do buffer de scanout, preferencialmente
RGBA/ABGR8888 compatível com a ordem de bytes produzida. Converter diretamente
da VRAM para a memória obtida por `SDL_LockTexture()` e deixar SDL/GPU escalar e
centralizar com `SDL_RenderCopy()`. Isso remove:

- o framebuffer PAL RGB565 intermediário;
- RGBA→RGB565;
- as divisões de scaling no loop CPU;
- uma leitura e uma escrita completas por frame.

Para RGB565 guest, avaliar uma textura de 16 bits com byte-swap vetorizado. Para
ARGB32, usar formato SDL cuja ordem de bytes evite swizzle. CLUT ainda requer LUT
CPU no SDL renderer; em OpenGL/Vulkan, uma textura R8 + palette shader elimina a
expansão CPU.

### P2 — dirty rectangles com fallback honesto

Toda primitiva host deve marcar exatamente sua região destino. `SetPanning`,
modo e paleta invalidam full frame. Para escritas diretas do 68k há três opções:

1. write-watch por página no backend Emu68 e equivalente no harness;
2. bitmap/tile dirty mantido pelo mecanismo de memória direta;
3. comparação por tiles durante scanout como fallback portátil.

A opção 3 é o primeiro protótipo mais seguro: hashes de tiles 32×16 ou 64×16,
conversão somente dos tiles alterados e coalescimento em retângulos. Depois,
write-watch elimina até a leitura de tiles limpos. Aplicar limiar semelhante ao
Amiberry: muitos retângulos ou grande área suja viram full frame.

### P3 — pacing e consistência visual

- Não apresentar quando não há mudança, salvo OSD/VBlank que exija refresh.
- Limitar a apresentação ao refresh host; emulação pode continuar entre flips.
- Publicar modo/palette/pan atomicamente no boundary do presenter.
- Para eliminar a progressão do front buffer, priorizar page flip P96 real:
  desenhar em bitmap traseiro e aplicar `SetPanning` no VBlank.
- Quando o software desenha diretamente no front buffer, double buffering host
  não inventa atomicidade. Um snapshot coerente evita tearing durante a cópia,
  mas ainda mostrará etapas que o guest deliberadamente completou entre frames.

### P4 — transporte das primitivas

O upload atual de template/pattern/planar envia bytes por registrador. Ele reduz
escritas de pixels guest, mas no Emu68 cada write de registro pode virar fault.
Substituir por uma área de staging compartilhada/bulk na abertura da board, com
um único doorbell de comando. Medir antes: para operações pequenas o fallback
CPU pode ser mais barato que centenas de faults.

### P5 — SIMD e renderer avançado

Somente após P1–P4:

- NEON para CLUT→RGBA, RGB565 byte-swap/expansão e planar→chunky;
- OpenGL ES/Vulkan com upload R8/RGB565/32-bit nativo;
- PBO/staging assíncrono e fences;
- fila de comandos e `WaitBlitter` real.

## Ordem prática

1. Instrumentação por estágio e baseline 640×480 CLUT com AROS ADF.
2. Presenter RTG SDL direto em textura 32-bit, sem framebuffer RGB565.
3. Remover scaling CPU e `SDL_RenderClear` quando o quad cobre a saída.
4. Dirty tiles/rectangles e skip de frames sem mudança.
5. Panning/page flip sincronizado.
6. Staging bulk das primitivas para o caminho Emu68.
7. NEON; depois OpenGL ES/Vulkan se os números ainda justificarem.

## Fontes

- Código Bellatrix: `rtg_scanout.c`, `machine_rigel_step.c`, `pal_posix.c` e
  `cards/bellatrix.card/src/card.c`.
- [Amiberry v8](https://github.com/BlitterStudio/amiberry/releases/tag/v8.0.0):
  release e código de `picasso96.cpp`, `amiberry_gfx.cpp` e
  `sdl_renderer.cpp` (zero-copy, write-watch, dirty rectangles e uploads
  parciais).
- SDL2: [`SDL_UpdateTexture`](https://wiki.libsdl.org/SDL2/SDL_UpdateTexture),
  [`SDL_LockTexture`](https://wiki.libsdl.org/SDL2/SDL_LockTexture) e
  [`SDL_RenderPresent`](https://wiki.libsdl.org/SDL2/SDL_RenderPresent).
- FS-UAE: opção [`texture_format`](https://fs-uae.net/docs/options/texture_format).
- [MiSTer Minimig RTG driver](https://github.com/MiSTer-devel/Minimig-AGA_MiSTer/tree/MiSTer/extra/rtg_driver):
  `MiSTer.card.asm`.
