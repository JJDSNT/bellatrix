---
id: ISSUE-0030
title: "SD card stage: lidar com layout Emu68 — RDB dentro de partição MBR"
status: open
priority: medium
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-25
tags:
  - hdf
  - sdcard
  - emu68
  - tooling
related_files:
  - tools/hdf/hdf.py
  - src/storage/sdcard/bcm_emmc.c
  - src/machine/expansions/lide_cdrom/lide_cdrom.c
  - src/machine/expansions/lide_cdrom/ata_ide.c
  - emu68/src/boards/sdcard.c
---

# Contexto

No arranjo do Emu68/PiStorm o disco Amiga não é um arquivo .hdf na FAT32:
o SD tem tabela MBR e o RDB fica **dentro de uma partição MBR** (tipo 0x76,
convenção PiStorm), que o Emu68 expõe ao lado Amiga como unidade de disco.
É esse arranjo (MBR → partição 0x76 → RDB → partições Amiga) que vamos
precisar suportar na próxima etapa (SD card) do Bellatrix.

# Objetivo

- `tools/hdf`: analisar e construir imagens SD completas — MBR com FAT32
  (boot Emu68) + partição 0x76 contendo RDB; `analyze` deve enxergar o RDB
  com offset da partição MBR
- Definir como o Bellatrix expõe essa partição ao lado Amiga (equivalente
  ao brcm-sdhc.device do Emu68 vs nosso lide.device)

# Referências

- https://github.com/PiStorm/hdf2emu68 — gera .img com esse layout a partir
  de um HDF
- https://mja65.github.io/Emu68-Imager/ — imager completo de SD
- Depende de HDF via lide funcionando primeiro (ver ISSUE-0026)

# Auditoria de arquitetura — 2026-07-25

Há três caminhos independentes que não devem ser confundidos:

1. `src/storage/sdcard/bcm_emmc.c` + FAT32: código ARM usado historicamente
   pelo launcher para arquivos ADF e ainda usado pela persistência de
   Bluetooth (`BTPAIRS.TXT`, `BTKEYS.TXT`, `BTSCAN.TXT`).
2. `brcm-sdhc.device`: ROM m68k da board Z3 nativa do Emu68; acessa diretamente
   a janela de periféricos do Raspberry Pi e reconhece partições MBR `0x76`.
3. `lide.device`: ROM m68k da board Z2 RIPPLE; fala com o ATA emulado pelo
   Bellatrix, cujo backend é uma interface de setores.

## Estado do caminho Z3 com Musashi

A infraestrutura DIRECT compartilhada já tem a arquitetura correta:

- Emu68 instala regiões no MMU;
- Musashi consulta seu mapa DIRECT antes da bridge;
- acessos fora das regiões conhecidas chegam à bridge/open bus.

Entretanto, a integração concreta da board SDHC ainda precisa ser validada e
completada:

- `emu68/src/boards/sdcard.c` é compilado nas imagens Musashi, mas registra seu
  descritor somente em `.boards.z3`;
- o perfil Musashi/legacy percorre hoje a seção `bellatrix_boards`;
- o `map()` da board chama `mmu_map()` diretamente, em vez de
  `cpu_backend_map_direct()`;
- o Emu68 expõe os periféricos do Pi ao m68k a partir de `0xF2000000` via MMU;
  no Musashi, endereços m68k são resolvidos pelos callbacks de memória e essa
  janela ainda não está representada como uma região DEVICE do backend.

Isso não invalida o modelo Z3. A regra alvo continua sendo: qualquer board Z3
compatível com a plataforma deve funcionar com Emu68 ou Musashi, single-core
ou multicore. Boards são filtradas por capacidades da plataforma/máquina:
SDHC exige Raspberry Pi + controlador SDHC + Z3; a board de suporte 68040 não
faz sentido quando o próprio Musashi já fornece um 68040; boards físicas do Pi
não entram no harness.

## Alternativa inicial em avaliação: lide.device sobre o SD inteiro

Em vez de entregar o controlador SD físico ao `brcm-sdhc.device`, o Bellatrix
pode manter o controlador sob posse do ARM e apresentar o SD inteiro como o
disco ATA master do `lide.device`:

```text
Amiga/lide.device
  -> board Z2 RIPPLE
  -> ATA emulado
  -> backend ARM de setores
  -> bcm_emmc
  -> MBR físico
  -> partição 0x76
  -> RDB
```

O mounter do LIDE deve reconhecer `0x76`, procurar o RDB relativamente ao
início do contêiner e incorporar esse deslocamento aos `DosEnvec` publicados.
Não se criam unidades ATA por partição: uma unidade representa o disco e o
mounter publica todos os volumes encontrados. Como o trackdisk clássico
expressa limites em cilindros, o início de `0x76` deve estar alinhado à
geometria das partições RDB.

### Por que pode ser o melhor primeiro passo

- reutiliza o `lide.device` e o ATA já exercitados no harness com HDF/RDB;
- funciona através do mecanismo Z2 compartilhado nos dois backends;
- mantém FAT/Bluetooth e disco Amiga sob um único dono ARM;
- permite implementar lock, serialização e política de acesso no mesmo
  backend `bcm_emmc`, sem coordenar um driver ARM com um driver m68k que toca o
  controlador diretamente;
- mantém o mesmo layout físico MBR + FAT32 + `0x76` esperado pelo caminho
  futuro do `brcm-sdhc.device`.

### Custos e limites aceitos para a etapa inicial

- ATA emulado e Z2 são mais lentos;
- cada acesso passa pelo datapath da board EXTERNAL e pelo backend de setores;
- o suporte ATA atual é LBA28;
- ainda é necessária arbitragem no ARM: Bluetooth não pode iniciar uma
  operação FAT no meio de uma transação ATA. A vantagem é haver um único ponto
  onde aplicar essa arbitragem;
- o caminho não valida `brcm-sdhc.device`; ele apenas permite avançar RDB/boot
  no hardware enquanto a integração Z3/DIRECT/DEVICE é estudada.

## Decisão provisória

Manter as duas opções documentadas. A primeira implementação do mounter LIDE
para MBR `0x76` está em `patches/0038-lide-mbr-rdb-container.patch`; o próximo
passo é conectar o SD inteiro ao backend ATA e validar a montagem em hardware.
Não descartar nem substituir a board Z3 do Emu68: ela permanece o caminho
direto e potencialmente mais rápido a ser validado posteriormente.
