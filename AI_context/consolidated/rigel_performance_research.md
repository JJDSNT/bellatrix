# Pesquisa histórica para performance do Rigel

Este documento preserva as conclusões úteis de `rigel_perf.md`. Ele **não é um
tracker**. Qualquer trabalho futuro só se torna ativo quando entrar na
ISSUE-0052 com uma medição que demonstre gargalo interno no Rigel.

## Gate antes de alterar o chipset

Separar por wall-time e por frame:

- tempo exclusivo dentro de `rigel_step_until`;
- espera e sincronização da integração;
- composição e apresentação;
- número de chamadas ao Rigel;
- CCK virtuais por chamada e por segundo de parede;
- custo separado de Agnus, Denise, Paula, CIA, Copper e blitter.

Muitas chamadas curtas indicam granularidade ruim da integração. Poucas chamadas
longas e alto tempo exclusivo indicam hot path interno. Nenhuma otimização do
Rigel deve ser escolhida antes dessa distinção.

## Hipóteses preservadas para A/B

1. **Scheduler orientado a eventos:** saltar até o próximo evento observável em
   vez de executar clocks vazios individualmente.
2. **Copper rápido com fallback preciso:** executar casos simples diretamente e
   retornar ao caminho exato diante de registradores ou waits sensíveis.
3. **Blitter timed-functional:** calcular a operação em bloco, preservando
   `BBUSY`, conclusão e IRQ no instante emulado correto; manter caminho exato
   para contenção ou observação intermediária.
4. **Renderização segmentada por scanline:** registrar mudanças de estado com
   posição horizontal e renderizar segmentos ao final da linha, caso o perfil
   mostre Denise/pixels como domínio dominante.
5. **Kernels somente depois da arquitetura:** NEON, expansão de bitplanes,
   minterms, shifts/fill, dual-playfield, sprites, HAM/EHB e colisões só entram
   após o perfil apontar esses kernels.

## Referências históricas

- [Amiberry-Lite](https://github.com/BlitterStudio/amiberry-lite) e UAE4ARM:
  referências ARM para scheduler de eventos e fast paths. Procurar especialmente
  `custom.cpp` (Agnus, Copper, registradores, DMA e sincronização),
  `drawing.cpp` (linhas e composição), implementação do blitter, cálculo do
  próximo evento, Fast Copper e Immediate Blitter.
- [WinUAE](https://github.com/tonioni/WinUAE): oráculo de correção e fonte de
  algoritmos para chipset, Copper, blitter, DMA e modos de vídeo. O estado
  global e a integração com a CPU/host são muito acoplados para transplante
  direto; comparar algoritmos e invariantes, não copiar o subsistema inteiro.
- [Discussão Fast Copper no Amiberry](https://github.com/midwan/amiberry/issues/654):
  evidência de que o fast path precisa de fallback preciso e gate de
  compatibilidade para jogos/demos.
- [Omega](https://github.com/h5n1xp/Omega): referência bare-metal para blitter
  em modo immediate; estudar a separação entre cálculo funcional da operação e
  manutenção do timing observável (`BBUSY`, conclusão e IRQ).

Áreas potencialmente transplantáveis como kernels puros:

- minterms booleanos, shifts A/B e fill carry do blitter;
- cálculo de `WAIT/SKIP` do Copper;
- expansão de bitplanes e composição dual-playfield;
- prioridade de sprites, HAM, EHB e colisões.

Áreas para aproveitar apenas como desenho/algoritmo:

- scheduler do custom chipset e controle/arbitragem de DMA;
- início/fim de scanline e eventos raster;
- sincronização antes de registradores observáveis.

Evitar transplantar preferências, frameskip, sincronização SDL/host, threading
do Amiberry ou o estado global completo de `custom.cpp`: esses componentes não
correspondem ao protocolo multicore do Bellatrix.

Essas ideias devem sempre ter fallback, feature flag, teste de equivalência e
A/B por domínio. KS1.3/Battle continuam gates de compatibilidade, não métricas
da velocidade geral da máquina.
