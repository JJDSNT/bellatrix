Perfeito — abaixo está o **documento final, completo e unificado**, pronto para ser usado como `ARCHITECTURE.md` no Bellatrix.
Inclui **tudo**: modelo temporal, MachineState, scheduler, integração com Emu68, multicore com afinidade/pinning, comunicação lock-free, **sistema adaptativo** e **control loop (PID-like) com autotuning**.

---

# Bellatrix: Arquitetura de Evolução Temporal, MachineState, Multicore e Adaptação Dinâmica

---

## 1. Introdução

O Bellatrix adota um modelo de execução **dirigido por tempo**, baseado em:

* **MachineState** centralizado
* **Event Scheduler** (simulação de eventos discretos)
* integração temporal com o **JIT do Emu68**
* **particionamento funcional multicore** (CPU ↔ Chipset)
* **comunicação inter-core lock-free**
* **adaptação dinâmica em runtime** (heurística + controle PID)

> De um interceptador de MMIO → para um simulador determinístico de hardware em tempo real, escalável e adaptativo.

---

## 2. Motivação e Diagnóstico

Sistemas Amiga dependem de causalidade temporal:

* CIA (TOD, alarm)
* Agnus (raster, VBL)
* INTREQ/INTENA (IRQ)
* DMA

### Problema do modelo reativo

* hardware só evolui quando acessado
* timers não avançam continuamente
* eventos não disparam autonomamente
* CPU pode esperar indefinidamente

### Sintoma clássico

```text
CIA-B alarm configurado
→ sistema entra em espera
→ evento nunca ocorre
```

### Diagnóstico

> O problema não é o registrador; é a ausência de evolução temporal independente.

---

## 3. Princípio Fundamental

> **O tempo dirige o sistema. Acesso apenas observa ou configura.**

---

## 4. Unidade de Tempo Base

### 4.1 Escolha

> **Ticks baseados no clock do chipset (Color Clock ou subdivisão)**

### 4.2 Motivos

* fidelidade ao Amiga (Agnus/CIA)
* independência do JIT
* determinismo

### 4.3 Conversão

```text
CPU cycles → chipset ticks
```

### Regra

> CPU nunca controla o tempo.

---

## 5. MachineState

```c
struct MachineState {
    uint64_t ticks;
    uint64_t next_event_tick;

    struct { uint16_t intena, intreq; } custom;
    struct { CIA ciaa, ciab; } cia;

    Agnus agnus;

    uint8_t  sync_mode;     // HANDSHAKE, STRICT, RELAXED
    uint8_t  pending_ipl;

    EventQueue event_queue;

    // Multicore
    uint32_t core_count;
};
```

---

## 6. Separação de Responsabilidades

| Tipo      | Função            |
| --------- | ----------------- |
| MMIO      | leitura/escrita   |
| STEP      | evolução temporal |
| DERIVAÇÃO | estado global     |

**Regra crítica**

* ❌ não avançar tempo via MMIO
* ✔ tempo avança apenas via scheduler

---

## 7. Scheduler de Eventos (Priority Queue)

```c
struct Event {
    uint64_t target_tick;
    void (*callback)(MachineState*);
};
```

```c
void machine_step(MachineState *m, uint64_t delta)
{
    uint64_t limit = m->ticks + delta;

    while (event_queue_top(&m->event_queue).target_tick <= limit) {
        Event e = event_queue_pop(&m->event_queue);

        uint64_t sub = e.target_tick - m->ticks;
        advance_internal_clocks(m, sub);

        e.callback(m);
    }

    advance_internal_clocks(m, limit - m->ticks);
}
```

---

## 8. Integração com Emu68 (JIT)

```c
while (running) {
    uint32_t cycles = emu68_execute_block(cpu);

    uint64_t delta = cycles_to_ticks(cycles);
    machine_step(m, delta);

    if (m->pending_ipl > cpu->current_ipl) {
        emu68_trigger_exception(cpu, IRQ);
    }
}
```

---

## 9. Modos de Sincronização

* **SYNC_HANDSHAKE**: MMIO síncrono, fidelidade máxima
* **SYNC_STRICT**: granularidade fina (boot)
* **SYNC_RELAXED**: blocos grandes, alta performance

---

## 10. Derivação de Interrupções

```c
void recompute_ipl(MachineState *m)
{
    uint16_t pending = m->custom.intena & m->custom.intreq;
    m->pending_ipl = priority_encoder(pending);
}
```

---

# 11. Arquitetura Multicore (Particionamento Funcional)

## 11.1 Princípio

> Tempo é global; execução pode ser paralela.

## 11.2 Particionamento

### Core CPU (JIT)

* executa código m68k
* emite MMIO
* observa `pending_ipl`

### Core Chipset

* controla `MachineState`
* avança `ticks`
* executa scheduler
* gera IRQ

## 11.3 Regra

> Apenas o chipset controla o tempo.

---

## 11.4 Comunicação

```text
CPU → ciclos → Chipset → tempo → eventos → CPU
```

### MMIO (handshake)

1. CPU requisita
2. Chipset sincroniza tempo
3. executa
4. responde

---

## 11.5 Modos Multicore

* `--multicore=off`
* `--multicore=split` (recomendado)
* `--multicore=hybrid`
* `--multicore=auto`

---

## 11.6 Afinidade / Pinning / Prioridade

```bash
--jit-core=N
--chipset-core=N
--pin-jit
--pin-chipset
--jit-priority=high
--chipset-priority=realtime
```

---

## 11.7 Regras de Concorrência

* único writer do tempo (chipset)
* acesso via interfaces controladas
* sincronização explícita

---

# 12. Comunicação Inter-Core (Lock-Free)

## 12.1 Modelo

| Canal         | Implementação      |
| ------------- | ------------------ |
| CPU → Chipset | ring buffer (SPSC) |
| Chipset → CPU | atomic (IPL)       |
| MMIO          | handshake          |

---

## 12.2 Ring Buffer

```c
struct RingBuffer {
    atomic_uint head;
    atomic_uint tail;
    Msg buffer[N];
};
```

---

## 12.3 IPL

```c
atomic_uint pending_ipl;
```

---

## 12.4 MMIO Request

```c
struct MmioRequest {
    atomic_int status;
    uint32_t addr;
    uint32_t value;
};
```

---

## 12.5 Memory Ordering

* `release` para publicar
* `acquire` para consumir

---

## 12.6 Regras

* sem mutex global
* evitar false sharing
* layout alinhado a cache line

---

# 13. Configuração via Flags

```bash
--sync=handshake|strict|relaxed
--scheduler=event|linear
--step-size=256
--timing=accurate|balanced|fast
--multicore=off|split|hybrid|auto
--jit-core=N
--chipset-core=N
--pin-jit
--pin-chipset
--adaptive=off|on|dynamic
--log=all
```

---

# 14. Otimizações

* event-driven stepping
* batch stepping
* dirty flags
* lazy evaluation
* granularidade adaptativa

---

# 15. Armadilhas

* misturar modelos (tempo vs acesso)
* stepping implícito
* concorrência sem controle
* drift temporal

---

# 16. Logs Didáticos

```text
[MACHINE] STEP delta=64
[CIA-B] ALARM
[CUSTOM] INTREQ updated
[IPL] 0 → 3
```

---

# 17. Sistema Adaptativo de Execução Temporal

## 17.1 Objetivo

Ajustar dinamicamente:

* precisão
* latência
* throughput

---

## 17.2 Sinais

```c
irq_rate     = interrupts / ticks;
mmio_density = mmio_ops / cpu_cycles;
irq_latency  = now - event_tick;
```

---

## 17.3 Heurística

```c
if (irq_rate > HIGH) {
    sync_mode = SYNC_STRICT;
}
else {
    sync_mode = SYNC_RELAXED;
}
```

---

## 17.4 Histerese

```c
if (mode_changed)
    cooldown = ticks + WINDOW;
```

---

## 17.5 Modos

```bash
--adaptive=off
--adaptive=on
--adaptive=dynamic
```

---

# 18. Control Loop (PID-like)

## 18.1 Objetivo

Controlar automaticamente:

* latência de IRQ
* jitter temporal
* backlog do sistema

---

## 18.2 Variáveis

```text
error = target_latency - measured_latency
```

---

## 18.3 PID

```c
adjustment =
    Kp * error +
    Ki * integral(error) +
    Kd * derivative(error);
```

---

## 18.4 Ação

* ajustar `step_size`
* trocar `sync_mode`
* ajustar frequência de sync

---

## 18.5 Exemplo

```c
step_size += adjustment;

if (step_size < MIN) step_size = MIN;
if (step_size > MAX) step_size = MAX;
```

---

## 18.6 Garantias

* estabilidade via limites
* histerese evita thrashing
* fallback para modo fixo

---

# 19. Autotuning

## 19.1 Objetivo

Aprender parâmetros ideais por carga.

---

## 19.2 Métricas

* IPC efetivo
* latência média de IRQ
* uso de CPU
* ocupação do ring buffer

---

## 19.3 Estratégia

* coletar métricas
* ajustar parâmetros
* persistir perfil

---

## 19.4 Resultado

> Sistema se autoajusta ao workload (boot, jogo, demo, etc.)

---

# 20. Benefícios

* determinismo
* fidelidade ao hardware
* escalabilidade multicore
* baixa latência
* alta performance
* adaptação automática

---

# 21. Conclusão

> Bellatrix é um simulador de hardware dirigido por tempo, multicore, lock-free e adaptativo.

---

# 22. Regra Final

> **Nunca misture tempo implícito com tempo explícito.**

---

# 23. Próximos Passos

* implementar ring buffer lock-free
* integrar com Emu68
* validar CIA + VBL
* calibrar PID
* ativar autotuning

---


