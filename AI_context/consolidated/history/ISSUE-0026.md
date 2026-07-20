---
id: ISSUE-0026
title: "aros.rom não completa o startup no harness com HARNESS_CPU=68020 (com ou sem HDF)"
status: superseded
priority: medium
type: bug
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - hdf
  - aros
  - lide.device
  - harness
  - musashi
  - cpu
related_files:
  - tools/harness/main.c
  - tools/harness/musashi_backend.c
  - src/machine/expansions/lide_cdrom/lide_cdrom.c
---

# Sintoma

`aros.rom` no harness Musashi com `HARNESS_CPU=68020` **não completa o
startup** — e isso ocorre **mesmo sem HDF**, portanto o bug é do caminho
68020 no harness/Musashi, não do disco. (Com 68000 o aros.rom boota até o
desktop normalmente.)

O HDF ArosOne-Lite só evidenciou o problema: seu boot code exige 68020+,
então não há hoje nenhuma combinação de CPU que chegue ao desktop dele.

## Repro (mínimo, sem HDF)

```bash
HARNESS_CPU=68020 KICKSTART=src/roms/aros.rom ./run.sh harness
```

## Comportamento observado (2026-07-03)

| CPU | Resultado |
|---|---|
| 68000 (default) | GURU: task `UDH0` morre com "Line 1111 (F) Emulator/Coprocessor error" em PC=0x00CA5F91 — o filesystem/boot code do HDF é build 68020+. Screenshot: `aros_hdf68000.jpg`. |
| 68020 | Sem GURU. Todos os residentes inicializam (`ata.device`, `lide.device` via Chainloader, `lowlevel.library`…), Chainloader carrega `lide.device` do diag ROM, mas **o startup-sequence não completa** — não chega ao desktop do AROS One. |
| new_aros.rom + 68020 | Para logo após `Chainloader ->success / callroms done` — nenhum InitResident roda. Provável ISSUE-0020 (sem `HARNESS_MSGPORT_OWNER_FIX=1`); ainda não retestado com o env. |

Boot com 68020 é notavelmente lento no serial (residentes levam minutos, no
68000 levam segundos) — pode ser I/O pesado no HDF ou algo girando em loop.

## Diagnóstico 2026-07-03 (headless FRAMES=3100, aros.rom, sem HDF)

| CPU | Comportamento |
|---|---|
| 68000 | Boot completo (referência). |
| 68010 | **Nenhuma** linha `InitCode` no serial — diverge muito antes do 020. PC final `0x0073fab8` (RAM), não o idle loop. |
| 68020/68040 | InitCode roda até `lowlevel.library` (pri 0) e **nunca chega aos residentes de prioridade negativa** (`afs-handler`, `romboot`, `dosboot`). |

Estado no deadlock do 68020 (`HARNESS_OS_DEBUG_DUMP=1`):
- Task de bootstrap `@00c123d8` em `WAIT` com `wait=0x00000000` (máscara de
  sinais vazia — nunca acorda). TaskReady vazio; todos os demais tasks em WAIT.
- `OSDBG-PORT`: bloqueado em `WaitPort @00c28f74` com a MsgPort **inteiramente
  zerada** (sigtask=NULL, sigbit=0, lista não inicializada).
- PC do idle: `0xfe849a`. `HARNESS_MSGPORT_OWNER_FIX=1` **não muda nada**
  (confirmado: o fix só se aplica ao new_aros.rom / ISSUE-0020).
- FSAVE do 68040 corrigido em patch 0013 (modos (An), (d16,An), (d8,An,Xn))
  — o crash de FPU era um gap do Musashi e foi eliminado; o deadlock persiste.

Hipótese de trabalho: algo dependente do tipo de CPU corrompe/perde o wakeup
do bootstrap durante a init do lowlevel.library — candidatos: construção
manual de exception stack frames (formato 68010+ com format word) no exec do
AROS vs. o caminho de IPL/interrupção do harness; ou detecção de CPU do AROS
(AttnFlags=0x0003 no run 020) tomando um caminho de código quebrado no
harness. O comportamento distinto do 68010 (falha antes de tudo) reforça a
pista de stack frames, já que 68010 introduz o format word.

## Mecanismo do deadlock (confirmado no source do AROS, 2026-07-03)

`external/aros` agora está baixado (shallow). Cadeia exata:

1. `lowlevel.library` Init (`workbench/libs/lowlevel/lowlevel_init.c`):
   `CreateMsgPort()` → **`FreeSignal(mp_SigBit); mp_SigBit = -1`** — porta sem
   signal bit, de propósito.
2. `OpenDevice("input.device")` OK; depois `DoIO(IND_ADDHANDLER)`.
3. `input.device` BeginIO (`rom/devs/input/input.c:146`): `IND_ADDHANDLER` é
   **sempre non-quick** → `PutMsg(InputDevice->CommandPort, …)`.
4. O design só funciona se o `Signal` do PutMsg **preemptar imediatamente**
   para a task do input.device (pri maior), que processa e responde antes do
   `WaitIO`. Com `mp_SigBit = -1`, o `WaitIO` degenera em `Wait(0)` — se a
   resposta não chegou antes, dorme para sempre. É exatamente o estado
   observado (`wait=0x00000000`).
5. No dump: `input.device` task com `recvd=0x00000000` — o Signal do PutMsg
   **nunca chegou**. E a porta do último PutMsg (`@00c93ad0`) e a porta do
   WaitPort (`@00c28f74`) aparecem **inteiramente zeradas** no momento do dump.

Ou seja: em 68020/68040 a preempção-por-Signal do exec m68k do AROS não
acontece (ou o PutMsg opera sobre uma porta corrompida/zerada). Em 68000
funciona. Suspeitos no source (arch/m68k-all/):
- `exec/schedule.S`, `switch.S`, `dispatch.S`, `exitintr.S` — todo o caminho
  Schedule/Switch/Dispatch roda em supervisor e termina em `rte`; frames de
  exceção 020 têm format word (o `regs_t`/`ExceptionContext` do AROS só tem
  d[8],a[8],sr,pc — sem format word; o RTE do Dispatch reaproveita o frame
  corrente do supervisor stack).
- `kernel/kernel_cpu.c` `cpu_Dispatch` — idle usa `stop #0x2000`; caminho
  TF_EXCEPT manipula stack.
- Portas zeradas: verificar se algo (cache handling? `CopyMem` 020?
  `copymem_020.S`!) zera/corrompe memória — AROS seleciona variantes de
  CopyMem por CPU (`copymem_020.S`, `copymem_040.S`): um bug no CopyMem 020
  do Musashi/harness corromperia estruturas ao copiá-las, o que casaria com
  "porta inteiramente zerada" e com falhas diferentes por tipo de CPU.

## CAUSA RAIZ E FIX (2026-07-03)

**Interrupção perdida por checagem de IRQ apenas em fronteira de timeslice do
Musashi.** Cadeia comprovada por instrumentação (`HARNESS_WATCH_MEM`,
`HARNESS_PC_RING`, `HARNESS_IPL_TRACE`, todas novas no backend):

1. No PutMsg do `DoIO(IND_ADDHANDLER)`, o `Reschedule` do AROS escreve
   `INTF_SETCLR|SOFTINT` (0x8004) no INTREQ (PC `0xf8a1e0`).
2. Paula levanta IPL 1 (`set_ipl(1)`) com a máscara do SR aberta (mask=0).
3. `m68k_set_irq()` do Musashi **só armazena o nível**; `m68ki_check_interrupts()`
   roda apenas no início de cada `m68k_execute()` (ou quando uma instrução
   escreve no SR). A IRQ fica pendente e não é tomada.
4. ~35 instruções depois o bootstrap chega ao `WaitIO`→`Wait(0)`; o `Disable`
   interno derruba INTENA, Paula baixa o IPL — **a interrupção é rescindida
   antes de ser tomada**. Preempção perdida → deadlock (a porta do lowlevel
   não tem signal bit; a resposta do input.device não tem como acordá-lo).

Em 68000 os cycle counts diferentes deslocam as fronteiras de timeslice e a
IRQ (por sorte) é tomada a tempo. Isso também explicava a lentidão geral do
boot em 020 (todas as IRQs mid-slice atrasavam até o fim do slice).

**Fix:** `tools/harness/musashi_backend.c` — `musashi_set_ipl()` agora chama
`m68k_end_timeslice()` quando o nível sobe, forçando o retorno ao
`m68k_execute()` que então toma a IRQ na fronteira da instrução seguinte
(comportamento do hardware real).

> **ATENÇÃO (2026-07-20): este fix nunca chegou ao produto bare-metal.**
> Ele foi aplicado só em `tools/harness/musashi_backend.c`. O backend do
> produto, `src/cpu/musashi/musashi_backend.c`, tem `musashi_set_ipl()`
> chamando apenas `m68k_set_irq()`, sem `m68k_end_timeslice()` — ou seja,
> carrega exatamente o defeito descrito acima. Isso é o suspeito principal
> da ISSUE-0070 (AROS trava em lowlevel.library no produto) e explica por que
> "o harness passa e o produto trava": o harness tem o fix, o produto não.
> Ver `AI_context/issues/ISSUE-0070.md`.

**Resultado:** `aros.rom` com 68020 e 68040 passa `lowlevel.library`, completa
todo o InitCode (`leave InitCode`) e chega ao `dosboot`; com o
`ArosOne-Lite.hdf`, o `lide.device` boota via Chainloader, `DOSBoot cleanup`
roda e o sistema executa código carregado do disco em Fast RAM (tela do
Workbench aberta em frame 12000; carga do desktop em andamento). 68000
sem regressão.

**Pendências:**
- 68010 falha por causa própria e anterior (nenhum `InitCode` roda; suspeita:
  instruções privilegiadas 010 — `MOVE from SR` — ou frame/VBR); baixa
  prioridade, não é alvo real.
- Confirmar desktop completo do ArosOne-Lite em run interativo longo.
- Ferramentas novas de debug no backend: `HARNESS_WATCH_MEM=addr:len,...`,
  `HARNESS_PC_BURST=pc:count[:skip]`, `HARNESS_PC_RING=pc:depth:d0`,
  `HARNESS_IPL_TRACE=1` (loga set_ipl + int-ack).

## Próximos passos

- [ ] Comparar com `HARNESS_CPU=68040` (variante adicionada em 2026-07-03:
      backend + TUI `musashi-68040`) — se 68040 bootar e 68020 não, o
      suspeito é o core 68020 do Musashi ou interação com cache/CACR; se
      ambos falharem igual, o suspeito é comum (ex.: exceção/stack frame
      020+ mal tratado no harness).
- [ ] Capturar em que ponto do startup-sequence trava (serial + screenshot).
- [ ] Retestar `new_aros.rom` com `HARNESS_MSGPORT_OWNER_FIX=1`.
- [ ] Verificar leituras do HDF via lide/ata durante o stall (trace de LBA).

## Contexto

- HDF montado via `--hdf` (ISSUE-0025, Fase 1). `wb31`/outros HDFs — status
  desconhecido; ArosOne-Lite é o primeiro HDF grande com RDB real testado.
- Musashi core já tinha 030/040 compilados (`M68K_EMULATE_030/040=ON`,
  `m68kfpu.c`); o backend só expunha até 68020. Agora `--cpu 68030|68040`
  são aceitos.
