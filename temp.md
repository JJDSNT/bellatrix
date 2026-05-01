• Achados

  1. Alta: o scroll fino em lowres está sendo lido do nibble errado de BPLCON1, então o alinhamento horizontal do display pode ficar deslocado ou sumir.
     Em Bellatrix, src/chipset/denise/denise.c:135 usa bplcon1 >> 4 para lowres e o nibble baixo só para hires. No referencias/emu, o caminho de output usa *BPLCON1 & 0xF tanto no trecho hires quanto no
     lowres, por exemplo em referencias/emu/c/omega2/chipset/agnus/Bitplane.c:514 e referencias/emu/c/omega2/chipset/agnus/Bitplane.c:707. Se o boot screen depende do scroll padrão da copperlist, o Bellatrix
     começa a desenhar na fase errada.
  2. Alta: o Bellatrix calcula a fase DIW/DDF, mas na prática ignora esse resultado e ancora sempre a imagem em src_first_pixel = 0.
     A helper existe em src/chipset/denise/denise.c:148, mas no render ela nunca é usada; phase_pixels fica 0 e o código força src_first_pixel = 0 em src/chipset/denise/denise.c:683 e src/chipset/denise/
     denise.c:698. No referencias/emu, o output nasce do relacionamento entre beam, DIWSTRT/DIWSTOP, DDFSTRT/DDFSTOP e BPLCON1, não de uma linha já pronta “ancorada” no zero, por exemplo em referencias/emu/c/
     omega2/chipset/agnus/Bitplane.c:509 e referencias/emu/c/omega2/chipset/agnus/Bitplane.c:699. Isso é um candidato forte para explicar “CPU roda, mas o boot screen não aparece”.
  3. Alta: DIWHIGH não existe no modelo do Bellatrix.
     No referencias/emu, DIWHIGH é registrado em referencias/emu/c/omega2/chipset/Chipset.c:1564. No Bellatrix não há constante nem handler correspondente em src/chipset/agnus/agnus.h:21 e src/chipset/agnus/
     agnus.c:523. Além disso, o cálculo de janela usa só os 8 bits baixos/altos de DIWSTRT/DIWSTOP em src/chipset/agnus/bitplanes.c:37 e src/chipset/denise/denise.c:102. Se a ROM ou a copperlist mexe nos bits
     altos da janela, o Bellatrix simplesmente ignora.
  4. Média: o Bellatrix está mascarando um problema de DMA de bitplane com um fallback não-hardware.
     Em src/chipset/agnus/bitplanes.c:97, se DMAEN está ligado mas BPLEN não, o código ainda assim força fetch se achar BPLCON0 e ponteiros “coerentes”. O referencias/emu não faz isso; o fetch depende do
     estado DMA real. Isso talvez não seja a causa direta da tela preta, mas mostra que o pipeline ainda não está fechado nos mesmos contratos do emu de referência.
  5. Média: a arquitetura de vídeo do Bellatrix ainda é mais “linha pronta” do que “pipeline de fetch + output”.
     O Bellatrix captura palavras da linha em src/chipset/agnus/bitplanes.c:328, marca line_ready e só depois compõe tudo em src/chipset/denise/denise.c:562. O referencias/emu mistura fetch e saída ao
     framebuffer no mesmo fluxo de beam/DMA em referencias/emu/c/omega2/chipset/agnus/Bitplane.c:699. Isso importa para start/stop reais do display, reset de HAM, scroll e qualquer efeito de phase/window. Não
     é um bug isolado, é uma diferença estrutural.

  Leitura geral

  Se eu tivesse que apostar em um motivo principal para o referencias/emu chegar no boot screen e o Bellatrix não, eu começaria por estes três pontos, nesta ordem:

  1. BPLCON1 lowres interpretado errado.
  2. Fase horizontal DIW/DDF efetivamente ignorada no Denise.
  3. Ausência total de DIWHIGH.

  O copper do Bellatrix não me parece o primeiro culpado por leitura estática; a discrepância mais gritante está mesmo no pipeline de vídeo.

  Próximo passo por código

  Eu atacaria assim:

  1. Corrigir denise_fine_scroll_pixels() para lowres usar o mesmo nibble do referencias/emu.
  2. Fazer o render usar de verdade a fase calculada entre DDFSTRT, DIWSTRT e BPLCON1, em vez de zerar src_first_pixel.
  3. Adicionar DIWHIGH ao estado de Agnus/Denise e refazer os helpers de janela para considerar os bits altos.

  Se quiser, eu posso implementar exatamente esses três ajustes agora.