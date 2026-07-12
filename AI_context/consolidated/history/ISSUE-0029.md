---
id: ISSUE-0029
title: "ISO→HDF na ferramenta HDF (tools/hdf) — extrair conteúdo de ISO para HDF"
status: closed
priority: low
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - hdf
  - iso
  - tooling
related_files:
  - tools/hdf/hdf.py
  - src/storage/iso/iso_image.c
  - external/ODFileSystem
---

# Contexto

`tools/hdf/hdf.py` cobre ADF→HDF (via amitools/xdftool). ISO→HDF ficou
fora de escopo no primeiro momento (decisão de 2026-07-03).

# Objetivo

Comando `iso2hdf <disc.iso> <image.hdf> [--dest ...] [--part DH0]`:
extrair a árvore da ISO para host e gravar na partição FFS com xdftool.

# Abordagens candidatas

- `pycdlib` (Python puro, ISO9660/Joliet/RockRidge) — encaixa no modelo
  "sem pip" só se vendorizado em external/
- `7z x disc.iso` ou `xorriso -osirrox` se disponíveis no host
- Reusar nosso leitor ISO (`src/storage/iso` / ODFileSystem host tools)
  compilado como utilitário host

# Cuidados

- Nomes ISO (`;1`, maiúsculas) → preferir Joliet/RockRidge quando presente
- Preservar bits de proteção Amiga não existe em ISO pura; default `rwed`

# Resolução (2026-07-03)

Implementado `iso2hdf` em tools/hdf/hdf.py usando `7z x` (instalado no host)
+ sanitização para FFS: strip de `;1`, truncamento de nomes a 30 chars com
preservação de extensão, e dedupe case-insensitive (RockRidge permite
`Palette.h`/`palette.h` no mesmo dir; FFS não). Validado com
aros-amiga-m68k.iso (336MB) → HDF 400MB DOS3: 655k blocos copiados em ~28s.
O HDF resultante boota o aros.rom até o Workbench Screen; o crash posterior
do Lib & Dev Loader Daemon é Line-F (FPU ausente no Musashi 020/030/040),
não problema do HDF.
