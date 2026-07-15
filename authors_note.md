# Notas técnicas sobre integração do Emu68 e emulação do chipset

## MMIO e page fault handler

Para emular dispositivos MMIO, como o chipset do Amiga, existem poucas alternativas práticas ao uso do mecanismo de page fault handler.

Talvez seja possível reduzir parte do overhead utilizando técnicas baseadas em hypervisor, mas, em um ambiente emulado, o mecanismo de page fault deve ser mais do que rápido o suficiente para lidar com o Amiga.

Somente os 24 bits inferiores do espaço de endereçamento, correspondentes ao espaço do Amiga, precisam atingir o page fault handler.

Esse espaço opera com um ciclo de aproximadamente 540 ns. Esse timing é parte do comportamento esperado do Amiga: executar o sistema significativamente mais devagar ou mais rápido pode causar problemas de compatibilidade, inclusive em jogos.

Fast RAM e dispositivos Zorro III não precisam passar pelo page fault handler. Esses acessos podem ir diretamente para a memória ARM.

O handler também pode servir como ponto de integração para outros emuladores de CPU. Um backend como Musashi pode ser conectado nesse ponto da mesma forma que o acesso ao hardware é encaminhado pelo Emu68.

## IRQ e FIQ

Para a emulação do chipset Amiga, deve ser utilizada uma IRQ ARM normal.

Não há necessidade de utilizar FIQ para um periférico tão lento quanto o chipset do Amiga.

O FIQ deve ser preservado para dispositivos com requisitos temporais mais críticos. USB é um possível uso futuro, especialmente com o controlador DWC2, que provavelmente precisa de atendimento adequado dos eventos SOF.

Portanto, a sugestão é usar IRQ para o Amiga e manter o FIQ disponível para USB/DWC2.

## Startup e preparação dos cores no Emu68

É importante observar cuidadosamente o startup do Emu68.

Em particular, deve ser analisada a parte em assembly responsável por preparar e inicializar os cores ARM.

Esse código é relevante para entender como os cores são configurados antes da execução normal do Emu68.

## `vectors.c` e roteamento de hardware

O arquivo `vectors.c` contém grande parte da lógica de interface entre o Emu68 e o PiStorm.

É nesse ponto que os acessos ao hardware são roteados para o PiStorm.

O mesmo mecanismo pode ser utilizado para encaminhar esses acessos para a emulação do chipset.

Assim, `vectors.c` é um ponto central a ser estudado para entender como substituir ou adaptar o roteamento destinado ao hardware PiStorm para um backend de chipset emulado.

## Scheduler mínimo e serviços no mesmo core

Pode ser uma boa ideia manter um scheduler mínimo em um dos cores.

Com isso, a emulação do Amiga e tarefas como Bluetooth ou USB poderiam compartilhar um core.

A emulação do Amiga já precisa de timer, e IRQs também são úteis. Com um scheduler pequeno, esses elementos podem ser reunidos no mesmo core.

Se o objetivo não for atingir o nível de precisão de emulação do WinUAE, mas sim uma emulação funcional, semelhante ao código UAE anteriormente indicado e baseado em uma versão mais antiga do UAE, a emulação do chipset provavelmente não utilizará toda a capacidade de um core ARM do Raspberry Pi.

Nesse cenário, o core ARM responsável por essa carga terá tempo disponível e poderá também executar tarefas como Bluetooth em paralelo.
