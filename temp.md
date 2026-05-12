Imagens: harness_aros_boot_screen.jpeg versus aros_boot-screen_reference.jpeg

Roadmap de investigação — Bellatrix / AROS boot screen pós-fix ddf_words=40

Contexto:
- O bug principal de skew diagonal foi fortemente reduzido ao ajustar o fetch hires de 39 para 40 words.
- Antes: ddf_words=39 causava déficit de 2 bytes por linha.
- Depois: AROS boot screen ficou estruturalmente correto: olhos, ícones e texto aparecem alinhados.
- Ainda há diferenças em relação ao WinUAE:
  - logo AROS está deslocado/truncado/comprimido;
  - texto “WAITING FOR BOOTABLE MEDIA” ainda parece deformado;
  - ícones/texto parecem mais grossos ou com escala horizontal incorreta;
  - posicionamento horizontal geral ainda não bate exatamente.

Objetivo:
Investigar o erro residual sem provocar regressões no avanço já obtido.

Regras:
- Não mexer em beam ainda.
- Não mexer em Copper agora.
- Não mexer em Paula/floppy/IRQ.
- Não reescrever Denise inteira.
- Fazer uma hipótese por patch.
- Rodar AROS boot screen e KS1.3/Workbench após cada mudança.

1. Confirmar o novo baseline

Rodar:

KICKSTART=src/roms/aros.rom ./run.sh harness

Verificar visualmente:
- AROS aparece sem skew diagonal grave.
- ddf_words deve ser 40.
- ponteiro por linha deve avançar:
  40 words = 80 bytes
  + BPLMOD 0x50 = 80 bytes
  total = 160 bytes = 0xa0 por linha.

Adicionar/confirmar log:

[BPL-DIAG-DONE] v=... words=40 ... post1=...

Critério:
- bpl1 deve avançar 0xa0 por scanline visível.
- Não deve voltar para 39 words.

2. Validar se Denise está recebendo linhas corretas

Antes de mexer em renderização, confirmar que o buffer de entrada de Denise está coerente.

Verificar:
- bp->nplanes = 4
- hires = 1
- ddf_words = 40
- line_words[plane][0..39] preenchidos
- line_vpos correto
- Denise é chamada apenas quando ready=1.

Procurar chamadas:

grep -RniE "denise.*render|DENISE-ENTRY|bitplanes_line_ready|bitplanes_clear_line_ready|bitplanes_plane_words" src/chipset

Critério:
- Denise não deve renderizar linha com ready=0.
- ready não deve ser limpo antes do consumo.
- o mesmo vpos não deve ser renderizado múltiplas vezes com buffers diferentes.

3. Investigar unpack de pixels hires em Denise

Suspeita atual:
O erro residual parece mais de unpack/escala horizontal do que de fetch.

No AROS:
- BPLCON0=c204
- hires=1
- nplanes=4
- ddf_words=40
- pix esperado por linha lógica: 40 words * 16 pixels = 640 pixels em hires, antes de recortes/janela.

Verificar em src/chipset/denise/denise.c:
- cada word de bitplane gera 16 pixels, não 32;
- em hires não deve duplicar pixel horizontal;
- hscale não deve aplicar duplicação indevida;
- x_phase/scroll não deve deslocar indevidamente a linha;
- BPLCON1 scroll deve ser aplicado corretamente ou explicitamente zerado se ainda não suportado.

Adicionar log resumido em linhas selecionadas:

[DENISE-HIRES] line=%d bp_v=%d words=%d pix=%d visible=%d hscale=%d scroll=%d x_phase=%d first_idx=%02x last_idx=%02x non_bg=%d xspan=%d-%d

Critério:
- Para 40 words, pix bruto deve ser 640.
- Se aparecer 624, 608 ou outro valor, achar onde está sendo cortado.
- Comparar com a imagem: se o logo está truncado à esquerda/direita, suspeitar de crop/DIW horizontal.

4. Revisar cálculo de largura renderizada

No log antigo, Denise mostrava:

pix=624
visible=624
out=624x512
fb0=8,0

Após ddf_words=40, verificar se mudou para:
pix=640 ou continua 624.

Se continuar 624, procurar hardcoded crop:

grep -RniE "624|640|visible|pix|hscale|xspan|fb0|DIW|diw|ddf_words" src/chipset/denise src/chipset/agnus

Hipótese:
- fetch agora está correto com 40 words;
- mas Denise ainda renderiza/copia apenas 39 words ou 624 pixels;
- isso explicaria truncamento residual.

Critério:
- Garantir que a quantidade de words usada por Denise venha de bitplanes_ddf_words(), não de cálculo antigo ou constante.

5. Investigar DIW horizontal

Somente depois de validar Denise/unpack.

Valores observados:
- DIWSTRT=2c81
- DIWSTOP=2cc1
- DDFSTRT=003c
- DDFSTOP=00d0

DIW horizontal:
- low byte de DIWSTRT/DIWSTOP influencia início/fim visível;
- DDF define fetch, DIW define janela visível.
- Não confundir fim de fetch com fim de linha.

Procurar decode atual:

grep -RniE "diwstrt|diwstop|display.*h|hstart|hstop|window|visible" src/chipset

Critério:
- O AROS referência mostra imagem centralizada, com olhos e texto mais à direita do que no Bellatrix.
- Se Denise está renderizando 640 pixels corretos, o próximo suspeito é offset/crop horizontal DIW.

6. Conferir BPLCON1 scroll/shift

Como o AROS pode usar scroll fino ou alinhamento de bitplanes:

grep -RniE "BPLCON1|bplcon1|scroll|x_phase|phase" src/chipset

Verificar:
- BPLCON1 é escrito pelo Copper?
- O valor fica 0x0c40 em algum momento?
- Denise interpreta os bits corretos?
- hires muda interpretação do scroll?

Critério:
- Se x_phase/scroll aparece não-zero, validar contra WinUAE/Omega.
- Se não há suporte, logar e confirmar que AROS usa zero no trecho principal.

7. Comparar linhas específicas com WinUAE mentalmente/log

Escolher linhas úteis:
- linha do logo AROS
- linha do texto
- linha dos ícones
- linha dos olhos

No Bellatrix, logar:
- vpos
- line index de Denise
- first non-background x
- last non-background x
- non_bg count
- first few color indices

Exemplo:

[DENISE-SPAN] line=%d bp_v=%d non_bg=%d first_x=%d last_x=%d first_idx=%02x last_idx=%02x

Critério:
- Se xspan começa cedo demais ou tarde demais, problema é offset/crop.
- Se xspan tem largura errada, problema é unpack/scale/word count.
- Se xspan correto mas pixels errados, problema é bitplane bit order ou plane composition.

8. Verificar ordem dos bits nos words

Em hires 4 bitplanes:
- Para cada pixel bit 15..0 dos words de cada plano formam color index.
- Ordem errada bit 0..15 pode espelhar blocos de 16 pixels.
- Ordem errada de planes pode alterar cores/espessura.

No Denise, revisar:
- loop bit de 15 para 0;
- color index = bitplane0 + 2*bitplane1 + 4*bitplane2 + 8*bitplane3;
- não inverter ordem dos planos.

Critério visual:
- Se formas parecem corretas mas espessura/cor errada, suspeitar plane order.
- Se blocos de 16 parecem espelhados, suspeitar bit order.

9. Validar regressão com KS1.3/Workbench

Após cada patch:

KICKSTART=src/roms/KS13.rom ADF=src/disks/wb13.adf ./run.sh harness

Critério:
- Workbench não pode voltar ao skew diagonal.
- DiagROM hires/lowres não pode piorar.
- Se AROS melhora mas Workbench piora, patch provavelmente especializou demais hires/AROS.

10. Ordem recomendada de patches

Patch A:
- Apenas ddf_words hires=40 já aplicado.
- Confirmar e manter.

Patch B:
- Garantir que Denise usa ddf_words=40 integralmente.
- Remover qualquer crop implícito para 624 se existir.

Patch C:
- Ajustar unpack hires se estiver duplicando/comprimindo pixels.

Patch D:
- Ajustar offset horizontal/DIW crop.

Patch E:
- Só se necessário, revisar BPLCON1 scroll.

Patch F:
- Só depois disso revisar beam/line lifecycle.

Resumo da hipótese atual:
- O grande bug era fetch count: 39 em vez de 40 words.
- O problema residual provavelmente está em Denise:
  1. largura renderizada ainda 624 em vez de 640;
  2. crop/offset horizontal;
  3. unpack hires/bit order;
  4. BPLCON1 scroll.
- Beam com BEAM_PAL_HPOS=454 parece plausível e não deve ser alterado agora.