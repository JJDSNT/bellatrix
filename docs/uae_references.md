Eu acho **muito relevante para o Bellatrix/Rigel**, mas não como base direta para substituir o Rigel. O valor maior está em mostrar, na prática, **como um UAE antigo foi reduzido até rodar em um microcontrolador ARM limitado**, mantendo Amiga OCS, som, ADF/HDF e CPU 68000/68020.

O ponto mais importante: esse port usa **UAE 0.6.9**, uma arquitetura muito antiga, simples e monolítica. O próprio diretório preserva a separação clássica em `custom.c`, `cia.c`, `disk.c`, `audio.c`, `memory.c`, `newcpu.c`, `blitter.c` e uma tabela global de eventos. Ele declara suporte a OCS, 68000/68020, até 2 MB de Chip RAM, Fast RAM, floppy, hard disk e áudio. ([GitHub][1])

Isso combina quase exatamente com o comentário que você recebeu: não buscar uma emulação “WinUAE-perfect”, mas uma emulação funcional baseada em UAE antigo, suficientemente leve para deixar o ARM relativamente ocioso. 

## O que ele demonstra

O Teensy 4.1 é um Cortex-M7 single-core, com clock alto, mas muito mais restrito que o Raspberry Pi 3B em memória, sistema, MMU e capacidade geral. Mesmo assim, o projeto afirma rodar Amiga em full speed com som, até 2 MB de Chip RAM, 4 MB de Fast RAM, ADF, HDF, mouse e teclado USB. ([GitHub][2])

Portanto, ele é uma evidência bastante forte de que:

> O código de chipset derivado de UAE antigo não deveria, sozinho, impedir um Raspberry Pi 3B de alcançar 50 Hz.

Isso reforça sua suspeita de que o gargalo atual provavelmente não está apenas nos algoritmos básicos de Agnus/Denise/Paula, mas em alguma combinação de:

* integração CPU–Rigel;
* granularidade excessivamente fina de sincronização;
* indireções e abstrações no hot path;
* desenho de vídeo;
* contenção/cache entre cores;
* MMIO/eventos;
* instrumentação de debug;
* ou alguma regressão concreta.

O Teensy não possui quatro cores para mascarar custos. Se consegue entregar o sistema inteiro em um Cortex-M7, vale estudar cuidadosamente o seu fluxo principal.

## A parte mais interessante: áudio adaptativo

Há um trecho especialmente relevante para o seu problema atual de áudio. O port não assume que a taxa de produção do emulador e a taxa do dispositivo de áudio permanecerão perfeitamente sincronizadas.

Ele mantém ponteiros fracionários no buffer de áudio e calcula um incremento variável de leitura. O código observa a distância entre o produtor e o consumidor e ajusta gradualmente `sndinc`, incluindo correções e reinicialização quando o buffer se aproxima de underflow ou overflow. ([GitHub][3])

Em essência, ele faz algo parecido com:

```text
Paula/UAE produz amostras
        ↓
buffer circular
        ↓
leitor com posição fracionária
        ↓
interpolação/repetição controlada
        ↓
I2S na frequência física fixa
```

O cálculo usa posição de leitura em ponto fixo e escolhe amostras com base nessa posição. Pelo trecho visível, parece ser uma forma simples de resampling, provavelmente sem interpolação linear sofisticada, mas com **controle adaptativo da velocidade de consumo**.

Isso é muito próximo da solução temporária que você estava imaginando para o HDMI:

* manter o HDMI em sua frequência correta;
* deixar Paula produzir conforme o tempo emulado;
* absorver pequenas diferenças com um resampler;
* controlar a ocupação do ring buffer;
* evitar que pequenas oscilações de FPS virem pitch drift, cortes ou estouros.

Entretanto, esse mecanismo só resolve diferenças relativamente pequenas e jitter. Se o Rigel estiver entregando apenas 1/3 ou 1/4 da velocidade necessária, ele não reconstruirá o áudio original. Nesse caso ele produzirá áudio esticado, repetitivo ou artificial. Continua sendo útil como estabilizador depois que você estiver perto de 50 Hz.

## O que eu estudaria no código

### 1. Scheduler de eventos

O arquivo `events.h` e o uso de `eventtab` representam o scheduler clássico do UAE. Em vez de avançar cada componente por chamadas abstratas muito frequentes, o sistema trabalha com eventos agendados, como áudio, Copper, disco e outros componentes. ([GitHub][4])

Esse modelo pode ajudar muito o Rigel:

```text
agora = chipset_cycle

while agora < limite:
    próximo = menor evento agendado
    avançar diretamente até próximo
    executar apenas o componente associado
```

Isso evita executar, a cada ciclo:

```c
step_agnus();
step_denise();
step_paula();
step_cia();
step_copper();
step_blitter();
```

mesmo quando quase todos não têm nada para fazer.

O Rigel já caminha conceitualmente para `rigel_get_next_event_ns()`. Este UAE é uma boa referência concreta de como uma arquitetura antiga e simples organiza isso.

### 2. `custom.c`

O `custom.c` é provavelmente a parte mais valiosa para comparação com o Rigel. Ele concentra:

* raster;
* bitplanes;
* Copper;
* sprites;
* DMA;
* interrupções;
* registradores custom;
* geração de linhas;
* eventos horizontais e verticais.

Não recomendaria copiar o arquivo. Ele é global, antigo e muito acoplado. Mas compararia funções equivalentes em pares:

```text
UAE custom.c       Rigel
--------------------------------
hsync_handler      avanço horizontal
vsync_handler      VBlank/frame
do_copper          Copper
decide_line        decisão de linha
decide_fetch       janela de fetch
custom_wget/wput   custom_read16/write16
INTREQ/INTENA      IRQ Rigel
```

Essa comparação pode revelar trabalho que o Rigel está fazendo com granularidade excessiva.

### 3. Renderização por linha

O port usa um buffer de linha (`slinebuf`) e entrega a linha ao backend com `flush_line()`. Não parece construir necessariamente um framebuffer intermediário completo para cada etapa. ([GitHub][3])

Isso é particularmente importante para sua dúvida sobre o caminho de vídeo. Uma arquitetura eficiente costuma ser:

```text
estado dos bitplanes
      ↓
renderizar linha pronta
      ↓
converter diretamente para formato de saída
      ↓
entregar ao framebuffer/DMA
```

Em vez de:

```text
render interno
      ↓
framebuffer Rigel
      ↓
cópia
      ↓
conversão
      ↓
framebuffer Bellatrix
      ↓
scanout HDMI
```

Mesmo que o “no-copy” atual não seja o culpado, este port oferece uma referência simples para medir quantos passes sobre os pixels realmente são necessários.

### 4. Immediate blits e simplificações

O UAE antigo oferece opções para concluir blits imediatamente e usar operações de 32 bits. O próprio README reconhece explicitamente a troca entre precisão e velocidade. ([GitHub][1])

Para o Bellatrix, isso sugere separar claramente:

```text
RIGEL_ACCURATE
RIGEL_FUNCTIONAL
RIGEL_FAST
```

Não necessariamente como produto final, mas como ferramenta de diagnóstico. Por exemplo:

* blitter instantâneo;
* Copper em granularidade reduzida;
* sprites desabilitados;
* áudio desabilitado;
* renderização desabilitada;
* apenas VBlank e registradores;
* sem colisões;
* sem fetch real de bitplanes.

Se desabilitar o render levar de 5 para 50 FPS, você encontrou a classe do gargalo. Se não mudar quase nada, o problema está na integração/sincronização.

## O que não aproveitar diretamente

Eu não usaria esse projeto como nova base do Rigel porque:

* é UAE 0.6.9, muito antigo;
* há muito estado global;
* CPU e chipset são fortemente acoplados;
* a API não foi desenhada como biblioteca embutível;
* há código específico de Arduino/Teensy misturado ao port;
* precisão e compatibilidade são inferiores às bases modernas;
* a licença histórica desse UAE merece uma revisão cuidadosa antes de copiar código;
* ele não resolve a integração Emu68 por MMIO/page fault;
* é essencialmente single-thread, enquanto seu desenho atual tem CPU e chipset em cores diferentes.

A licença incluída no README é uma licença histórica própria, com permissões e restrições diferentes de GPL, MIT ou BSD. Eu trataria o código inicialmente como **referência de implementação**, não como material automaticamente copiável. ([GitHub][1])

## Minha conclusão para o Bellatrix

Eu colocaria esse código entre as referências mais úteis para você neste momento, provavelmente acima de WinUAE para a investigação específica de desempenho.

Não porque seja tecnicamente superior ao WinUAE, mas porque ele responde melhor à pergunta atual:

> Qual é a menor quantidade de arquitetura necessária para rodar um Amiga OCS funcional em ARM limitado?

O WinUAE mostra máxima compatibilidade e décadas de refinamento. O TeensyUAE mostra **o mínimo funcional que consegue rodar rápido**.

Minha recomendação concreta seria usá-lo para três trabalhos:

1. Comparar o scheduler de eventos e o hot path de `custom.c` com o Rigel.
2. Adaptar o conceito de ring buffer com consumo fracionário para o áudio HDMI.
3. Criar no Rigel um modo de benchmark “UAE antigo”, progressivamente habilitando Copper, bitplanes, blitter, sprites e áudio.

A descoberta mais importante talvez seja esta: **se esse UAE roda o Amiga completo em um Cortex-M7, o Raspberry Pi 3B não está sem potência; o seu problema é arquitetural, de integração ou uma regressão mensurável.**

[1]: https://github.com/Jean-MarcHarvengt/MCUME/tree/master/MCUME_teensy41/teensyuae "MCUME/MCUME_teensy41/teensyuae at master · Jean-MarcHarvengt/MCUME · GitHub"
[2]: https://github.com/Jean-MarcHarvengt/MCUME "GitHub - Jean-MarcHarvengt/MCUME: Multi CompUter Machine Emulator for several MCUs · GitHub"
[3]: https://github.com/Jean-MarcHarvengt/MCUME/raw/refs/heads/master/MCUME_teensy41/teensyuae/wrapemu.cpp "raw.githubusercontent.com"
[4]: https://github.com/Jean-MarcHarvengt/MCUME/blob/master/MCUME_teensy41/teensyuae/events.h "MCUME/MCUME_teensy41/teensyuae/events.h at master · Jean-MarcHarvengt/MCUME · GitHub"
