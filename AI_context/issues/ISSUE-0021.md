---
id: ISSUE-0021
title: "AROS não chega à tela gráfica via ADF — bug no harness"
status: consolidated
priority: critical
type: bug
owner: unassigned
created_at: 2026-06-28
updated_at: 2026-06-28
tags:
  - aros
  - harness
  - trackdisk
  - exec
  - adf
related_files:
  - tools/harness/musashi_backend.c
  - external/aros/arch/m68k-amiga/devs/trackdisk/trackdisk_device.c
  - external/aros/arch/m68k-amiga/devs/trackdisk/trackdisk_hw.c
  - external/aros/rom/disk/getunit.c
  - external/aros/rom/disk/giveunit.c
consolidated_in: AI_context/consolidated/rigel_aros_adf_investigation.md
---

> **CONSOLIDATED (2026-06-28)** — Ver conclusão completa em
> `AI_context/consolidated/rigel_aros_adf_investigation.md` (seção "Causa-Raiz 3").
> Bug `mp_SigTask` em `getunit()` identificado; workaround via
> `HARNESS_MSGPORT_OWNER_FIX=1`; fix limpo proposto para source AROS. AROS
> renderiza tela gráfica após RC1+RC2+RC3 aplicados.

# Resumo

WinUAE inicializa AROS até a tela gráfica com `aros.rom` + `aros.adf`. O harness
não consegue. O bug está no harness/emulação, não nos dados do ADF nem na ROM.

O foco desta issue é identificar e corrigir o que impede o harness de completar o
boot do AROS até a tela gráfica. Qualquer problema específico de versão de ROM
(`new_aros.rom` vs `aros.rom`) é secundário — o primário é chegar à tela gráfica.

# Evidência coletada (2026-06-28)

## Comportamento observado com RIGEL_DISK_TRACE=1

Com `new_aros.rom` + `aros.adf` + `HARNESS_TRACKDISK_WAITPORT_OWNER_FIX=1`:

```
[SERIAL] romtaginit done
[RIGEL-DISK-START] cyl=0 side=0   ← boot block
[RIGEL-DISK-START] cyl=0 side=1
[RIGEL-DISK-START] cyl=0 side=0   ← retry
[RIGEL-DISK-START] cyl=40 side=0  ← root block
[RIGEL-DISK-START] cyl=24 side=0
...
[TRACKDISK-WAITPORT-FIX] port=00c12708 sigtask:00c94f40->00c9f6a0
... ~30 leituras adicionais ...
[EXEC-DUMP] frame=3000 PC=0x00fe7462  ← idle loop, todas tasks em Wait
```

- DMA funciona: todos os reads têm DISK-START→DISK-DONE com `irq=0002` (DSKBLK)
- O filesystem navega ativamente: ~30 tracks lidas incluindo root block, directory
  blocks, seeks não-lineares (padrão de filesystem scan normal)
- **Um** `TRACKDISK-WAITPORT-FIX` dispara durante a sequência de reads
- Após ~30 reads o sistema trava: PC=0xfe7462 (idle loop exec), `TaskReady` vazia

## Silêncio serial após romtaginit — normal no novo ROM

O novo ROM (`new_aros.rom`, maio/2026) não tem `EXECDEBUGF_INITCODE` setado em
`SysBase->ex_DebugFlags`. As mensagens `[EXEC] InitCode: calling InitResident`
são suprimidas silenciosamente. Isso não é evidência de stall — InitCode continua
mas sem output. `bug("romtaginit done")` usa kprintf direto, por isso aparece.

## Bug confirmado: getunit() WaitPort com mp_SigTask errado

Em `arch/m68k-amiga/devs/trackdisk/trackdisk_device.c:52`:

```c
static void getunit(struct TrackDiskBase *tdb)
{
    while (GetUnit(&tdb->td_dru) == NULL) {
        WaitPort(&tdb->td_druport);   // mp_SigTask = TD task (SEMPRE)
    }
}
```

`td_druport.mp_SigTask = tdb->td_task` é setado uma vez e nunca muda. Quando
`beginio()` com `IOF_QUICK` chama `TD_PerformIO` → `getunit` de uma task cliente,
o `GiveUnit()` → `ReplyMsg` sinaliza a TD task (errada). A task cliente fica
bloqueada para sempre.

**Por que o novo ROM expõe isso:** O commit `06c521a903` corrigiu `useralert.c`
(não `exec_locks.c` em si — esse arquivo inteiro é `#ifdef __AROSEXEC_SMP__` e
não compila para m68k). A linha relevante é: `EXEC_UNLOCK_LIST` (no-op em m68k)
→ `EXEC_UNLOCK_LIST_AND_PERMIT` (chama `Permit()`). Com o ROM antigo, qualquer
chamada a `Alert_AskSuspend` deixava o sistema em Forbid permanente → execução
essencialmente single-threaded durante init → race condition invisível. Com o
novo ROM o Permit() é chamado corretamente → task switching real → race exposta.

**Fix no source AROS** (em vez do hack no harness):
```c
static void getunit(struct TrackDiskBase *tdb)
{
    struct DiskBase *DiskBase = tdb->td_DiskBase;
    while (GetUnit(&tdb->td_dru) == NULL) {
        tdb->td_druport.mp_SigTask = FindTask(NULL);  // sinalizar o chamador real
        WaitPort(&tdb->td_druport);
    }
}
```

Em multitasking cooperativo m68k, não há reentrância simultânea em `getunit`,
então a atualização do campo é segura.

## Estado de tasks no stall

Do EXEC-DUMP (frame=3000):
- `trackdisk.device`: `tc_SigWait=0x00028000`, `tc_SigRecvd=0x00080000`
  → bit 19 (= `td_druport.mp_SigBit`) recebido mas fora do wait mask
- `console.device`: `tc_SigWait=0x00053000`, `tc_SigRecvd=0x00020000`
  → bit 17 recebido mas fora do wait mask
- Nenhuma task tem `tc_SigWait & tc_SigRecvd != 0`

O bit extra no TD task confirma o WaitPort bug: `GiveUnit` sinalizou a TD task
(via `mp_SigTask` errado) em vez da task cliente. O bit extra no `console.device`
sugere um segundo ponto de bloqueio de mesma natureza, mais adiante no boot chain.

# Hipótese atual

O bug do `mp_SigTask` não é isolado ao `getunit()`. É um padrão que pode aparecer
em múltiplos pontos do boot chain onde uma task chama `WaitPort` em uma porta cuja
`mp_SigTask` foi setada por outra task. O hack `HARNESS_TRACKDISK_WAITPORT_OWNER_FIX`
corrige UMA instância via intercepção no harness. Há pelo menos uma segunda
instância (evidenciada pelo `console.device`). A correção no harness não cobre
caminhos que vão por `DoIO` → `WaitIO` → `WaitPort` em reply ports de outras tasks.

Mesmo que todos os bugs mp_SigTask sejam resolvidos, pode haver outras diferenças
fundamentais entre WinUAE e o harness que impedem o boot completo. O WinUAE é
a referência: qualquer coisa que ele faz e o harness não faz é candidata a causa.

# Próximos passos

1. **Testar `aros.rom` + ADF no harness** para determinar se o problema afeta
   só o novo ROM ou qualquer ROM. Se `aros.rom` também falha, confirma bug
   fundamental independente do exec_locks.c.

2. **Identificar a segunda task bloqueada**: capturar EXEC-DUMP no momento em que
   as disk reads param (não ao frame 3000 que pode ser muito depois). Habilitar
   `HARNESS_EXEC_CALL_TRACE=1` e filtrar os últimos `Wait()` por task.

3. **Investigar path DoIO → WaitIO → WaitPort** para reply ports: o fix atual
   intercepta WaitPort (LVO 64). O `WaitIO` também chama WaitPort no reply port
   do IO request. Se o reply port de um DoIO pertence a uma task diferente do
   chamador, o mesmo bug ocorre. Candidato: o console.device bit 17.

4. **Comparar com WinUAE**: identificar o que WinUAE implementa corretamente que
   o harness não implementa. Focar em comportamentos de Exec (Signal/Wait/WaitPort)
   e device I/O (DoIO/WaitIO/ReplyMsg) mais do que em hardware chipset.

# Log

- 2026-06-28: issue criada. WinUAE confirmado como referência. Bug mp_SigTask em
  `getunit()` identificado e raiz causal explicada (useralert.c Permit fix).
  Disk trace confirma DMA funcionando, filesystem navegando, stall após ~30 reads.
  Silêncio serial pós-romtaginit confirmado como comportamento normal do novo ROM.
  Segundo ponto de bloqueio inferido do console.device signal mismatch.
- 2026-06-28: usuário confirmou que WinUAE consegue bootar `aros.adf` com
  `aros.rom`; portanto a hipótese "ROM/ADF mismatch" deve ser tratada como
  refutada para este par. O foco volta para diferença de emulação/harness.
- 2026-06-28: adicionado diagnóstico env-gated em
  `tools/harness/musashi_backend.c`: `HARNESS_MSGPORT_OWNER_FIX=1` substitui o
  nome estreito `HARNESS_TRACKDISK_WAITPORT_OWNER_FIX=1` e cobre `WaitPort()`
  e `WaitIO()`; a variável antiga ainda habilita o mesmo caminho por
  compatibilidade.
- 2026-06-28: teste limpo com `new_aros.rom + aros.adf`:
  `HARNESS_MSGPORT_OWNER_FIX=1 HARNESS_OS_DEBUG_DUMP=1 ./out/harness-rigel/harness
  src/roms/new_aros.rom --adf src/disks/aros.adf --frames 5000 --headless`.
  Resultado: só uma correção disparou,
  `WaitPort port=00c12708 sigtask:00c94f40->00c9f6a0`; nenhum `WaitIO` owner
  fix disparou. Em frame 3000, `TaskReady` ainda vazia; `console.device`,
  `DF0`, `trackdisk.device`, `Boot Mount`, `Lib & Dev Loader Daemon` e `CON`
  ainda têm `tc_SigRecvd` fora de `tc_SigWait`. Portanto o segundo bloqueio
  não é simplesmente um `WaitIO()` com reply port de outra task.
- 2026-06-28: teste com trace Rigel verboso demais, mas útil, mostrou que com
  o owner fix o caminho gráfico chega a `BPLCON0=a201`, 2 bitplanes e
  framebuffer `768x256`, com eventos de composição não-zero perto do frame 4200.
  O dump PPM não foi gerado porque o dump de frame continua acoplado ao trace
  Rigel e não disparou no caminho usado.
- 2026-06-28: controle com `aros.rom + aros.adf` no harness confirma a correção
  do usuário. A ROM antiga passa por `dosboot.resource`, entra em
  `InitCode(0x04)`, inicializa `DOSBoot cleanup`, `icon.library`,
  `lddemon.resource`, `shell.resource`, `shellcommands.resource`,
  `workbook.resource`, `con-handler`, `ram-handler`, `nonvolatile.library`,
  `nvdisk.library`, `setpatch.library`, e sai de `InitCode(0x04)`. Ainda assim
  aparecem respostas com bits fora do wait mask e o harness termina em idle
  (`PC=0x00fe849a`) no frame 3200. Isso reforça que o problema atual é diferença
  de sincronização/sinalização/execução no harness, não validade do ADF.
- 2026-06-28: comparação direta no alvo atual (`aros.rom + aros.adf`) mostrou que
  `HARNESS_MSGPORT_OWNER_FIX=1` não é relevante para a falha que vamos perseguir
  agora. Sem o fix, a ROM também passa por `dosboot.resource`, entra e sai de
  `InitCode(0x04)`, carrega Workbook/handlers e termina no mesmo idle
  (`PC=0x00fe849a`) no frame 3200. Portanto o owner-fix deve ser tratado apenas
  como diagnóstico histórico criado para `new_aros.rom`, não como lead principal
  para `aros.rom + aros.adf`.
