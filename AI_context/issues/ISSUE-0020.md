---
id: ISSUE-0020
title: "new_aros.rom (maio/2026) boot screen nao aparece sem HARNESS_MSGPORT_OWNER_FIX — mp_SigTask em getunit()"
status: doing
priority: high
type: bug
owner: unassigned
created_at: 2026-06-28
updated_at: 2026-06-28
tags:
  - aros
  - rom
  - exec
  - trackdisk
  - harness
related_files:
  - src/roms/new_aros.rom
  - tools/harness/main.c
  - tools/harness/musashi_backend.c
  - external/aros/arch/m68k-amiga/devs/trackdisk/trackdisk_device.c
  - external/aros/rom/disk/getunit.c
---

> **Issue secundária (2026-06-28)**: boot screen do `new_aros.rom` (ROM mai/2026)
> não aparece sem `HARNESS_MSGPORT_OWNER_FIX=1`; com ADF inserido o boot funciona
> mesmo sem o fix. Issue principal
> (`aros.rom + aros.adf`) foi resolvida por RC1+RC2 — ver ISSUE-0015/ISSUE-0021.

**Root cause**: `getunit()` em `trackdisk_device.c:52` chama `WaitPort` em uma
porta cujo `mp_SigTask` aponta para a TD task permanentemente. Quando uma task
cliente chama `beginio()` → `getunit()`, `GiveUnit()` sinaliza a TD task (errada),
e a task cliente fica bloqueada.

Exposto pelo commit AROS `06c521a903` (abr/2026): corrigiu `&&`→`&` em
`exec_locks.c` + `Permit()` em `useralert.c`. ROM antiga (jul/2025) ficava em
`Forbid` permanente durante init → race invisível. ROM nova (mai/2026) com
task switching real → race exposta.

**Workaround ativo** (`tools/harness/musashi_backend.c`):
`HARNESS_MSGPORT_OWNER_FIX=1` intercepta `WaitPort` (LVO 64) e `WaitIO` (LVO 74)
e atualiza `mp_SigTask` para a task chamadora real antes de bloquear. Com o
workaround, `new_aros.rom + aros.adf` chega ao boot screen.

**Fix limpo proposto** (não aplicado):
```c
// external/aros/arch/m68k-amiga/devs/trackdisk/trackdisk_device.c
static void getunit(struct TrackDiskBase *tdb) {
    while (GetUnit(&tdb->td_dru) == NULL) {
        tdb->td_druport.mp_SigTask = FindTask(NULL);  // callee real
        WaitPort(&tdb->td_druport);
    }
}
```
Seguro em m68k cooperativo: sem reentrância simultânea em `getunit`.

**Próximo passo**: aplicar o fix no source AROS e verificar com `new_aros.rom + aros.adf`
sem `HARNESS_MSGPORT_OWNER_FIX`. Pode haver segunda instância similar no
`console.device` (dump mostra `tc_SigRecvd` fora do `tc_SigWait`).

# Resumo

## Estado atual verificado em 2026-06-28

`new_aros.rom` tambem trava sem ADF:

```
rtk timeout 50 env HARNESS_OS_DEBUG_DUMP=1 \
  ./out/harness-rigel/harness src/roms/new_aros.rom --frames 3001 --headless
```

Resultado:

- Serial para em `romtaginit done`.
- PC fica no idle loop `STOP #$2000` (`PC=0x00fe7462`).
- `TaskReady` esta vazia.
- `ThisTask` fica como `input.device` em `TS_WAIT`.
- Nenhuma task do dump atual tem `tc_SigWait & tc_SigRecvd != 0`.
- `console.device` tem `tc_SigWait=00053000` e `tc_SigRecvd=00020000`, mas
  esses masks **nao intersectam** (`0x53000` nao inclui `0x20000`).
- `trackdisk.device` tambem tem sinal recebido (`00080000`) fora do mask de
  espera (`00028000`).

Com `BELLATRIX_TRACE=1 BELLATRIX_RIGEL_TRACE=1`, o trace util no harness e o
Rigel trace: VBL/PORTS continuam chegando, IPL sobe/desce, e o CPU volta sempre
ao `STOP #$2000`. Nao apareceu um trace Bellatrix separado nesse binario porque
o harness esta no caminho Musashi + Rigel.

Trace de chamadas Exec (`HARNESS_EXEC_CALL_TRACE=1`) confirma que `Wait()`,
`SetSignal()` e `ReplyMsg()` continuam vivos globalmente. `input.device` e
`trackdisk.device` acordam e voltam a esperar repetidamente. Portanto o problema
nao e "Exec inteiro nao acorda tasks"; o bloqueio atual parece ser uma task ou
cadeia de inicializacao especifica esperando um evento que nunca e produzido.

Comparacao sem ADF com `src/roms/aros.rom`:

```
rtk timeout 50 ./out/harness-rigel/harness src/roms/aros.rom --frames 3001 --headless
```

A ROM antiga passa por:

```
[EXEC] InitCode: calling InitResident (-10 81 "p96gfx.hidd")
[EXEC] InitCode: calling InitResident (-45 01 "alert.hook")
[EXEC] InitCode: calling InitResident (-49 01 "ATA boot wait")
[EXEC] InitCode: calling InitResident (-50 01 "dosboot.resource")
```

A ROM nova nunca imprime o `InitResident (-10 "p96gfx.hidd")`. Portanto o
bloqueio acontece depois de `romtaginit done` e antes de o fluxo de `InitCode`
retornar para o proximo residente. Isso descarta ADF/floppy como causa primaria.

Hipotese antiga refutada: **nao ha evidencia no dump atual de task sinalizada que
deveria estar pronta e ficou presa em `TaskWait`**. A suspeita de bug generico de
wakeup/scheduler SMP deve ser tratada apenas como contexto historico, nao como
lead principal, ate aparecer uma task com `tc_SigWait & tc_SigRecvd != 0`.

Lead atual: identificar qual task/codigo fica esperando o evento ausente logo
apos `romtaginit done`. O dump de OS foi estendido para ler slow RAM e imprimir
`tc_SigWait`, `tc_SigRecvd`, `ETask` e uma varredura heuristica de contexto salvo.
Essa varredura ainda precisa ser refinada porque os PCs salvos atuais parecem
falsos positivos (`000100c*`/`00ffffff`), provavelmente por offset de `ETask` em
build com campos/alinhamentos diferentes.

A ROM `new_aros.rom` (build AROS 13 maio/2026) para em `romtaginit done` e nunca
avanca para o boot screen. A ROM anterior `aros.rom` (build 30 julho/2025) funciona
corretamente e, sem ADF, passa por `p96gfx.hidd`, `alert.hook`, `ATA boot wait` e
`dosboot.resource`.

Importante para futuras sessoes: nao usar ADF, ATA boot wait, CD-ROM ou Rigel
bitplane/Copper como explicacao primaria para esta ROM nova. A reproducao sem
ADF trava antes de `p96gfx.hidd`; os devices de input/trackdisk ainda acordam
por interrupcoes; e o problema aparece antes de qualquer tela AROS ser montada.

# Contexto dos arquivos de ROM

| Arquivo | Build | Status |
|---|---|---|
| `src/roms/aros.rom` | 30/07/2025 | Funciona — chega ao boot screen |
| `src/roms/new_aros.rom` | 13/05/2026 | Para em `romtaginit done` |
| `new_rom/aros-ext.bin` + `new_rom/aros-rom.bin` | 13/05/2026 | Fonte do new_aros.rom |

Concatenacao correta verificada: `aros-ext.bin` (EXT ROM, 0xE00000) + `aros-rom.bin`
(main ROM, 0xF80000). Ambos 512KB = 1MB total. Headers corretos:
- Offset 0x00000: `AROS EXT Extension Libraries`
- Offset 0x80000: `AROS ROM Operating System`

# Output serial observado

## ROM antiga (aros.rom) — funciona
```
[Resident modules list — ~60 entradas]
[EXEC] InitCode: calling InitResident (120 01 "exec.library")
[EXEC] InitCode: calling InitResident (105 01 "diag init")
callroms / callroms done
[EXEC] InitCode: ... (todas as entradas, até -50)
romtaginit / romtaginit done
[EXEC] InitCode: calling InitResident (-50 01 "dosboot.resource")
```
Chega a dosboot dentro de ~10000 frames.

## ROM nova (new_aros.rom) — travada
```
ROMInfo: 1MiB ROM detected
ROMInfo: ROM region(s)..  0x00e00000-0x00e7ffff / 0x00f80000-0x00ffffff
callroms / callroms done
[memory regions]
romtaginit / romtaginit done
[silencio total — nenhum output adicional em 30000 frames]
```

# Investigacao no harness

## Comportamento do CPU

Com `BELLATRIX_RIGEL_TRACE=1`, frames 2000-2500:

- **Frames 1-40**: `ipl=0`, `intreq=0000`. CPU inicializando, INTENA so com master
  enable (0x4000). Exec ainda nao habilitou interrupcoes especificas.
- **Frame ~2488**: INTENA torna-se `0x202c` (VERTB+PORTS+SOFT+EXTER). VBL passa a
  gerar IPL=3. Exec esta no scheduler com task switching real.
- **Frames 2488+**: CPU alterna entre `STOP #$2000` (idle task) e handlers de VBL/CIA.
  `PC=0xfe7462` fixo no EXEC-DUMP de frame 3000 (mesmo com 30000 frames).

O `PC=0xfe7462` e o loop idle do exec:
```
0xfe7456: MOVE.W #$C000,$DFF09A  ; habilita master INTENA
0xfe745e: STOP #$2000            ; aguarda interrupcao
0xfe7462: BRA.S  0xfe743a        ; loop
```

CPU esta no idle porque **todas as tasks estao bloqueadas**.

## Diferenca arquitetural entre as builds

### ROM antiga — exec_locks.c com bug `&&`
Commit `06c521a903` (24/04/2026) corrigiu `exec_locks.c`: operador `&&` (logico) 
substituido por `&` (bitwise) ao testar `LOCKF_DISABLE` e `LOCKF_FORBID`.

**Antes (ROM Jul/2025)**: qualquer flags != 0 chamava `Forbid()` E `Disable()`
incondicionalmente. Resultado pratico: o sistema ficava sempre em estado Forbid
(task switching desabilitado) durante toda a inicializacao. Exec InitCode rodava
linearmente como se fosse single-threaded.

**Depois (ROM Mai/2026)**: flags testados corretamente. Task switching funciona
normalmente. `ObtainSemaphore()` agora bloqueia de verdade quando o semaforo nao
esta disponivel, e outra task precisa acordar quem esta bloqueado.

### ROM nova — scheduler STOP-based
A ROM nova usa `STOP #$2000` no loop idle em vez do busy-poll da ROM antiga
(`AROS-LOOP`, PC `0xfe85fa`, spinning em SERDATR). Isso exige que interrupcoes
cheguem corretamente para acordar o CPU — o que esta funcionando (VBL confirmado
nos traces).

## Hipoteses antigas agora rebaixadas

### ATA boot wait / GAYLE

Esta hipotese nao deve guiar a investigacao atual. A ROM nova nao chega ao log
`InitResident (-10 "p96gfx.hidd")`; `ATA boot wait` e prioridade -49 e so seria
alcancado depois.

### p96gfx.hidd

`p96gfx.hidd` era o candidato imediato porque e o proximo residente visto na ROM
antiga. Mas a ROM nova nao imprime a entrada desse residente; ainda nao esta
provado se o bloqueio ocorre dentro dele, antes dele, ou em uma task criada pelo
fim de `romtaginit`. Manter como candidato, nao como conclusao.

# Investigacao adicional: modulo apos romtaginit

A lista de residentes da ROM antiga (capturada com harness) revela a ordem exata:

```
-9:  romboot            ("romtaginit done" — ultimo output confirmado)
-10: p96gfx.hidd        ← PROXIMO MODULO APOS ROMTAGINIT
-45: alert.hook
-49: ATA boot wait
-50: dosboot.resource
```

# Proximos passos

## 1. Identificar a task/evento bloqueante (prioritario)

- Refinar o dump de `ETask->et_RegFrame` com offsets corretos para este build.
- Capturar os ultimos `Wait()` por task com cap/filtro para evitar log gigante.
- Comparar a lista `TaskWait` nova vs. ROM antiga imediatamente apos
  `romtaginit done`.
- Descobrir quem deveria sinalizar a task sem nome com `wait=00080000`.

## 2. Comparar lista de residentes entre ROMs

Correr o novo ROM no harness para capturar a lista completa de residentes.
Comparar com a lista da ROM antiga para identificar modulos novos ou prioridades
alteradas que possam ser responsaveis pelo bloqueio.

## 3. Tornar `HARNESS_EXEC_CALL_TRACE` utilizavel

O trace bruto de chamadas Exec funciona, mas gera milhoes de linhas porque
`input.device` e `trackdisk.device` acordam continuamente. Adicionar filtro/cap:

- imprimir apenas os primeiros N eventos por task/chamada;
- opcionalmente ignorar `input.device`/`trackdisk.device` depois do primeiro ciclo;
- sempre manter os ultimos `Wait()` de cada task para saber quem ficou bloqueado.

## 4. Testar ROM 2011-aros.rom

Criar o arquivo `src/roms/2011-aros.rom` (binarios em `new_rom/2011/`) e rodar
no harness. Serve como controle: se 2011 tambem travar, o problema e no harness.
Se passar, o problema e especifico ao comportamento da ROM 2026.

## 5. Refinar dump de contexto salvo

O dump heuristico atual encontra falsos positivos para `ETask->et_RegFrame`.
Calcular offsets reais deste build, ou usar uma estrategia mais robusta de
context-scan, antes de concluir qualquer PC salvo de task.

# Referencias

- Commit AROS `06c521a903`: exec SMP lock fix (`&&` → `&` + Permit() em useralert)
- `external/aros/arch/m68k-amiga/romboot/romboot.c`: romtaginit
- `external/aros/arch/m68k-amiga/diag/diag.c`: callroms
- `external/aros/rom/exec/wait.c`: `Wait()` e transicao para `TS_WAIT`
- `external/aros/rom/exec/signal.c`: `Signal()` e wakeup de tasks

# Log

- 2026-06-28: issue criada. Investigacao inicial confirmou estrutura de ROM
  correta, IRQs vivos e bloqueio pos-`romtaginit`. A primeira suspeita era ATA
  `DetectionSem`, mas isso foi rebaixado porque `ATA boot wait` vem muito depois
  do ponto onde a ROM nova para.
- 2026-06-28: lista de residentes da ROM antiga capturada: o modulo apos
  `romtaginit` e `p96gfx.hidd` (-10), seguido de `alert.hook`, `ATA boot wait`
  e `dosboot.resource`. `p96gfx.hidd` permanece apenas candidato de fronteira:
  ainda nao ha prova de que a ROM nova entra nele.
- 2026-06-28: nova reproducao sem ADF. `new_aros.rom` para antes de chamar
  `p96gfx.hidd`; `aros.rom` passa por `p96gfx.hidd`, `alert.hook`,
  `ATA boot wait` e `dosboot.resource`. Traces Rigel confirmam IRQ/VBL vivos.
  Dump de OS mostra `TaskReady` vazia, mas **nenhuma** task com
  `tc_SigWait & tc_SigRecvd != 0`; a leitura inicial de `console.device` como
  "sinal pronto para acordar" estava errada (`0x53000` nao inclui `0x20000`).
  Hipotese ATA/p96 deixa de ser a principal; foco passa a ser identificar qual
  evento/sinal especifico deixou a cadeia de boot sem tasks prontas.
- 2026-06-28: `HARNESS_EXEC_CALL_TRACE=1` confirmou chamadas Exec reais. O log
  bruto e grande demais, mas mostra `input.device` e `trackdisk.device` alternando
  `Wait()`/`ReplyMsg()`/`SetSignal()`, entao `Wait()`/wakeup nao esta globalmente
  quebrado.
