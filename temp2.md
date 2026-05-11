Core 0 - Emu68 CPU (JIT)   ← boot core, fixed by Emu68
Core 1 - Agnus/GFX          ← DMA, copper, blitter, beam, Denise
Core 2 - Audio (Paula)      ← paula_audio_step, mixer (future)
Core 3 - IO (CIA + serial + disk)

Claro. Com sua divisão final:

```text
Core 0 — CPU/JIT + I/O moderno
Core 1 — Agnus + Denise + vídeo + DMA
Core 2 — Paula (áudio + INTREQ/INTENA + registradores Paula)
Core 3 — CIA-A/CIA-B + floppy + serial + RTC + portas
```

o sistema começa a ficar MUITO próximo de uma arquitetura realmente elegante para um Amiga moderno distribuído.

A chave é entender:

> cada core representa um domínio funcional e temporal do hardware.

---

# Visão geral

## Core 0 — Execução

Responsável por:

```text
68k
JIT
execução de instruções
acessos ao barramento
memória
Fast RAM
AutoConfig
I/O moderno
rede
filesystem moderno
serviços host
```

Esse core:

* executa código
* solicita acessos
* reage a IPL

Mas NÃO controla o tempo do chipset.

---

# Core 1 — Agnus + Denise

Esse é o coração temporal do sistema.

Responsável por:

```text
beam
raster
DMA arbitration
copper
blitter
bitplane fetch
sprites
display timing
scanout
framebuffer
```

Esse core:

* define o tempo observável do Amiga
* decide quem usa o barramento
* controla slots DMA
* sincroniza vídeo

Aqui vive o verdadeiro “clock” da máquina.

---

# Core 2 — Paula

Responsável por:

```text
INTREQ
INTENA
IPL
audio DMA
mixagem Paula
registradores Paula
SERDAT/SERDATR
DSKLEN/DSKBYTR
```

Mas:

```text
ele NÃO controla floppy físico
ele NÃO controla CIAs
ele NÃO arbitra DMA
```

Ele:

* consolida interrupções
* gerencia canais de áudio
* expõe registradores Paula
* conversa com Agnus para DMA
* consome dados vindos do Core 3

---

# Core 3 — Periféricos clássicos

Responsável por:

```text
CIA-A
CIA-B
timers
TOD
ICR
PRA/PRB
RTC
floppy drive
serial físico/lógico
teclado
mouse
joystick
```

Esse core representa:

* o “mundo externo clássico”
* os sinais físicos do Amiga

---

# O fluxo completo

Agora vem a parte importante.

---

# 1. CPU acessa registradores

Exemplo:

```text
MOVE.W #$8200,DMACON
```

Fluxo:

```text
Core 0:
CPU executa instrução
    ↓
bus write
    ↓
Core 1 (Agnus) recebe DMACON
    ↓
estado DMA atualizado
```

Outro exemplo:

```text
MOVE.W #$C020,INTENA
```

Fluxo:

```text
Core 0
    ↓
Core 2 (Paula)
    ↓
INTENA atualizado
```

Outro:

```text
MOVE.B $BFE001,D0
```

Fluxo:

```text
Core 0
    ↓
Core 3 (CIA-A)
    ↓
retorna PRA
```

---

# 2. Beam e vídeo

Core 1 roda continuamente:

```text
beam avança
    ↓
copper executa
    ↓
bitplanes fetch
    ↓
sprites fetch
    ↓
Denise renderiza
```

Core 0 não controla isso.

A CPU apenas:

* programa registradores
* sofre wait states
* observa o resultado

---

# 3. DMA

Exemplo: disk DMA.

---

## Core 3 — floppy

O drive gera:

```text
MFM bits
track data
disk ready
track0
disk change
```

---

## Core 2 — Paula

Paula recebe fluxo do floppy:

```text
MFM stream
    ↓
DSKBYTR
sync detect
disk shifter
DMA request
```

---

## Core 1 — Agnus

Agnus arbitra:

```text
Paula quer DMA
    ↓
Agnus concede slot
    ↓
transferência ocorre
```

---

## Core 0 — CPU

CPU vê:

```text
INTREQ
DSKBYTR
DMAON
```

---

# 4. Interrupções

Esse fluxo é MUITO importante.

---

## CIA gera interrupção

Exemplo:

```text
timer underflow
```

No Core 3:

```text
CIA ICR set
    ↓
IRQ CIAA/CIAB gerada
```

---

## Paula consolida

Core 3 envia evento:

```text
CIAA IRQ
```

para Core 2:

```text
INTREQ atualizado
    ↓
INTENA aplicado
    ↓
IPL calculado
```

---

## CPU recebe IPL

Core 2 publica:

```text
IPL=2
```

Core 0:

```text
CPU detecta IPL
    ↓
exception interrupt
```

---

# 5. Serial

## Core 3

Serial física:

```text
RX/TX
baud timing
UART host
null modem
```

---

## Core 2

Paula:

```text
SERDAT
SERDATR
TX empty
RX full
interrupts
```

---

## Core 0

CPU:

```text
lê/escreve registradores
```

---

# 6. Áudio

## Core 2

Paula:

```text
audio DMA
4 canais
mixagem
period
volume
sample fetch
```

---

## Core 1

Agnus:

```text
concede DMA slots
```

---

## Core 0

CPU:

```text
programa AUDx*
```

---

# 7. Input

## Core 0

Host moderno recebe:

```text
teclado USB
mouse
gamepad
```

---

## Core 3

Traduz para:

```text
CIA ports
keyboard protocol
mouse quadrature
joystick bits
```

---

# Comunicação ideal

Você NÃO quer:

```text
locks pesados
```

Você quer:

---

## 1. Snapshots atômicos

Para:

```text
beam position
IPL
PRA/PRB
disk status
```

---

## 2. Filas lock-free

Para:

```text
writes
IRQ events
DMA requests
serial events
```

---

## 3. Tempo lógico global

Todos os cores obedecem:

```text
mesmo timestamp lógico
```

Mesmo executando em paralelo.

---

# Filosofia final

## Core 1

Define:

```text
quando
```

---

## Core 0

Executa:

```text
o quê
```

---

## Core 2

Orquestra:

```text
interrupções e streams internos
```

---

## Core 3

Representa:

```text
o mundo externo clássico
```


