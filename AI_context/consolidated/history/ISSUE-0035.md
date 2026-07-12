---
id: ISSUE-0035
title: "Build bare-metal (BELLATRIX_CPU_BACKEND=musashi) falha no link: getenv/strtoul indisponíveis"
status: resolved
priority: medium
type: bug
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - baremetal
  - build
  - musashi
  - lide
related_files:
  - src/machine/expansions/lide_cdrom/ata_ide.c
  - src/machine/expansions/lide_cdrom/atapi_cdrom.c
  - external/rigel
---

# Sintoma

Rodando um build bare-metal completo pela primeira vez nesta sessão
(`BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=1
BELLATRIX_USBSTACK=1 ./scripts/build.sh`, ao verificar ISSUE-0034), o
link final falha:

```
undefined reference to `getenv'
undefined reference to `strtoul'
```

Em `ata_ide.c`, `atapi_cdrom.c` (chamadas `getenv("HARNESS_CD_TRACE")`,
`getenv("HARNESS_CD_BOOTABLE")` — nomeadas "HARNESS_", claramente
pensadas só para debug no harness) e em `external/rigel`
(`blitter_ref.c`/`blitter_command.c`).

Ambiente bare-metal (firmware Raspberry Pi, sem libc completa/sem
env vars) não tem `getenv`/`strtoul` disponíveis — essas chamadas
precisam ficar atrás de `#ifdef BELLATRIX_HARNESS` (ou equivalente) ou
o bare-metal precisa de stubs.

# Confirmado não-relacionado

Achado ao verificar ISSUE-0034 (FPU no Musashi) — as próprias mudanças
de FPU/CPU_TYPE COMPILAM limpo (confirmado após corrigir um
`#include <stdio.h>` faltando em `m68kcpu.h`, ver patch 0015). Esse
erro de link é anterior, em código não tocado nesta sessão.

# Objetivo

- `ata_ide.c`/`atapi_cdrom.c`: guardar as chamadas `getenv("HARNESS_*")`
  atrás de `#if defined(BELLATRIX_HARNESS)` (padrão já usado em outros
  arquivos do projeto).
- `external/rigel` (blitter): investigar uso de `getenv`/`strtoul` —
  submódulo, precisa de patch se for código deles, ou é nosso glue.
- Validar com build completo `BELLATRIX_CPU_BACKEND=musashi` até gerar
  a imagem final.
