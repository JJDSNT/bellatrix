# Issue: Contrato de interrupção Emu68 (PiStorm-cêntrico) vs Bellatrix multicore

> Decisão 2026-07-11: IRQs físicas e I/O pertencem ao Core 0 supervisor;
> Core 1 observa apenas IPL 68k emulado, Core 2 possui Rigel e Core 3 fica
> reservado. IRQ ARM continua sem passar pelos vectors PiStorm/INT6.

## Status: decisão de arquitetura registrada (2026-07-10)

Documenta por que o Bellatrix **nunca** roteia interrupções ARM de periférico
(USB, HDMI audio, Bluetooth) para dentro do Emu68, e qual é a fronteira portátil
correta de longo prazo. Consolida a investigação que substituiu uma leva de
rascunhos externos (`Bellatrix_Emu68_Multicore_Agent_Plan.md`, `instrucoes.md`,
`consideracoes.md`, `multicore_temp*.md`, todos apagados) cujo plano partia de
premissas que a branch já superou.

Relacionado: `[[emu68_public_api]]`, `[[issue_multicore_runtime]]`,
`[[issue_interrupt_pipeline]]`, `[[issue_paula_audio_timing]]`.

## O modelo de interrupção nativo do Emu68 é PiStorm-cêntrico

A vector table de exceção do Emu68 (`emu68/src/aarch64/vectors.c`) trata IRQ/FIQ
de forma **hardcoded** para o topologia PiStorm, onde o Amiga é hardware real e o
Pi é apenas a CPU. `curr_el_spx_irq` (vectors.c:140-157) e `curr_el_spx_fiq`
(vectors.c:160-177) fazem exatamente uma coisa:

```
IRQ/FIQ ARM chega
  → lê INTENA shadow (INT_shadow)
  → se INTEN(bit14)+EXTER(bit13) ativos:
       escreve nível 6 no campo `pint` do contexto M68K (via TPIDRRO_EL0)
  → mascara IRQ/FIQ no SPSR_EL1 pra não ser reperturbado
  → eret
```

Literalmente `mov w0, #6`. SError → nível 7 (vectors.c:180-189). Ou seja: a única
função da vector de interrupção do Emu68 é traduzir **uma** linha física — o
INT6/EXTER do Amiga real — no IPL do 68K. `ARM IRQ ≡ Amiga IPL`. Não há noção de
"periférico do host", porque no PiStorm o Pi não tem periféricos: USB, teclado,
disco, áudio são todos hardware Amiga real.

**Portabilidade entre plataformas:** esse código é idêntico para qualquer board
AArch64 (raspi64, etc.; há uma árvore AArch32 paralela). O que muda entre boards é
só *onde* a linha do Amiga está fisicamente ligada. O Emu68 **não** é portável
entre *modelos* de interrupção — é portável entre boards que apresentam o **mesmo**
modelo PiStorm (uma IRQ externa do Amiga). Ele não tem, e nunca precisou ter,
conceito de GIC com afinidade por periférico.

## Como o Bellatrix já inverteu / bypassou esse modelo

O Bellatrix é o oposto do PiStorm: o Pi **é** a máquina inteira (Amiga emulado via
Rigel, periféricos reais do Pi). Isso quebra as duas pontas do modelo do Emu68, e
a branch já contornou ambas:

1. **IPL não vem de fio físico.** O Rigel (Core 2) calcula `active = INTREQ & INTENA`
   e chama `bellatrix_machine_on_ipl_changed(ipl)`, que injeta o IPL no contexto
   68K por software (`PAL_IPL_Set` → `M68KState.INT.IPL`, ver CLAUDE.md e
   `[[issue_interrupt_pipeline]]`). O caminho `curr_el_spx_irq → mov #6` está
   **morto** no Bellatrix: nada assere aquela linha, porque não há Amiga físico.
   A vector de interrupção do Emu68 é código dormente aqui.

2. **Periféricos do host são polled no Core 3.** `src/runtime/core_io.c:63`:
   `bt_host_step()` + `usb_host_step()` em loop de polling ("só faz polling de
   hardware físico"). Ver `[[issue_multicore_runtime]]` (Core 3 = IO).

3. **O JIT nem roda no Core 0.** Core 0 estaciona em `wfe` após o boot; o JIT roda
   no Core 1. A premissa "Emu68 é dono do Core 0 e das IRQs físicas" não vale na
   branch atual.

## Decisão deliberada: device IRQ ARM fica FORA do Emu68

**Nunca** roteamos IRQ ARM de USB/HDMI/Bluetooth para a vector table do Emu68. Isso
é intencional, não acidental:

- O handler do Emu68 hardcoda `#6` — uma IRQ de device seria interpretada como
  Amiga nível 6 e corromperia o IPL emulado.
- O handler se auto-mascara no retorno; e o hot loop do JIT pina os registradores
  M68K em x13–x29 (ver CLAUDE.md, "ABI constraint"). Tomar IRQ de device no core do
  JIT perturbaria esse ABI.
- `src/io/bluetooth/bt_hal_raspi3.c:44-51` documenta o regime real: *"sev-free bare
  metal with no routed IRQs"*, com BTStack dirigido por polling; a nota registra que
  builds antigos só sobreviviam ao WFE porque "stray pending interrupts (USB)"
  acordavam o core — ou seja, IRQ de device hoje é incidental e não-roteada, não
  projetada.

Polling no Core 3 não foi só pragmatismo inicial: foi o caminho que **evita a
colisão** entre os dois significados incompatíveis de "IRQ ARM" (Amiga INT6 vs
device do host precisa de serviço).

## A fronteira portátil correta (contrato mínimo)

O trabalho de longo prazo **não** é um "gateway de IRQ no Core 0" (o que os
rascunhos apagados propunham). É separar de vez o que o Emu68 confunde
(`ARM IRQ ≡ Amiga IPL`), dando a ele um contrato de host pequeno — do qual metade
já existe em `[[emu68_public_api]]`:

- `emu68_machine_set_ipl()` — IPL persistente dirigido por software; o host
  decide a fonte (Rigel hoje, fio real no PiStorm, stub no teste).
- `emu68_machine_run()` / `emu68_machine_request_stop()` — JIT como backend
  escalonável, não dono do core, implementado pelo runtime público com fronteiras
  nativas e contagem de ciclos, não por wrapper Bellatrix.
- IRQ de device do host permanece inteiramente fora do Emu68: ou polled (atual), ou
  — se algum dia medição justificar — numa vector table **do Bellatrix**, num core
  onde o Emu68 não roda, nunca encadeada no tradutor INT6 dele.

Isso reordena a prioridade dos rascunhos de trás pra frente: o que eles punham por
último ("Emu68 como backend puro" via API pública) é na verdade a fundação, e o
gateway de IRQ que punham primeiro é opcional e provavelmente desnecessário.

## O que foi rejeitado e por quê

- **Gateway de IRQ no Core 0 / top-half–bottom-half** (rascunhos): constrói sobre um
  handler que (a) não dispara no Bellatrix e (b) hardcoda INT6. Reconstruiria, com
  risco ao ABI do JIT, capacidade que o polling no Core 3 já entrega.
- **HAL portátil de IRQ com capabilities/afinidade** (`consideracoes.md`): resolve
  um problema inexistente — não há roteamento de IRQ de device pra abstrair, está
  tudo em polling. A única intuição aproveitável ali era ownership explícito de
  vetores, já coberto pela decisão acima.

## Polling não é o estado final — Emu68 deixa de possuir as IRQs da plataforma

Correção de tom: polling foi o bootstrap correto (evita a colisão INT6), mas **não**
é a decisão permanente. A monopólio do subsistema de exceção/IRQ pelo Emu68 nos
custa em dois lugares: (1) o fault + lock por acesso no barramento (performance),
(2) forçar todo I/O a polling e o IPL a injeção por software.

A resolução não é manter polling para sempre — é subordinar o Emu68 a um
**árbitro** dono da máquina (`[[issue_core0_arbiter_scheduler]]`) e instalar um
subsistema de exceções da plataforma pertencente ao Bellatrix. O roteamento de
IRQ ao Core 0 observado hoje é uma decisão PiStorm do Emu68, não um compromisso
futuro do Bellatrix.

Na topologia alvo, o Core 3 possui o I/O físico e recebe diretamente suas IRQs
sempre que o hardware permitir. O Core 0 é árbitro de causalidade e tempo, não
top-half universal de drivers: recebe do Core 3 eventos/completions normalizados
por filas e wakeups coalescidos. Somente uma limitação real de afinidade pode
justificar Core 0 como gateway mínimo; mesmo nesse fallback, CherryUSB/DWC2,
UART/BT e HDMI/DMA continuam executando exclusivamente no Core 3.

Sobre áudio: pela memória do projeto o gargalo é **velocidade-de-emulação vs
realtime**, não o mecanismo de entrega (`[[issue_paula_audio_timing]]`). IRQ tira
jitter; não cura déficit de velocidade. Por isso device IRQ para áudio continua
posterior à estabilização do backend/árbitro, mas sua afinidade alvo é o Core 3,
owner do hardware, e não o Core 0.

## Arquivos relevantes

- `emu68/src/aarch64/vectors.c:140-189` — vectors IRQ/FIQ/SError (tradutor INT6→IPL)
- `src/runtime/core_io.c` — Core 3, polling de USB + Bluetooth
- `src/io/bluetooth/bt_hal_raspi3.c:40-51` — regime "no routed IRQs" documentado
- `src/cpu/emu68/emu68_machine.h` — contrato público de CPU, memória, execução e IPL
- `src/cpu/emu68/emu68_backend.c` — adapter Bellatrix para o contrato público
- CLAUDE.md — "IPL injection", "ABI constraint"
