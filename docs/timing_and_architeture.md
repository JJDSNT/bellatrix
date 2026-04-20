# Bellatrix — Arquitetura de Execução e Evolução Temporal

---

## 1. Princípio Fundamental

> **O tempo observável do sistema é determinado pelo chipset.**

* A CPU não controla o tempo
* MMIO não avança o tempo
* O tempo não nasce de ciclos do JIT
* O tempo emerge da evolução dos componentes de hardware

---

## 2. Papel da Máquina

A máquina não é o “motor do tempo”.

> **A máquina é o ponto de composição e orquestração explícita dos componentes.**

```c
BellatrixMachine
 ├── CPU (Emu68)
 ├── CIA A
 ├── CIA B
 ├── Paula
 ├── Agnus
 │    └── Copper
 └── Denise
```

Responsabilidades:

* inicialização
* reset
* wiring explícito entre módulos
* coordenação da evolução
* publicação do estado global (ex: IPL)

---

## 3. Domínio Temporal

### 3.1 Dono do tempo

O domínio temporal é distribuído, com responsabilidades claras:

| Componente | Responsabilidade temporal |
| ---------- | ------------------------- |
| CIA        | timers, TOD, alarm        |
| Agnus      | raster, DMA, VBL          |
| Paula      | consolidação de IRQ       |
| Denise     | composição visual         |

---

### 3.2 Regra crítica

> **Tempo só avança dentro dos componentes.**

* CIA avança timers
* Agnus avança beam e DMA
* Paula reage a eventos
* Denise consome estado

A máquina apenas **coordena a propagação**, não simula tempo por conta própria.

---

## 4. Separação de Responsabilidades

| Domínio   | Função                             |
| --------- | ---------------------------------- |
| MMIO      | leitura/escrita de registradores   |
| STEP      | evolução temporal interna          |
| DERIVAÇÃO | cálculo de estado global (ex: IPL) |

### Regra

* ❌ MMIO não avança tempo
* ✔ STEP avança tempo
* ✔ DERIVAÇÃO não altera estado estrutural

---

## 5. Evolução Temporal (Step)

A evolução ocorre por propagação coordenada:

```c
void bellatrix_machine_step(BellatrixMachine *m, uint64_t ticks)
{
    m->ticks += ticks;

    cia_step(&m->cia_a, ticks);
    cia_step(&m->cia_b, ticks);

    agnus_step(&m->agnus, ticks);

    paula_step(&m->paula, ticks);

    machine_recompute_ipl(m);

    denise_step(&m->denise, &m->agnus, ticks);
}
```

> **Importante:**
> A máquina não gera tempo — apenas propaga o avanço para os componentes.

---

## 6. Fluxo de Evolução Temporal

```text
ticks
  ↓
CIA / Agnus evoluem
  ↓
eventos são gerados
  ↓
Paula consolida (INTREQ)
  ↓
IPL é derivado
  ↓
CPU reage
```

---

## 7. Interrupções (Ownership correto)

### 7.1 Princípio

> **INTREQ / INTENA pertencem a Paula**

### 7.2 Fluxo

```text
CIA / Agnus
     ↓
  eventos
     ↓
   Paula (INTREQ)
     ↓
  prioridade
     ↓
    IPL
     ↓
   CPU
```

### 7.3 Regras

* CIA não escreve IPL diretamente
* Agnus não é dono de interrupções
* Paula é o único consolidado de IRQ

---

## 8. Integração com Emu68

### 8.1 Princípio

> **CPU executa. Chipset determina o mundo.**

### 8.2 Loop conceitual

```c
while (running) {
    emu68_execute(cpu);

    bellatrix_machine_step(machine, delta);

    if (machine->ipl > cpu->ipl) {
        emu68_trigger_irq(cpu);
    }
}
```

### 8.3 Regras

* `delta` não define o tempo do sistema
* `delta` apenas define quanto avançar nesta iteração
* o comportamento emerge da evolução do chipset

> **Importante:**
> `delta` deve respeitar a granularidade temporal relevante do hardware
> (ex: eventos de raster, timers da CIA, etc.)

---

## 9. DMA como parte do tempo

> **DMA não é efeito colateral. DMA é parte da evolução temporal.**

* Agnus controla DMA
* DMA compete implicitamente com CPU
* o timing depende do estado interno do chipset

### Regra

* DMA deve ser modelado dentro de `agnus_step()`
* nunca como operação externa isolada

---

## 10. Copper

> **Copper pertence ao domínio do Agnus**

* executa sincronizado com o beam
* modifica registradores durante o frame
* não existe como entidade independente

---

## 11. Denise

> **Denise é consumidor do estado visual**

* não gera tempo
* não gera interrupções
* não controla DMA

Responsabilidades:

* bitplanes
* sprites
* composição final

---

## 12. Modelo de Sincronização

### Modos possíveis

| Modo      | Característica      |
| --------- | ------------------- |
| STRICT    | alta fidelidade     |
| RELAXED   | performance         |
| HANDSHAKE | sincronização forte |

### Regra

> Modos afetam granularidade de execução, não ownership de estado.

---

## 13. Multicore (Direção)

### Princípio

> Execução pode ser paralela. Tempo permanece único.

### Particionamento natural

| Core         | Função            |
| ------------ | ----------------- |
| CPU core     | Emu68             |
| Chipset core | evolução temporal |

### Comunicação

* CPU → Chipset: requisições
* Chipset → CPU: IPL

---

## 14. Comunicação

### 14.1 MMIO

* síncrono
* exige estado consistente

### 14.2 Interrupções

* assíncronas
* baseadas em estado consolidado

---

## 15. Regras de Arquitetura

### 15.1 Nunca fazer

* ❌ avançar tempo em MMIO
* ❌ centralizar estado em “custom” genérico
* ❌ deixar Agnus como dono de interrupções
* ❌ usar Denise como singleton implícito
* ❌ duplicar lógica de integração fora da máquina

---

### 15.2 Sempre fazer

* ✔ estado explícito por componente
* ✔ wiring via máquina
* ✔ ownership claro
* ✔ evolução temporal dentro dos chips
* ✔ Paula como consolidado de IRQ

---

## 16. Critério de Correção

Um sistema está correto quando:

* CIA gera eventos temporais reais
* Agnus gera VBL corretamente
* Paula consolida interrupções corretamente
* CPU reage ao IPL corretamente
* Denise reflete estado visual coerente

---

## 17. Resultado Esperado

* boot consistente (Happy Hand)
* timers funcionais
* interrupções corretas
* ausência de loops de espera travados
* sistema independente de polling artificial


Perfeito — isso é **crítico** e precisa mesmo estar explícito no documento.
Vou te entregar a seção pronta, já no mesmo padrão do seu doc, para você só encaixar.

---

## 18. Fonte de Tempo e Granularidade

### 18.1 Fonte de tempo

> **O Bellatrix consome tempo do host.**

A única fonte real de tempo é o hardware do host (ARM):

* contador físico (`CNTPCT_EL0`)
* timers do sistema

Esse tempo é convertido em uma unidade interna (`ticks`) e propagado para o chipset.

```c
delta = host_time_now() - last_time;
bellatrix_machine_step(machine, delta);
```

---

### 18.2 Princípio

> **O host fornece a passagem do tempo.
> O chipset define o significado desse tempo.**

* o host não conhece raster, DMA ou timers da CIA
* o chipset interpreta os ticks e produz comportamento

---

### 18.3 Granularidade temporal

A granularidade define **quanto tempo cada tick representa**.

> **A granularidade deve ser compatível com os eventos mínimos do hardware.**

Eventos relevantes:

* raster (Agnus)
* VBL (vertical blank)
* timers da CIA (incluindo TOD/alarm)
* arbitragem de DMA

---

### 18.4 Regra fundamental

> **O step não pode “pular” eventos do hardware.**

Isso implica:

* `delta` não pode ser arbitrariamente grande
* eventos intermediários precisam ser observáveis

---

### 18.5 Estratégias de granularidade

#### Granularidade fina (alta fidelidade)

* baseada em ciclos do chipset (ex: color clock ou subdivisão)
* garante precisão total
* maior custo computacional

#### Granularidade média (balanceada)

* múltiplos de ciclos relevantes (ex: HSync ou blocos menores)
* mantém coerência de eventos
* bom custo-benefício

#### Granularidade grossa (baixa fidelidade)

* grandes blocos de tempo
* pode perder eventos intermediários
* risco de travamentos (ex: CIA alarm não dispara)

---

### 18.6 Recomendação prática

> **A granularidade deve ser suficiente para preservar:**

* disparo correto de timers da CIA
* geração consistente de VBL
* ordem causal entre eventos

---

### 18.7 Relação entre `delta` e granularidade

`delta` representa o tempo decorrido no host, mas:

> **deve ser decomposto conforme a granularidade do sistema.**

Exemplo conceitual:

```c
while (delta > 0) {
    uint64_t step = min(delta, GRANULARITY);
    bellatrix_machine_step(machine, step);
    delta -= step;
}
```

---

### 18.8 Consequências de granularidade incorreta

Granularidade inadequada leva a:

* timers da CIA não disparando
* VBL inconsistente
* interrupções fora de ordem
* loops de espera no Kickstart

---

### 18.9 Regra final

> **O tempo pode vir do host,
> mas a granularidade deve ser definida pelo hardware que está sendo simulado.**

---

## 19. Regra Final

> **Bellatrix não simula registradores.
> Bellatrix simula comportamento emergente de hardware.**

---

## 20. Modelo de Acesso ao Hardware (Protocolo)

### 20.1 Princípio

> **A CPU não acessa o hardware diretamente.
> Toda interação ocorre através de um protocolo de acesso.**

Esse protocolo é responsável por:

* arbitrar acesso ao barramento
* sincronizar com o estado do chipset
* garantir consistência temporal
* fornecer resposta à CPU

---

### 20.2 Fluxo de acesso

```text
CPU
 ↓
requisição (read/write)
 ↓
protocolo de acesso
 ↓
chipset (CIA / Agnus / Paula / Denise)
 ↓
resposta
 ↓
CPU
```

---

### 20.3 Responsabilidades do protocolo

O protocolo deve garantir:

* ordenação correta dos acessos
* visibilidade consistente do estado
* sincronização com a evolução temporal
* bloqueio quando necessário (ex: DMA ativo)

---

### 20.4 Relação com o tempo

> **A latência de acesso faz parte do comportamento do hardware.**

Isso implica:

* acessos não são instantâneos
* o resultado pode depender do estado interno do chipset
* o tempo de resposta pode variar

---

### 20.5 Inspiração

Este modelo é inspirado em arquiteturas como o protocolo utilizado no projeto PiStorm protocol, onde:

* a CPU emite requisições
* o hardware arbitra o acesso
* a resposta depende do estado do sistema

No Bellatrix, essa abordagem é reproduzida em software, mantendo:

* coerência temporal
* fidelidade ao comportamento do hardware
* separação clara entre CPU e chipset

---

### 20.6 Regra fundamental

> **Nenhum acesso da CPU pode ignorar o estado do chipset.**




