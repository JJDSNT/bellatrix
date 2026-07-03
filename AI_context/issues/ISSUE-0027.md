---
id: ISSUE-0027
title: "Tornar o FAT32 (src/storage/fat) mais robusto — hoje é in-place overwrite only"
status: open
priority: low
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - fat32
  - storage
  - launcher
related_files:
  - src/storage/fat/fat32.c
  - src/storage/fat/fat32.h
---

# Contexto

O leitor/escritor FAT32 do Bellatrix suporta escrita apenas por sobrescrita
in-place (sem criação de arquivos, sem crescimento, sem alocação de clusters
novos). Isso é suficiente para BTPAIRS.TXT/BTKEYS.TXT/BTSCAN.TXT hoje, mas
limita casos futuros (salvar HDFs/configs criados em runtime, logs maiores).

# Objetivo

- Criação de arquivos e diretórios
- Alocação/liberação de clusters (crescimento de arquivo)
- Atualização correta de FAT primária + espelho e FSInfo
- Long File Names na escrita

# Notas

Registrado durante o trabalho de tooling HDF (2026-07-03). Não é o foco
atual — o foco é HDF (tools/hdf + amitools em external/). Retomar após a
etapa de SD card (hdf2emu68 / Emu68-Imager como referências).
