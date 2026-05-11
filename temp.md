
● Encontrei o bug! agnus_read() usa (void)size — ignora o tamanho. Quando o CPU faz leitura de byte de $DFF002, o harness devolve a palavra completa de DMACONR, e o Musashi pega o byte baixo ($DFF003 = 0xd0
  com BLTEN=1 no bit 6) em vez do byte alto ($DFF002). O bit 6 do byte errado está sempre 1 → loop infinito.

  Há também um erro secundário nas constantes de bit: BBUSY=0x4000 (bit 14) e BZERO=0x2000 (bit 13), mas no hardware real BBUSY=0x8000 (bit 15) e BZERO=0x4000 (bit 14).

  Vou verificar se paula_read e denise_read têm o mesmo problema antes de corrigir tudo de uma vez.

  Searched for 1 pattern (ctrl+o to expand)

● Bash(sed -n '245,260p' /home/jaime/bellatrix/src/chipset/paula/paula.c && echo "---" && sed -n '263,278p' /home/jaime/bellatrix/src/chipset/denise/denise.c)
  ⎿  uint32_t paula_read(Paula *p, uint32_t addr, unsigned int size)
     {
         (void)size;
     … +30 lines (ctrl+o to expand)

● Ambos têm (void)size. Vou corrigir tudo de uma vez: extração de byte em agnus_read/paula_read, e as constantes de bit BBUSY/BZERO nas definições.

  Primeiro vou verificar o estado inicial de blitter.zero:

  Searched for 2 patterns, read 1 file (ctrl+o to expand)

● blitter.zero = 1 no init, e a callback agnus_dma_blitter_zero_cb retorna s->blitter.zero. O loop vai continuar girando enquanto blitter.zero=1. Preciso ver se a blitter executa e atualiza esse flag. Vou ler
  blitter.c ao redor do trecho onde zero é calculado:

  Read 1 file (ctrl+o to expand)

● Vou checar se a blitter está sendo acionada durante o boot, e depois fazer os dois fixes (extração de byte + constantes BBUSY/BZERO) de uma vez:


When writing the low word of a Blitter pointer register, Omega keeps all 16 bits.
SAE / UAE always deletes the least significant bit which makes the address even:

bltapt = ((bltapt & 0xffff0000) | (v & 0xfffe)) >>> 0;
Although it has no effect on the copy Blitter in Omega, the line Blitter sometimes computes different results if the least significant bit is set or cleared.

Yeah, this is a known issue with my linedraw algorithm.

Due to it internally using native CPU sized variables for the calculations, if you lose the least significant bit, then the lines don’t draw correctly under certain conditions.

My linedraw algorithm is not what Angus actually uses... as I said before, it’s good enough to get you going, but you will probably have to replace it for accuracy in future 