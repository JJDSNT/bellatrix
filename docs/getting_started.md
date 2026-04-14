# Bellatrix — Getting Started

## 🎯 Objetivo

Iniciar a implementação do Bellatrix a partir da Fase 0, estabelecendo a base de integração com o Emu68 e garantindo um ciclo básico de build, deploy e debug.

---

## 🧭 Por onde começar

O ponto de partida é trabalhar diretamente sobre o código do Emu68 e identificar os locais onde o Bellatrix será integrado.

Antes de escrever qualquer código novo, é necessário:

- entender a estrutura do Emu68
- localizar os pontos de interceptação de memória (fault handler)
- identificar como o acesso ao chipset é tratado atualmente (especialmente no modo PiStorm)

---

## 🧠 Estratégia de implementação

Os componentes do Bellatrix devem ser implementados seguindo um padrão único:

- a estrutura deve ser correta desde o início (interface, estado interno e organização)
- a profundidade da implementação não é fixa e pode variar conforme a sessão

---

### 🎯 Critério de decisão

O Claude Code pode escolher entre uma abordagem mais completa ou incremental com base em:

**Preferir implementação mais completa quando:**

- o comportamento é bem definido
- existe referência confiável
- o componente é relativamente isolado
- o risco de retrabalho é baixo

**Preferir abordagem iterativa quando:**

- o comportamento ainda depende de observação (via btrace)
- há muitos detalhes não essenciais no momento
- a implementação completa pode bloquear o progresso

---

### 🔁 Integração com observabilidade

Toda implementação deve ser guiada por dados observados:

- acessos ao barramento capturados via btrace
- análise posterior via ferramentas de análise
- evolução incremental baseada nesses dados

---

### 🧩 Regra prática

A forma deve ser correta desde o início.  
A implementação deve evoluir conforme necessário para permitir o avanço do sistema.

Evitar tanto:

- stubs descartáveis sem estrutura
- implementações completas prematuras que bloqueiem o progresso

## 🔍 Primeira etapa

Mapear os seguintes elementos no código do Emu68:

- tratamento de data abort / fault handler
- fluxo de acesso à memória não mapeada
- leitura do nível de interrupção (IPL)
- configuração de memória (MMU / maprom)
- definição de variantes de build (ex: pistorm)

Essa etapa é apenas de leitura e compreensão.

---

## 🔧 Segunda etapa

Criar a base de integração do Bellatrix:

- adicionar uma nova variant: `bellatrix`
- criar a estrutura de diretórios correspondente
- garantir que o build funcione sem alterar o comportamento atual

O objetivo aqui é apenas inserir o Bellatrix no sistema sem impacto funcional.

---

## 🔌 Terceira etapa

Inserir um ponto de interceptação no acesso ao barramento:

- criar uma função de entrada (ex: `bellatrix_bus_access`)
- conectar essa função ao fluxo de acesso à memória
- manter o comportamento original intacto

Essa etapa valida que o Bellatrix pode observar o sistema.

---

## 📡 Debug e observabilidade

A observabilidade é parte central da Fase 0.

O objetivo não é apenas imprimir logs, mas criar um fluxo onde o comportamento do sistema possa ser capturado e analisado.

---

### 🔹 btrace (bus trace)

O primeiro nível de observabilidade é o rastreamento de acessos ao barramento.

Durante o desenvolvimento, o sistema deve emitir registros de cada acesso relevante


Esses eventos são consumidos pela ferramenta **btrace**, que:

- captura a saída serial
- salva em formato estruturado (JSON Lines)
- permite inspeção posterior

---

### 🔹 Captura de execução

Fluxo esperado:

1. executar o sistema
2. capturar a saída via btrace
3. gerar um arquivo de log (ex: `boot.jsonl`)

Esse arquivo representa o comportamento real do sistema durante o boot.

---

### 🔹 analyze (análise de acesso)

Após capturar os logs, a ferramenta **analyze** deve ser usada para extrair informações úteis:

- quais endereços foram acessados
- quais regiões ainda não possuem implementação
- quais padrões se repetem

Exemplo de saída esperada:
Unimplemented accesses:

0xDFF096 (frequente)
0xBFE001 (CIA access)

Suggested next step:
→ implementar registradores básicos de CIA


---

### 🔹 Papel das ferramentas

As ferramentas de observabilidade não são auxiliares — elas guiam o desenvolvimento.

O fluxo correto é:

```text
executar → capturar (btrace) → analisar (analyze) → implementar → repetir

🔁 Ciclo de desenvolvimento

O fluxo esperado durante o desenvolvimento é:

build do projeto
deploy do artefato para o Raspberry Pi (ex: via TFTP)
boot do sistema
captura da execução via btrace
análise com analyze
decisão do próximo passo
⚠️ Escopo da Fase 0

Nesta fase, não deve ser feito:

implementação do chipset
emulação de dispositivos completos
alterações profundas no Emu68

O foco é apenas:

integração
interceptação
observabilidade estruturada
🚀 Próximo passo

Após essa base estar funcional:

iniciar implementação incremental dos componentes do chipset
usar os dados coletados por btrace/analyze como guia
🧩 Resumo

O objetivo do Getting Started é:

entender o Emu68
inserir o Bellatrix sem impacto
provar que o barramento pode ser interceptado
estabelecer um sistema observável

A partir desse ponto, o desenvolvimento deixa de ser baseado em tentativa e erro e passa a ser guiado por dados.
