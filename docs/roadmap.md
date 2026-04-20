
# Bellatrix — Roadmap de Implementação para o Modelo Arquitetural Atual

## Contexto

Foi consolidada uma decisão arquitetural mais precisa para Bellatrix:

> **o chipset é o dono do tempo observável da máquina**

Essa formulação substitui a ideia anterior de uma “machine dona do tempo” ou de um “step central” como origem semântica do sistema.

A `machine` continua existindo, mas seu papel correto é outro:

* integrar CPU, chipset e protocolos
* manter a composição concreta da máquina
* centralizar init/reset/wiring
* oferecer o ponto de entrada do barramento
* publicar IPL para a CPU

Ela **não** deve ser tratada como um scheduler abstrato que impõe tempo aos chips.

O tempo observável da máquina nasce do comportamento do chipset, em especial de:

* arbitragem de DMA
* estados de registradores custom
* evolução de beam/raster
* latências observáveis do mundo chip
* geração de interrupções
* prontidão real para conclusão de transações

O protocolo do PiStorm deixa isso claro: o Emu68 não é fonte do tempo; ele é o lado que executa CPU e emite transações, enquanto o outro lado do protocolo define quando elas realmente se completam. Em Bellatrix, esse “outro lado” passa a ser o próprio chipset em software.

---

## Objetivo

Migrar o codebase de forma consistente para esse modelo arquitetural, garantindo que:

* o **chipset** seja a fonte do tempo observável
* a **machine** seja o ponto de integração estrutural
* o **barramento** seja tratado como protocolo de sincronização
* o **Emu68** seja tratado como executor de CPU, emissor de acessos e consumidor de IPL
* o **DMA** pertença ao domínio do Agnus
* `bellatrix.c` deixe de preservar o modelo antigo
* o codebase pare de carregar a arquitetura anterior de forma implícita

---

## Decisão arquitetural

A decisão correta agora é esta:

> **Bellatrix deve ser organizado como uma máquina em que o chipset define o tempo observável, a machine integra os componentes, e a CPU progride por meio de protocolos de sincronização com esse estado.**

Portanto:

* não tratar `machine` como scheduler soberano
* não tratar `emu68_step()` como centro conceitual do sistema
* não espalhar a coerência arquitetural em singletons e glue informal
* não reduzir o barramento a simples decode de endereço
* não manter o modelo antigo como referência implícita
* não usar “compatibilidade temporária” para preservar ownership errado

---

## Princípios do modelo atual

O modelo atual parte das seguintes ideias.

### 1. O chipset é dono do tempo

O tempo observável da máquina nasce do chipset.

Isso inclui:

* progressão de estados internos relevantes
* prontidão de resposta de hardware
* eventos de DMA
* evolução de beam e vídeo
* busy/wait observável
* interrupções e seus gatilhos

### 2. O Emu68 não é dono do tempo

O Emu68 deve ser tratado como:

* executor de instruções M68K
* emissor de transações de barramento
* consumidor de nível de IPL

Ele não deve ser tratado como:

* scheduler principal da máquina
* fonte do relógio semântico do sistema
* árbitro de DMA
* autoridade temporal sobre o chipset

### 3. O barramento é protocolo, não apenas roteamento

A camada de barramento deve:

* receber a transação emitida pela CPU
* identificar o alvo
* consultar ou sincronizar o chipset quando necessário
* impor espera quando o estado do hardware exigir
* concluir a transação apenas quando ela puder ser considerada observavelmente pronta

### 4. A machine integra; ela não substitui o chipset

A `machine` deve existir como ponto de composição concreta da máquina:

* CPU
* Agnus
* Paula
* Denise
* CIA A
* CIA B
* wiring explícito
* init/reset
* publicação de IPL
* entrada unificada do protocolo de acesso

Ela não deve se tornar uma abstração genérica de scheduler.

### 5. DMA pertence ao Agnus

DMA não deve ficar na machine, nem num “DMA manager” genérico.

O ownership correto é:

* **Agnus**: arbitragem e avanço de DMA
* **Paula**: canais, eventos funcionais e interrupções associadas
* **Denise**: consumo dos efeitos do DMA de vídeo
* **CPU**: observação indireta de wait/busy/IRQ

### 6. Paula é dona de INTREQ/INTENA

A consolidação funcional das interrupções pertence a Paula.

CIA e Agnus produzem eventos.
Paula consolida e publica o efeito observável que chega como IPL à CPU.

---

## Estrutura conceitual esperada

A composição correta da máquina deve seguir esta ideia:

```c
BellatrixMachine
 ├── CPU (Emu68 / M68KState *)
 ├── CIA A
 ├── CIA B
 ├── Paula
 ├── Agnus
 │    └── Copper
 └── Denise
```

Com os seguintes papéis:

### BellatrixMachine

É o ponto de integração concreta.

Responsável por:

* composição dos componentes
* init/reset
* wiring explícito
* avanço conjunto quando necessário
* publicação de IPL
* protocolo de acesso central

Não é dona semântica do tempo.

### Agnus

É o centro temporal do domínio chipset.

Responsável por:

* beam/raster
* arbitragem de DMA
* copper
* blitter
* coordenação temporal do domínio visual
* disputa observável sobre recursos chip

### Paula

É dona de:

* `INTREQ`
* `INTENA`
* consolidação funcional de fontes de interrupção
* publicação do nível que será refletido em IPL

### CIA A / CIA B

São donas de seu estado próprio:

* timers
* TOD
* alarm
* ICR local
* portas
* eventos próprios

Não devem virar intermediárias informais de estado alheio.

### Denise

Mantém estado visual explícito e consome o estado produzido pelo domínio de vídeo do Agnus para compor a saída.

Não deve permanecer como singleton implícito.

### Copper

Pertence ao domínio do Agnus.

Não deve ser tratado como subsistema solto e paralelo à arbitragem central do chipset.

---

## Escopo da migração

A migração deve abranger todos os pontos que ainda refletem a arquitetura antiga.

No mínimo:

* `src/core/machine.h`
* `src/core/machine.c`
* `src/chipset/cia/cia.h`
* `src/chipset/cia/cia.c`
* `src/chipset/agnus/agnus.h`
* `src/chipset/agnus/agnus.c`
* `src/chipset/denise/denise.h`
* `src/chipset/denise/denise.c`
* `src/chipset/paula/paula.h`
* `src/chipset/paula/paula.c`
* `src/cpu/bellatrix.c`

Se houver código de Copper separado ou embutido de forma inadequada, ele também entra no escopo da migração.

---

## O que precisa ser feito

## 1. Fixar a arquitetura atual como canônica

A referência do projeto passa a ser esta:

* o chipset é dono do tempo observável
* a machine integra
* o barramento sincroniza
* a CPU consome o sistema, não o governa

Isso significa que todos os módulos devem convergir para esse modelo, e não o contrário.

---

## 2. Reescrever a machine como ponto de integração, não como scheduler genérico

`machine.h` e `machine.c` devem refletir a composição concreta da máquina.

A `BellatrixMachine` deve conter explicitamente:

* `struct M68KState *cpu`
* `CIA cia_a`
* `CIA cia_b`
* `Paula paula`
* `Agnus agnus`
* `Denise denise`

Além disso, deve oferecer:

* `bellatrix_machine_init()`
* `bellatrix_machine_reset()`
* `bellatrix_machine_read()`
* `bellatrix_machine_write()`
* `bellatrix_machine_sync_ipl()`
* `bellatrix_machine_advance()` ou equivalente

Esse avanço não deve ser descrito como “fonte do tempo”, mas como mecanismo concreto de sincronização entre componentes.

---

## 3. Tornar todos os chips explicitamente integráveis à machine

Todo chip relevante deve possuir estado explícito e integrável ao fluxo central da máquina.

Isso significa:

* abandonar estado global implícito quando ele mascara ownership
* abandonar singleton de arquivo quando ele impede wiring explícito
* permitir init/reset/step concretos
* permitir acesso claro aos registradores de que cada módulo é dono

O caso mais evidente continua sendo Denise, que precisa deixar de existir como singleton global estático.

---

## 4. Corrigir ownership funcional junto com a estrutura

A migração não é apenas nominal.

Ela precisa corrigir ownership real.

### Ownership correto

* `INTREQ/INTENA` → Paula
* DMA → Agnus
* Copper → domínio do Agnus
* estado visual e composição → Denise
* timers/TOD/alarm/ICR local → CIA
* CPU IPL observável → publicado pela integração a partir do estado dos chips

### Ownership incorreto que deve desaparecer

* Agnus como dono canônico de interrupções
* Denise como singleton global implícito
* `bellatrix.c` recalculando a arquitetura por fora da machine
* barramento tratado apenas como decode sem sincronização

---

## 5. Reestruturar o barramento como protocolo de sincronização

O barramento em Bellatrix deve deixar de ser só “rotear leitura e escrita”.

Ele precisa se tornar o ponto onde:

* a CPU emite a transação
* a machine a recebe
* o alvo é identificado
* o chipset é sincronizado quando necessário
* a transação só é considerada concluída quando puder responder de forma coerente

Isso é especialmente importante para:

* registradores custom
* CIA
* chip RAM observável
* polling do Kickstart
* busy/wait de blitter
* efeitos de DMA e IRQ

---

## 6. Colocar DMA explicitamente no domínio do Agnus

Agnus precisa concentrar a lógica de arbitragem de DMA.

Isso inclui, conforme o estágio atual do projeto:

* estado de `DMACON`
* critérios de elegibilidade de canais
* arbitragem temporal do domínio chip
* avanço de copper
* avanço de blitter
* fetch de bitplane
* publicação dos efeitos para Paula e Denise

Não criar um DMA manager genérico na machine.

A machine pode chamar `agnus_step()`, mas o DMA vive semanticamente no Agnus.

---

## 7. Reescrever `bellatrix.c` para consumir a arquitetura nova

`bellatrix.c` deve parar de ser a ponte informal que preserva o legado.

Ele precisa:

* usar `BellatrixMachine` como ponto de entrada real
* encaminhar acessos pelo protocolo novo
* deixar de reproduzir wiring por fora
* deixar de recalcular interrupções pelo caminho antigo
* deixar init/reset pertencerem à machine
* usar ownership correto na leitura/escrita de registradores

Em especial, deve sair desse arquivo qualquer resquício de:

* `agnus_intreq_set()` como caminho central de IRQ
* init duplicado de chips fora de `bellatrix_machine_init()`
* ponte paralela de IPL fora da machine

---

## 8. Tornar o IPL reflexo do estado real dos chips

A publicação do IPL para a CPU deve ser feita pela integração central a partir do estado real dos chips.

O fluxo esperado é:

* CIA e Agnus produzem eventos locais
* Paula consolida o estado funcional de interrupções
* a machine calcula/publica o IPL resultante para `M68KState`

Isso substitui o modelo antigo em que um glue informal em `bellatrix.c` recalculava tudo fora do ownership correto.

---

## 9. Eliminar definitivamente o modelo antigo como referência

Isso inclui eliminar como referência principal:

* `CIA_State`
* `AgnusState`
* Denise singleton implícito
* init/reset/step espalhados fora da machine
* ownership antigo de interrupção
* barramento como simples despacho sem semântica de sincronização

A migração deve ser estrutural, não cosmética.

---

## O que não fazer

* Não voltar o modelo para caber no codebase antigo.
* Não transformar a machine em scheduler abstrato soberano.
* Não tratar o Emu68 como fonte do tempo.
* Não criar um “DMA manager” neutro fora do Agnus.
* Não manter Agnus como dono canônico de `INTREQ/INTENA`.
* Não manter Denise como singleton implícito.
* Não deixar `bellatrix.c` continuar sendo ponte paralela de integração.
* Não usar typedef cosmético para fingir migração estrutural.
* Não tratar barramento como simples decode sem sincronização.
* Não considerar build quebrado como argumento para retroceder a arquitetura.

---

## Critério de aderência ao modelo atual

Um módulo só pode ser considerado migrado quando:

* usa o tipo estrutural novo esperado pela machine
* possui estado explícito coerente com seu papel
* respeita ownership funcional correto
* pode ser integrado sem singleton implícito
* não exige glue paralelo em `bellatrix.c`
* participa do barramento/protocolo de forma coerente
* não depende do modelo antigo para existir

---

## Interpretação correta dos erros atuais

Os erros de compilação devem ser lidos assim:

* não “o modelo novo está errado”
* e sim “o restante ainda não convergiu para ele”

Exemplos típicos:

* `unknown type name 'CIA'`, `Agnus`, `Denise`
  → headers ainda não convergiram para os tipos explícitos novos

* `cia_init` esperando `CIA_State *`
  → módulo CIA ainda preso ao modelo anterior

* `agnus_init` esperando `AgnusState *`
  → módulo Agnus ainda não migrou estruturalmente

* `denise_init(void)` e API global
  → Denise ainda no modelo singleton antigo

* `bellatrix.c` usando caminho antigo de IPL
  → integração principal ainda fora da machine

Esses erros não invalidam a direção. Eles mostram exatamente onde a migração ainda não foi concluída.

---

## Sequência prática de implementação

A ordem recomendada de execução é esta.

### Etapa 1 — Fixar machine nova e composição explícita

* consolidar `BellatrixMachine`
* consolidar init/reset/accessor
* consolidar read/write central
* consolidar sync de IPL

### Etapa 2 — Migrar Paula para ownership correto de interrupções

* `INTREQ`
* `INTENA`
* cálculo funcional das fontes
* publicação de IPL

### Etapa 3 — Migrar CIA A/B para estado explícito

* timers
* TOD
* alarm
* ICR
* ligação com Paula por evento, não por ownership indevido

### Etapa 4 — Migrar Agnus para centro temporal do chipset

* beam
* DMA
* copper
* blitter
* integração com Paula e Denise

### Etapa 5 — Migrar Denise para instância explícita

* remover singleton implícito
* explicitar estado visual
* integrar com Agnus

### Etapa 6 — Reescrever `bellatrix.c`

* consumir a machine nova
* remover glue legado
* rotear barramento de forma coerente
* remover ownership errado de IRQ

### Etapa 7 — Limpeza final do legado

* remover tipos antigos
* remover APIs antigas
* remover caminhos paralelos
* remover estruturas que preservam o modelo anterior

---

## Pedido objetivo de execução

Migrar o codebase para esse modelo, garantindo que:

1. o chipset seja tratado como fonte do tempo observável

2. a machine exista como integração concreta da máquina

3. o barramento seja tratado como protocolo de sincronização, não só decode

4. `CIA`, `Agnus`, `Paula` e `Denise` existam como tipos explícitos integráveis à machine

5. `Copper` permaneça subordinado ao domínio do Agnus

6. DMA pertença explicitamente ao Agnus

7. `INTREQ/INTENA` pertençam a Paula

8. Denise deixe de ser singleton implícito

9. `bellatrix.c` seja reescrito para consumir a arquitetura nova, e não preservar a antiga

10. os erros atuais de compilação sejam resolvidos por migração estrutural real, e não por recuo arquitetural

---

## Resultado esperado

Ao final da migração:

* o chipset será o centro semântico do comportamento observável
* a machine será o ponto claro de integração estrutural
* o barramento terá semântica arquitetural correta
* DMA ficará no lugar certo, dentro do Agnus
* interrupções terão ownership correto em Paula
* Denise participará explicitamente da máquina
* `bellatrix.c` deixará de carregar a arquitetura antiga
* o build voltará a compilar porque o restante finalmente convergiu para o modelo decidido


