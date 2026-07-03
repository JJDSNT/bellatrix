---
id: ISSUE-0031
title: "Harness Musashi sem FPU: AROS ISO-desktop crasha com Line-F em 020/030; 68040 aborta silencioso"
status: open
priority: medium
type: bug
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - harness
  - musashi
  - fpu
  - aros
  - hdf
related_files:
  - tools/harness/main.c
  - external/musashi
  - patches/0013 (FSAVE/FRESTORE addressing modes)
---

# Sintoma

Com o HDF gerado da aros-amiga-m68k.iso (iso2hdf, ISSUE-0029):

- `HARNESS_CPU=68020`: boota até o Workbench Screen, depois **Software
  Failure — Line 1111 (F) Emulator/Coprocessor error** no task
  "Lib & Dev Loader Daemon" (binário do CD usa instruções FPU).
- `HARNESS_CPU=68030`: mesmo Line-F (vector 0x2c), cai no SAD.
- `HARNESS_CPU=68040`: harness **sai com exit=1 sem mensagem** logo após
  `InitCode leave (0x04)` / setpatch.library — abort silencioso, sem
  "[HARNESS] Done". Investigar separadamente (pode ser trap não tratado
  no backend Musashi 040).

# Objetivo

- Habilitar FPU no Musashi para 020(+68881)/030/040 no harness
  (M68K_EMULATE_FPU / softfloat do Musashi), validando com o boot completo
  do desktop AROS a partir do HDF.
- Root-cause do abort silencioso em 68040.

# Reprodução

```
python3 tools/hdf/hdf.py create aros.hdf 400Mi
python3 tools/hdf/hdf.py iso2hdf src/disks/aros-amiga-m68k.iso aros.hdf
KICKSTART=src/roms/aros.rom HDF=aros.hdf HARNESS_CPU=68020 FRAMES=1500 ./run.sh harness
```

# Progresso (2026-07-03, sessão HDF)

- O crash Line-F/condição-inválida era **red herring de FPU**: o dump ao
  redor do PC (0x00C1C102) mostrou tabela de ponteiros (0x00F8xxxx ROM,
  0x00C1C064 RAM) — a CPU saltou para dados, provável LVO/ABI errado.
- **Causa raiz do crash com aros.rom**: mismatch de versão. `aros.rom` é
  build 30.7.2025; `aros-amiga-m68k.iso` é build 12.5.2026 (mesma data do
  `new_aros.rom`). Nightlies do AROS não têm ABI estável entre builds.
- Com **new_aros.rom + aros_iso.hdf (68040)** boota até o shell de boot e a
  Startup-Sequence roda; para em `FixFonts: Could not open "stdc.library"`
  (a lib ESTÁ no HDF, 382KB — suspeita de falta de RAM: só 2MB chip +
  1.5MB slow; `bellatrix_zorro2_enable_fast_ram()` existe mas só é chamada
  no backend emu68, não no harness). Próximo passo: fast RAM no harness.
- MaxTransfer do RDB ajustado para 0x1FE00 no aros_iso.hdf (não era a
  causa, mas é o valor seguro; considerar default na tool).
- Patch **0014-musashi-fpu-test-condition-mask**: predicados >=0x20
  passam a mascarar para 5 bits com warning em vez de exit(1) do host
  (o abort silencioso do 68040 era esse exit(1) via fatalerror).
