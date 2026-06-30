---
id: ISSUE-0022
title: "AROS ISO não monta como CD0: — FindCDFS() chamado antes de cdrom-handler registrar"
status: doing
priority: high
type: bug
owner: unassigned
created_at: 2026-06-30
updated_at: 2026-06-30
tags:
  - aros
  - lide.device
  - cdrom
  - iso
  - harness
related_files:
  - external/lide.device/device.c
  - external/lide.device/mounter/mounter.c
  - src/machine/expansions/lide_cdrom/atapi_cdrom.c
  - src/machine/expansions/lide_cdrom/lide_cdrom.c
  - tools/harness/main.c
  - out/harness-rigel/lide_rom_data.c
  - scripts/make_lide_rom.py
---

# Resumo

Ao iniciar o harness com `aros.rom + aros.adf + aros-amiga-m68k.iso`, AROS abre
um requester "Insert Live CD" em vez de reconhecer o ISO automaticamente. O volume
`CD0:` nunca aparece no Workbench. Causa raiz: timing entre lide.device (diag
init, prioridade 105) e cdrom-handler (prioridade -1).

## Bug #1 (RESOLVIDO): lide.rom estava vazio

`external/lide.device/lide.rom` tinha 0 bytes. O sistema de build (CMake via Docker
m68k) nunca foi executado nesta máquina. Os binários pré-compilados já existiam:
- `external/lide.device/bootrom/obj/bootnibbles` (2.0 KB — bootloader nibble-encoded)
- `external/lide.device/lide.device` (24.2 KB — device binary compilado)

**Fix aplicado:**
```bash
python3 scripts/make_lide_rom.py external/lide.device/ external/lide.device/lide.rom
python3 scripts/rom_to_c.py external/lide.device/lide.rom out/harness-rigel/lide_rom_data.c
cmake --build out/harness-rigel --target harness -j4
```

Resultado: `g_lide_rom_size = 32768u`, DiagArea executa, lide.device inicializa,
`lide ata task` aparece na lista de tasks. ATAPI funciona corretamente:
- IDENTIFY retorna device type ATAPI CD-ROM
- UNIT_ATTENTION limpo na primeira TUR após inserção
- REQUEST SENSE retorna sense data correto
- READ CAPACITY retorna 171623 setores / 2048 bytes

## Bug #2 (PENDENTE): CD não monta — FindCDFS() timing

### Root cause

Em `external/lide.device/device.c:1036`:
```c
BOOL CDBoot = FindCDFS();    // chamado em diag init, prioridade 105
...
struct MountStruct ms = { .cdBoot = CDBoot, ... };
MountDrive(&ms);
```

`FindCDFS()` percorre FileSystem.resource procurando `fse_DosType == 'CD01'`
(0x43443031). O `cdrom-handler` do AROS registra este entry durante `InitCode`
em **prioridade -1**. Quando lide.device roda (prioridade 105, durante `callroms`),
o `cdrom-handler` ainda não inicializou.

Resultado: `FindCDFS()` retorna `false` → `cdBoot = false` →

Em `external/lide.device/mounter/mounter.c:1523`:
```c
case DG_CDROM:
    if (!ms->cdBoot) {
        printf("CDROM boot disabled.\n");
        break;              // <-- ScanCDROM nunca é chamado
    }
    ret = ScanCDROM(md);
```

`ScanCDROM` (que faria `CheckPVD` + `AddBootNode`) nunca executa. Nenhum
BootNode é adicionado para o CD → nenhum `CD0:` volume → requester "Insert Live CD".

### Evidência de funcionamento correto do ATAPI

Apesar do CD não montar como volume, o ATAPI responde corretamente:
```
[ATAPI] UNIT_ATTENTION cleared after TUR
[ATAPI] READ CAPACITY: 171623 sectors, 2048 bytes/sector
```

O lide.device sabe que há um CD, mas não o monta porque `cdBoot=false`.

### Nota sobre o PVD patch

`src/machine/expansions/lide_cdrom/atapi_cdrom.c` tem um `patch_pvd_sysid()` que
troca o System Identifier do PVD para "AMIGA BOOT". O ISO tem "AROS-m68k-amiga".

Sem o patch, `CheckPVD` retornaria `isBootable=0` → `bootPri=-1`. O CD ainda
montaria como `CD0:` (só sem prioridade de boot). **O patch não é a causa do
bloqueio** — o bloqueio está antes, em `cdBoot=false` que impede `ScanCDROM`
de ser chamado.

## Opções de fix

### Opção A — Rebuild lide.device com scan diferido (requer Docker)

Adicionar um COLDSTART resident de prioridade -48 no lide.device que:
1. Roda após cdrom-handler (-1) e antes de dosboot.resource (-50)
2. Percorre as units do dispositivo, identifica CDs não montados
3. Chama `MountDrive` com `cdBoot=true`

Padrão idêntico ao "ATA boot wait" (prioridade -49) do AROS para o `ata.device`.
Requer Docker com toolchain m68k-amigaos-gcc e rebuild do lide.device.

### Opção B — Hook no harness (sem Docker)

Em `tools/harness/musashi_backend.c`, interceptar `InitCode` no ponto em que
todos os residentes de prioridade ≥ -1 já rodaram. Nesse momento:
1. Localizar lide.device na DeviceList via `FindName`
2. Abrir o device na unit 0 (CD)
3. Chamar `ScanCDROM` via IORequest

Complexidade moderada; não precisa de Docker.

### Opção C — Interceptar OpenResource no harness (sem Docker)

Em `musashi_backend.c`, interceptar a chamada LVO `OpenResource` quando o argumento
é `"FileSystem.resource"`. Antes de retornar a resposta normal, injetar um
FileSysEntry para DosType `0x43443031` (CD01) apontando para `cdrom-handler`.

Isso faz `FindCDFS()` retornar `true` antes de cdrom-handler inicializar, pois o
entry já está presente. Complexidade moderada; requer conhecer a estrutura de
FileSysEntry em memória AROS.

## Estado atual verificado (2026-06-30)

- `lide ata task` presente em TaskWait → lide.device inicializou corretamente
- Janela Intuition 432×57 em (104,100) = requester "Insert Live CD" visível
- ATAPI responde: TUR OK, READ CAPACITY 171623/2048, PVD System ID patcheado
- Nenhum `CD0:` no boot log, nenhum BootNode adicionado para CD

## Opção A — Status de implementação (2026-06-30)

Fix escolhido: **Opção A** (tarefa diferida em lide.device). Código escrito em
`external/lide.device/device.c`, mas **build bloqueado** por Docker Desktop
corrompido (read-only filesystem na VM — requer restart pelo Windows).

### Mudanças em `external/lide.device/device.c`

1. Adicionada função `cd_deferred_task()` (envolvida em `#if CDBOOT`):
   - Guarda `configDev` via `tc_UserData`
   - Abre `timer.device` (UNIT_MICROHZ)
   - Poll de 500ms, até 240 iterações (~2 minutos)
   - Ao detectar `FindCDFS()=true`, chama `MountDrive(cdBoot=true)` +
     `TweakBootList`

2. Em `init()`, após `MountDrive(&ms)`:
   - Se `!CDBoot`: verifica se há unidades ATAPI no `dev->units`
   - Se sim: chama `L_CreateTask("lide cd defer", 0, cd_deferred_task,
     LIDE_CD_DEFER_STACK, itask->cd)`
   - Tarefa usa `tc_MemEntry` via `L_CreateTask`, então o Exec a limpa
     automaticamente quando termina

### Próximos passos após restart do Docker Desktop

```bash
# 1. Rebuild lide.device via Docker
./scripts/build-lide-rom.sh

# 2. Regenerar C array embutido no harness
python3 scripts/rom_to_c.py external/lide.device/lide.rom out/harness-rigel/lide_rom_data.c

# 3. Rebuild harness
cmake --build out/harness-rigel --target harness -j4

# 4. Testar
HARNESS_MSGPORT_OWNER_FIX=1 ./out/harness-rigel/harness src/roms/aros.rom \
  --adf src/disks/aros.adf --iso src/disks/aros-amiga-m68k.iso \
  --frames 6000 --headless

# Sinal de sucesso: "CD0:" no log, sem requester "Insert Live CD"

# 5. Gerar patch para patches/
cd external/lide.device
git add device.c
git commit -m "feat: deferred CD mount task for cdrom-handler timing"
git format-patch HEAD~1 -o ../../patches/
cd ../..

# 6. Adicionar ao setup.sh: nova seção LIDE_PATCHES para external/lide.device
```

## Log

- 2026-06-30: lide.rom vazio identificado e corrigido (bug #1). ATAPI funcional.
  Causa raiz do CD não montar identificada: timing FindCDFS() vs cdrom-handler.
  Três opções de fix documentadas.
- 2026-06-30: Opção A implementada em device.c (tarefa diferida `lide cd defer`).
  Build bloqueado por Docker Desktop corrompido. Restart Windows necessário antes
  de continuar.
