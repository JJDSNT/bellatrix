---
id: ISSUE-0025
title: "Suporte a HD (RDB): SD card como disco rígido; primeiro passo — HDF no harness"
status: open
priority: high
type: feature
owner: unassigned
created_at: 2026-07-02
updated_at: 2026-07-02
tags:
  - hd
  - hdf
  - rdb
  - sdcard
  - lide.device
  - harness
related_files:
  - tools/harness/main.c
  - src/machine/expansions/lide_cdrom/ata_ide.c
  - src/machine/expansions/lide_cdrom/atapi_cdrom.c
  - src/host/posix/pal_posix.c
  - src/storage/sdcard/bcm_emmc.c
---

# Objetivo

Suporte a disco rígido com RDB (Rigid Disk Block), para no fim usar o SD card
(ou um arquivo nele) como HD do Amiga no hardware real.

## Fase 1 — HDF no harness (escopo inicial desta issue)

Adicionar ao harness Musashi suporte a imagem HDF:

- Flag de linha de comando (ex.: `--hdf <arquivo.hdf>`) análoga a `--adf` /
  `--iso`.
- Expor o HDF como device ATA (não ATAPI) na interface IDE já existente do
  lide.device — hoje `ata_ide.c` só atende o caminho ATAPI/CD; falta o
  conjunto de comandos ATA de disco (IDENTIFY DEVICE, READ/WRITE SECTORS,
  geometria CHS/LBA derivada do tamanho do arquivo).
- HDF "plain" (imagem crua com RDB no início) é o alvo; o RDB em si é lido e
  interpretado pelo lide.device/OS, não por nós — nosso trabalho é servir os
  setores fielmente e suportar escrita.
- Critério de aceite: bootar KS3.1 (ou montar como volume secundário num boot
  por ADF), formatar/partitionar via HDToolBox contra o HDF, ler e escrever
  arquivos com persistência entre execuções.

## Fase 2 — hardware (fora do escopo inicial, registrar direção)

- No hardware NÃO haverá HDF: o SD card terá uma **partição RDB dedicada**
  (tipo MBR `0x76`, convenção Emu68) que funciona diretamente como o HD do
  Amiga.
- **Caminho preferido: brcm-sdhc.device nativo do Emu68.** O Emu68 já traz
  uma placa Zorro III de ROM (`emu68/src/boards/sdcard.c`) com o
  `brcm-sdhc.device` (Emu68-tools) embutido — código 100% m68k, autoconfig,
  fala direto com o EMMC do Pi e expõe partições MBR tipo 0x76 como discos
  RDB bootáveis. Zero emulação por setor, zero código novo do nosso lado.
- **Trabalho real da Fase 2 = arbitração do EMMC.** O Bellatrix também usa o
  EMMC do lado ARM (`bcm_emmc.c`: launcher, FAT32, BTPAIRS/BTKEYS). Regra a
  garantir: ARM só toca o SD antes do boot do Kickstart (launcher); depois o
  controlador é do driver m68k. No multicore, definir qual core pode acessar
  o EMMC.
- Alternativa (descartada por ora): usar o mesmo lide/ATA emulado da Fase 1
  no hardware com backend na partição RDB — unificaria harness e hardware,
  mas com overhead de bus-trap por acesso e código redundante frente ao
  driver nativo.
- Consequência aceita: harness (lide/ATA + HDF) e hardware (brcm-sdhc +
  partição) usam stacks diferentes; a Fase 1 vale como teste do lado
  OS/filesystem/RDB, não do driver do hardware.

# Progresso 2026-07-02 (Fase 1 implementada)

- `--hdf` no harness (`tools/harness/main.c`): arquivo aberto r+b, setores
  servidos on demand, escrita persistente (fflush por WRITE).
- `ata_ide.c/.h`: canal virou bus de 2 devices — **HD = master (dev 0),
  CD = slave (dev 1)**, seleção pelo bit 4 do dev_head. Sem disco, mantém o
  comportamento legado (ATAPI responde independente da seleção). Comandos
  ATA: IDENTIFY (0xEC, multiple=0 → lide usa READ/WRITE 0x20/0x30 simples),
  READ/WRITE SECTORS com LBA28, SET FEATURES. Estados novos ATA_DATA_IN/OUT.
- `lide_cdrom.c/.h`: API `lide_hd_attach(machine, read_fn, write_fn, ctx,
  sectors)`.
- **Validado (KS2.0 + wb31.adf headless)**: bootldr detecta o disco, IDENTIFY
  ok, lê e aceita RDSK+PART. Script `make_rdb.py` (scratchpad da sessão)
  gera HDF com RDB embrulhando uma imagem de partição DOS\0 (testado com
  AW.hdf → DH0).

## Pendente

- Nenhuma leitura de filesystem após o parse do RDB no KS2.0 headless —
  provavelmente o stall do KS20 ([[ISSUE-0023]]) ou wb31.adf incompatível
  com KS2.0. Validar interativo + HDToolBox (critério de aceite).
- ~~KS3.1 não roda o bootldr~~: FALSO ALARME — o KS3.1 precisa de 68020
  (`--cpu 68020` no harness / cpuBackend musashi-68020 na TUI). Com 68020,
  bootldr roda, disco é detectado e RDSK+PART são lidos, igual ao KS2.0.
  Em 68000 o lide simplesmente não inicializa nesse ROM.
- ~~TUI Go do harness~~: FEITO (2026-07-02) — seção "Hard disk (HDF)" no
  `tools/launcher` (Tab KS→ADF→ISO→HDF), linha `HDF=` no output, e `run.sh`
  passa `--hdf` nos modos harness/harness-serial (env `HDF=<file>` também
  funciona direto). Atenção: AW.hdf/Arka.hdf em src/disks são partition-only
  (sem RDB) — o lide não vai achar RDSK nelas; usar HDF com RDB
  (make_rdb.py as embrulha).
- Launcher bare-metal (hardware): seleção de HDF fica para a Fase 2.
- **KS1.3 sem HD na placa Z3**: Zorro III exige expansion.library V36+;
  KS1.3 não enumera Z3, então o brcm-sdhc não serve para KS1.3. Se KS1.3+HD
  for alvo, o caminho é o lide (Z2) com nosso ATA emulado também no hardware.

# Notas

- Reaproveitar a infra do CD ([[ISSUE-0023]]): mesmo controlador IDE, o HD
  entra como master/slave ao lado do CD.
- Decidir cedo se HD e CD coexistem na mesma porta (master=HD, slave=CD) —
  é o arranjo que o hardware real vai querer.
