---
id: ISSUE-0030
title: "SD card stage: lidar com layout Emu68 — RDB dentro de partição MBR"
status: open
priority: medium
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - hdf
  - sdcard
  - emu68
  - tooling
related_files:
  - tools/hdf/hdf.py
  - src/storage/sdcard/bcm_emmc.c
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
