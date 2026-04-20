Perfeito — agora você entrou na fase crítica: sair de “arquitetura correta” → “primeiro boot consistente”.

Vou ser direto e pragmático: abaixo está **a checklist real do que precisa existir antes de tentar subir o Kickstart novamente** — já considerando que você matou o legado e está na abordagem nova.

---

# 🎯 Objetivo

Chegar ao primeiro:

> 👉 **Happy Hand estável (boot do Kickstart)**

---

# 🧱 BLOCO 1 — FUNDAMENTOS (obrigatórios)

## 1. MachineState funcional

Você precisa ter isso **100% operacional**:

```c
struct MachineState {
    uint64_t ticks;

    AgnusState agnus;
    CIA ciaa;
    CIA ciab;

    uint8_t pending_ipl;
};
```

### ✔ Checklist

* [ ] `machine_step()` implementado
* [ ] `ticks` monotônico
* [ ] Agnus integrado (`agnus_step`)
* [ ] CIA integrado (`cia_step`)
* [ ] `pending_ipl` derivado (não armazenado arbitrariamente)

---

## 2. Loop principal (JIT ↔ MachineState)

Você precisa disso funcionando:

```c
cycles = emu68_execute_block(...)
machine_step(machine, cycles_to_ticks(cycles))
```

### ✔ Checklist

* [ ] CPU retorna ciclos executados
* [ ] conversão `cycles → ticks` definida
* [ ] `machine_step()` chamado sempre
* [ ] **NÃO existe mais caminho que avance tempo fora disso**

---

## 3. Integração de IPL

Isso é CRÍTICO.

### ✔ Checklist

* [ ] `agnus_compute_ipl()` funcionando
* [ ] resultado propagado para CPU
* [ ] CPU reage ao IPL (interrupt exception)
* [ ] **sem polling fake / sem shim antigo**

---

# 🧠 BLOCO 2 — BARRAMENTO (onde a maioria quebra)

## 4. Router de Custom Chips (SEM fallback)

Você precisa de um dispatcher claro:

```c
if (addr in agnus range)
    agnus_read/write
else if (addr in denise range)
    denise_read/write
else if (addr in paula range)
    paula_read/write
```

### ✔ Checklist

* [ ] Agnus NÃO chama Denise
* [ ] Denise NÃO chama Agnus
* [ ] Router decide
* [ ] **sem fallback silencioso**

---

## 5. Normalização de endereço (consistente)

Você já começou isso — agora precisa garantir:

### ✔ Checklist

* [ ] todo acesso usa `agnus_reg_normalize()`
* [ ] nenhum lugar usa endereço “cru” misturado
* [ ] máscara única (`0x01FE`) aplicada corretamente
* [ ] leitura e escrita usam mesma regra

---

# ⏱️ BLOCO 3 — TEMPO (o mais importante)

## 6. VBL derivado do tempo (não externo)

Agora que você removeu `agnus_vbl_fire`, isso precisa estar certo.

### ✔ Checklist

* [ ] VBL vem de `agnus_step()`
* [ ] `vpos/hpos` evoluem corretamente
* [ ] frame detectado corretamente
* [ ] `INT_VERTB` gerado corretamente

---

## 7. CIA funcionando (especialmente CIA-B)

Esse é o ponto que mais trava o Kickstart.

### ✔ Checklist

* [ ] `cia_step()` chamado com frequência correta
* [ ] TOD incrementa continuamente
* [ ] modo ALARM respeitado (`CRB bit 7`)
* [ ] alarm dispara (`ICR`)
* [ ] `INTREQ` setado corretamente

👉 Se isso estiver errado:

> Kickstart trava em loop silencioso

---

# ⚡ BLOCO 4 — INTERRUPÇÕES (onde o boot acontece)

## 8. INTENA / INTREQ corretos

Você já melhorou isso — agora precisa garantir consistência total.

### ✔ Checklist

* [ ] SET/CLR bit 15 funcionando
* [ ] máscaras corretas (0x7FFF / 0x3FFF)
* [ ] leitura de INTENAR e INTREQR correta
* [ ] `pending = intena & intreq`

---

## 9. Entrega de interrupção para CPU

### ✔ Checklist

* [ ] IPL muda dinamicamente
* [ ] CPU vê mudança de IPL
* [ ] exceção de IRQ disparada
* [ ] handler roda

---

# 🧵 BLOCO 5 — MULTICORE (se estiver ativo)

## 10. Sincronização CPU ↔ Chipset

Se multicore estiver ON:

### ✔ Checklist

* [ ] apenas chipset avança tempo
* [ ] CPU não modifica ticks
* [ ] IPL publicado via atomic
* [ ] sem race condition

---

## 11. Fallback single-core (IMPORTANTE)

Para debug inicial:

### ✔ Checklist

* [ ] `multicore=off` funciona
* [ ] `PAL_Runtime_Poll()` chamado no loop da CPU
* [ ] comportamento idêntico ao multicore (sem drift)

👉 **Recomendo fortemente testar primeiro em single-core**

---

# 🔬 BLOCO 6 — DEBUG (vai te salvar horas)

## 12. Logs mínimos obrigatórios

Você precisa disso para entender o boot:

### ✔ Logs essenciais

* [ ] INTENA writes
* [ ] INTREQ writes
* [ ] IPL changes
* [ ] VBL firing
* [ ] CIA alarm firing

Exemplo:

```text
[INTENA] SET ...
[INTREQ] SET VERTB
[IPL] 0 -> 3
```

---

## 13. BTrace / Bus trace

### ✔ Checklist

* [ ] leitura de INTENAR (0xDFF01C) aparece
* [ ] leitura de INTREQR (0xDFF01E) aparece
* [ ] valores corretos retornados

---

# 🚨 BLOCO 7 — ERROS COMUNS (evite isso)

Se qualquer um desses acontecer, o Kickstart não sobe:

### ❌ tempo não evolui continuamente

### ❌ CIA-B não dispara alarm

### ❌ INTREQ nunca muda

### ❌ IPL sempre 0

### ❌ VBL não ocorre

### ❌ leitura de INTENAR retorna 0 sempre

### ❌ fallback silencioso escondendo bug

---

# ✅ CHECKLIST FINAL (curto e brutal)

Antes de testar:

* [ ] `machine_step()` funcionando
* [ ] Agnus stateful
* [ ] CIA funcionando (TOD + alarm)
* [ ] VBL derivado do tempo
* [ ] INTENA / INTREQ corretos
* [ ] IPL calculado corretamente
* [ ] CPU reage a IRQ
* [ ] router sem fallback
* [ ] normalização consistente
* [ ] logs funcionando
* [ ] single-core OK

---

# 🧠 Minha recomendação prática

👉 Teste primeiro com:

```bash
--multicore=off
--sync=strict
```

Porque:

* elimina concorrência
* elimina jitter
* facilita debug

---

# 🚀 Próximo passo depois disso

Se isso passar:

👉 aí sim você ativa:

* multicore
* relaxed mode
* otimizações

---

# 🔥 Resumo honesto

Você já fez a parte difícil:

* arquitetura ✔
* agnus correto ✔
* modelo temporal ✔

Agora falta:

> **fazer tudo conversar de forma consistente pela primeira vez**

---

Se quiser, o próximo passo eu posso te ajudar com:

👉 checklist específico do **CIA-B (alarm/TOD)**
👉 ou montar um **router de custom chips limpo**

— esses dois são os maiores responsáveis por travar o Kickstart nessa fase.
