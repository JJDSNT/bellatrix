---
id: SPEC-0001
title: "Integração CPU, mapa direto e barramento externo"
status: draft
created_at: 2026-07-15
updated_at: 2026-07-15
related_issues:
  - ISSUE-0032
  - ISSUE-0058
related_files:
  - authors_note.md
  - emu68/src/aarch64/vectors.c
  - emu68/src/aarch64/mmu.c
  - src/cpu/emu68/bellatrix.c
  - src/cpu/cpu_bridge.c
  - src/machine/machine_rigel_bus.c
  - src/machine/bus/zorro3/zorro3.c
---

# Objetivo

Definir uma arquitetura esparsa de memória e MMIO que preserve o caminho nativo
do Emu68, permita Musashi e outros backends e entregue cada região diretamente
ao owner de sua semântica. Esta spec descreve contratos; não declara suporte Z3
existente nem define uma `machine box` central.

# Princípios normativos

1. `vectors.c` **deve** continuar sendo a camada de integração do Emu68 com o
   barramento da plataforma, como já é para o protocolo PiStorm.
2. O fault handler original do Emu68 **deve** permanecer como mecanismo normal
   para páginas externas/MMIO.
3. Memória armazenável configurada **deve** usar o caminho direto do backend e
   **não deve** ser despachada por acesso através de Rigel ou `vectors.c`.
4. Musashi **deve** usar a mesma classificação esparsa e os mesmos owners
   semânticos usados pelo hook de `vectors.c`; não deve simular Data Abort ARM.
5. Rigel **deve** continuar dono do estado e da evolução temporal do chipset.
6. Normalização de barramento de 24 bits **não deve** truncar o endereço CPU
   universal. Wrap deve ser aplicado somente no perfil/região que o exige.
7. A política de uma região **deve** ser explícita: `DIRECT`, `EXTERNAL` ou
   `UNMAPPED`.
8. O contrato de memória/MMIO **não deve** codificar número de core. Emu68 no
   Core 0 é a baseline provisória de estabilização, não uma propriedade da ABI.
9. O dispatcher **não deve** transformar Bellatrix numa máquina opaca que recebe
   todo acesso. Ele deve ser uma classificação esparsa que chega diretamente ao
   owner da região.

# Arquitetura esparsa

`BellatrixMachine`, enquanto ainda existir no código, é somente composição e
compatibilidade transitória. Não é a fronteira arquitetural do CPU. O caminho
alvo é:

```text
endereço CPU
    -> classificação da região
       -> RAM/ROM DIRECT: mapping/bank do backend
       -> CIA EXTERNAL: semântica CIA
       -> custom EXTERNAL: semântica do bloco correspondente
       -> Autoconfig EXTERNAL: sequenciador Zorro
       -> board EXTERNAL: owner da board
       -> UNMAPPED: open bus ou bus error conforme região/perfil
```

Não deve haver callback universal de “máquina”. Pode existir um dispatcher
estático compartilhado entre backends, mas seus ramos chamam diretamente os
owners semânticos. Sincronização com outro core é propriedade do owner/região,
não motivo para encapsular toda a máquina atrás de uma caixa única.

# Placement da CPU

O placement é ortogonal à classificação de memória. Durante a estabilização,
Emu68 permanece no Core 0 para preservar o ambiente original de vetores,
faults, timers, IRQ e contexto JIT. Um backend Musashi, uma futura migração do
Emu68 ou outra composição multicore deve consumir o mesmo descritor de regiões
e o mesmo serviço externo.

Mover a CPU só é permitido depois de demonstrar equivalência em Data Abort,
MMIO, IPL/IRQ, STOP/wakeup, PMU/timers e preservação do contexto. Nenhum hook,
descritor ou owner de board pode assumir afinidade fixa com Core 0.

# Espaços de endereço

## Endereço CPU

É o endereço arquitetural produzido pelo 68k. Em 68020+ pode possuir 32 bits.
Ele é preservado até a classificação da região e nunca sofre máscara global de
24 bits.

## Barramento externo Amiga baixo

É a transação lógica usada para custom chips, CIA, RTC, Autoconfig e demais
regiões externas do espaço Amiga baixo. A normalização/mirroring pertinente é
aplicada neste domínio, depois que a região foi classificada.

## Janela de board configurada

É a faixa atribuída pelo Autoconfig. Uma board pode declarar mais de uma região
contígua, inclusive uma região direta de RAM/ROM/VRAM e páginas externas de
registradores.

O Emu68 não fixa uma faixa canônica de boards Z3. `vectors.c` recebe em
`$E80044` o high word escolhido pelo guest, calcula `map_base = value << 16` e
chama `board->map()`. As boards então instalam ROM/dados diretamente com
`mmu_map()`. Portanto Bellatrix não deve impor a política de alocação de uma
expansion.library específica.

O AROS local escolhe `0x40000000..0x7fffffff` em slots de 16 MiB e conhece uma
janela em `0xff000000`; isso é evidência de compatibilidade, não fonte
normativa. A antiga constante Bellatrix `0x10000000`, sem uso, continua
removida. A faixa aceita será consequência dos descritores, alinhamento,
ausência de sobreposição e capacidade de `map()` do backend.

# Tipos de região

## Matriz esparsa inicial

Esta matriz define o destino arquitetural, não afirma que o código atual já
esteja decomposto dessa forma:

| Região lógica | Classe | Owner/rota alvo |
|---|---|---|
| Chip RAM e RAM armazenável | `DIRECT` | mapping Emu68 ou bank/buffer Musashi; Rigel apenas observa DMA/tempo |
| ROM e Extended ROM | `DIRECT` | mapping/bank read-only do backend |
| CIA A/B | `EXTERNAL` | semântica CIA, com sincronização local quando necessária |
| Custom registers | `EXTERNAL` | bloco Rigel correspondente, não gateway genérico de máquina |
| `$E80000` Autoconfig | `EXTERNAL` | sequenciador Zorro compartilhado |
| RAM/ROM/VRAM de board configurada | `DIRECT` | região instalada pelo backend no fim do Autoconfig |
| Registradores de board | `EXTERNAL` | owner da board/página |
| Endereço sem região | `UNMAPPED` | open bus ou bus error definido pelo perfil |

O fault adapter do Emu68 só recebe páginas que não são `DIRECT`; portanto seu
hot path deve despachar `EXTERNAL` diretamente. Musashi classifica primeiro os
banks diretos e usa o mesmo dispatcher esparso somente para o restante.

Callbacks de `map/unmap` são permitidos no lifecycle de Autoconfig, fora do hot
path. Function pointers por acesso MMIO não são parte do contrato alvo.

## Semântica da transação externa

O classificador recebe o endereço CPU integral antes de qualquer wrap, a
direção e a largura. Para o barramento Amiga baixo, valores de 16 e 32 bits são
representados em ordem numérica big-endian: o byte no menor endereço ocupa os
bits mais altos, igual aos helpers Zorro atuais e ao host big-endian do Emu68.

As primitivas externas obrigatórias são 8, 16 e 32 bits. Acessos faultados de
64/128 bits existem no decoder AArch64 do Emu68, mas não podem ser truncados
para 32 bits como ocorre na ponte transitória atual. O adapter só poderá
decompô-los em lanes menores quando a região declarar que isso preserva seus
efeitos e sua atomicidade; caso contrário o owner retorna comportamento
`UNMAPPED`/bus error definido pelo perfil.

O resultado lógico possui três estados, mesmo que a primeira implementação os
codifique de forma compacta:

- `HANDLED`: owner concluiu a transação e, em read, produziu o valor;
- `OPEN_BUS`: região responde sem owner, com valor definido pelo perfil/largura;
- `BUS_ERROR`: acesso arquiteturalmente inválido, entregue ao backend como
  exceção 68k, não como crash ARM.

Normalização de mirrors de 24 bits ocorre somente depois que o endereço foi
classificado como barramento Amiga baixo. Endereços Z3 nunca passam por
`bellatrix_bridge_normalize_addr()`.

## `DIRECT`

Memória sem efeitos colaterais por acesso:

- Emu68: região instalada/removida com MMU e acessada por load/store AArch64;
- Musashi: bank/buffer instalado na tabela do backend;
- o owner fornece backing, tamanho, permissões e cacheabilidade;
- writes em ROM são tratados por proteção/política da região;
- não há chamada ao hook no steady state.

Exemplos: Chip RAM, Fast RAM Z2/Z3, ROM, VRAM armazenável e dados de uma board.

## `EXTERNAL`

Região com semântica de dispositivo ou efeito colateral:

- Emu68: página ausente/protegida causa fault e entra por `vectors.c`;
- Musashi: decoder chama diretamente o hook;
- o hook conclui a transação de forma síncrona ou aplica a política publicada/
  postável que preserve a semântica observável;
- o estado do dispositivo pertence a Rigel ou à board registrada.

Exemplos: custom registers, CIA, janela de Autoconfig e registradores Z3.

## `UNMAPPED`

Região sem responder. Cada perfil define open bus ou bus error, mas Emu68 e
Musashi devem observar a mesma política. Um acesso alto que faulta no Emu68 não
deve ser confundido com uma board Z3 mapeada.

# Interface estável exposta por `vectors.c`

Bellatrix não deve manter uma substituição extensa do bloco PiStorm em
`vectors.c` a cada atualização do Emu68. A mudança desejada é uma fronteira
pequena, estável e aceitável upstream:

- `vectors.c` preserva o decoder de Data Abort, contexto e retomada do JIT;
- operações de barramento e mapping dependentes da plataforma são exportadas
  por uma interface estreita;
- o adapter PiStorm continua sendo a implementação padrão;
- Bellatrix fornece outra implementação em arquivo próprio;
- o backend Musashi chama a implementação Bellatrix da mesma fronteira sem
  passar pela vector table.

Para não acrescentar overhead desnecessário, a preferência é por símbolos
diretos resolvidos no link/compile-time, com funções separadas por operação
quando isso evitar switches, e não por registro dinâmico, tabela de function
pointers ou descriptor lookup dentro de todo fault. Uma única alteração
upstreamável no Emu68 substitui a série de patches Bellatrix sobre o corpo de
`vectors.c`.

O escopo mínimo a expor deve ser derivado do que hoje é PiStorm-specific no
vetor:

- read/write externos por largura;
- política de endereço não mapeado;
- conclusão de Autoconfig e instalação de mapping da board;
- overlay e sombras de interrupção apenas onde forem realmente parte do
  contrato Emu68, sem transferir estado do chipset para o adapter.

# Fluxo do hook Emu68

O fluxo normativo é:

```text
load/store JIT em página EXTERNAL
  -> Data Abort síncrono
  -> decoder existente de vectors.c
  -> bellatrix_external_bus_{read,write}(addr, value, size)
  -> Rigel/board/política de máquina
  -> valor/resultado
  -> retomada do JIT
```

O nome da função acima é conceitual; a ABI existente pode conservar
`bellatrix_bus_access()` enquanto for compatível. A interface exposta pelo
Emu68 não deve, contudo, ter nome ou tipo específico do Bellatrix.

O vetor deve apenas preservar contexto, extrair endereço/largura/direção,
invocar o hook e retomar. Estado de chipset, scheduler e stacks físicas não
pertencem a `vectors.c`.

# Contrato de custo

O hook normal:

- deve possuir ABI pequena e chamada direta;
- não aloca memória e não chama logger, BT, USB ou host scheduler;
- não reclassifica nem normaliza o mesmo endereço em múltiplas camadas;
- oferece fast paths para polls publicados e writes postáveis;
- sincroniza com o core do Rigel somente quando necessário à semântica;
- compila tracing e profiling para fora da configuração de produto.

O hook não define a fonte de tempo do Rigel. Em particular,
`bellatrix_emu68_report_jit_progress()` é uma adaptação Bellatrix posterior e
ainda não validada; não faz parte do contrato original do fault handler. O
handler pode oferecer um ponto de sincronização antes da transação, mas não
deve assumir que faults são o relógio que mantém o chipset vivo.

O custo do Data Abort original é aceito como baseline. Mudanças nele exigem
medição e prova de compatibilidade; não são pré-requisito de integração.

# Integração Musashi

O backend Musashi classifica o endereço CPU sem truncamento universal:

```text
DIRECT   -> bank/buffer
EXTERNAL -> mesma implementação final do hook Bellatrix
UNMAPPED -> política comum de open bus/bus error
```

O decoder comum pode compartilhar descritores e política com Emu68, mas não
deve obrigar o hot path direto do Emu68 a fazer lookup por acesso.

# Integração Rigel

Rigel recebe apenas transações `EXTERNAL` do chipset. Fast RAM e VRAM direta
não são acessadas byte a byte por Rigel. DMA que precise enxergar memória usa o
backing compartilhado ou uma API de memória em bloco, não o hook MMIO do CPU.

A fonte de tempo do Rigel ainda precisa ser rebaselineada. A hipótese
prioritária é um timer/scheduler mínimo independente do número de faults, como
sugerido nas notas do autor, com catch-up síncrono apenas quando um acesso
observável exigir. O modelo atual de progresso publicado pelo CPU permanece
evidência experimental, não decisão normativa.

No baseline Core0=Emu68 atual, a timeline wall-clock ainda pertence ao antigo
loop Core0=supervisor e não é atualizada. O event stream do Core 2 apenas o
acorda; não aumenta seu horizonte. Essa dependência deve ser removida antes de
considerar o modelo temporal assimilado.

Com Rigel em outro core:

- reads mutáveis/critical writes podem exigir catch-up e lock;
- polls publicados devem evitar rendezvous;
- writes postáveis devem carregar o timestamp lógico necessário;
- ownership único do stepper não impede acesso síncrono protegido ao estado,
  mas qualquer acesso concorrente deve obedecer ao contrato de lock/snapshot.

# Lifecycle de Autoconfig e boards

1. No reset, a board ainda não responde em sua futura janela.
2. A janela baixa de Autoconfig responde como `EXTERNAL`.
3. Ao receber a atribuição completa de base, o sequenciador valida alinhamento,
   tamanho e faixa.
4. O backend instala todas as regiões declaradas pela board.
5. Somente depois da instalação a board é marcada configurada/visível.
6. Reset, shutup ou remoção desfazem as regiões antes de liberar o backing.

O contrato deve suportar rollback se uma região não puder ser instalada. Não
deve existir estado parcialmente configurado visível ao guest.

# Ordem de implementação

1. Separar endereço CPU 32-bit de normalização do barramento baixo.
2. Definir descritor de região e callbacks backend `map/unmap`.
3. Adaptar Z2 existente sem mudar seu comportamento observável.
4. Implementar Z3 Fast RAM como primeiro caso `DIRECT`.
5. Implementar regiões mistas somente após o caso RAM estar provado.
6. Afinar o hot path `vectors.c -> hook -> Rigel` com perfil, preservando
   sincronização temporal.

# Critérios para tornar a spec `active`

- bases/faixas Z3 reconciliadas e justificadas;
- lifecycle map/unmap revisado para Emu68 e Musashi;
- matriz de regiões do mapa Amiga documentada;
- hook não contém trabalho físico ou alocação;
- Z2 mantém a regra de invisibilidade antes de Autoconfig;
- pelo menos um teste de contrato por backend para `DIRECT`, `EXTERNAL` e
  `UNMAPPED`;
- validação em hardware permanece fora do escopo até autorização do usuário.

# Fora do escopo desta spec

O contrato de IRQ física, housekeeper e injeção `INT.ARM` é acompanhado em
ISSUE-0058. A interface de barramento exposta por `vectors.c` não deve misturar
IRQ do host com MMIO guest; são pontos próximos no mesmo arquivo, mas contratos
distintos.

O esclarecimento vigente é que o housekeeper PiStorm encaminha o IPL físico do
Amiga a `M68KState.INT.IPL`. `INT.ARM` é reservado a notificações explícitas de
um serviço ARM para o AmigaOS, não ao atendimento normal de periféricos do host.
