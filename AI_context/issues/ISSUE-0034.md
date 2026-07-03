---
id: ISSUE-0034
title: "FPU completa no Musashi + 68040 como default (harness e bare-metal)"
status: open
priority: high
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - musashi
  - fpu
  - cpu
  - harness
  - baremetal
  - launcher
related_files:
  - external/musashi/m68kfpu.c
  - external/musashi/m68k_in.c
  - src/cpu/musashi/musashi_baremetal_config.h
  - src/cpu/musashi/musashi_backend.c
  - tools/harness/musashi_backend.c
  - tools/launcher/tui.go
---

# Contexto

Sessão de debug ao vivo (2026-07-03) mapeou com precisão o estado real
de suporte a FPU no nosso Musashi, ao investigar crashes "Software
Failure! Line 1111 (F)" durante boots do AROS (ver ISSUE-0031/0033
para a jornada completa — boa parte dos crashes investigados acabou
sendo coisa NÃO relacionada a FPU: bug de memória do `new_aros.rom`,
aliasing Z3, ownership do lide).

Achados que motivam esta issue:

- **68000/010/020 nunca tiveram dispatch de FPU** nesta variante do
  Musashi (`m68k_op_040fpu0_32`/`op1` só entram quando
  `CPU_TYPE_IS_030_PLUS(CPU_TYPE)`, que exclui 020). Não é bug, é
  design — mas significa que qualquer AROS/software com FPU trapeia
  direto nessas CPUs.
- **68040 tem dispatch mas cobertura incompleta**: `fpgen_rm_reg` em
  `m68kfpu.c` cobre FMOVE/FINT/FINTRZ/FSQRT/FABS/FNEG/FSIN/FCOS/
  FSINCOS/FGETEXP/FDIV/FMOD/FADD/FMUL/FSUB/FCMP/FTST — mas ~18
  funções transcendentais/hiperbólicas caem em `default:
  fatalerror(...)` (mata o processo host): FSINH, FCOSH, FTANH,
  FATANH, FASIN, FACOS, FATAN, FTAN, FETOX, FETOXM1, FTWOTOX,
  FTENTOX, FLOGN, FLOGNP1, FLOG10, FLOG2, FGETMAN, FSCALE.
- A AROS é a única coisa que bate nisso hoje: Kickstarts reais
  carregam `mathieeedoubbas.library` incondicionalmente (rede de
  segurança); a AROS (`cpu_detect.S`/`GetAttnFlags`) assume FPU 68040
  nativa via sonda `FSAVE` (que já passamos, patch 0013) e NUNCA
  carrega fallback — qualquer gap depois é fatal, sem rede.
- Achado crítico: o backend Musashi **bare-metal** (que é o backend
  ativo de verdade em hardware hoje, não um fallback — a integração
  Emu68 ainda não fechou) desliga TUDO acima de 68000 via
  `src/cpu/musashi/musashi_baremetal_config.h`
  (`M68K_EMULATE_010/020/030/040` todos OFF).

# Decisão do usuário (2026-07-03)

68040 + FPU devem virar **default em tudo**: harness, backend Musashi
bare-metal, e no launcher Go (`tools/launcher`).

# Plano (ver /home/jaime/.claude/plans/fizzy-cuddling-origami.md para o detalhe completo)

1. `external/musashi/m68kfpu.c` — preencher os ~18 opmodes faltando em
   `fpgen_rm_reg()`, seguindo o padrão já usado por SIN/COS
   (`double_to_fx80(libm_func(fx80_to_double(source)))`). FTWOTOX/
   FTENTOX via `pow()`; FGETMAN/FSCALE precisam de lógica própria
   (padrão do FGETEXP já existente no arquivo).
2. `external/musashi/m68k_in.c` — gate de `040fpu0`/`040fpu1` passa a
   `CPU_TYPE_IS_040_PLUS(CPU_TYPE) || CPU_TYPE_IS_030_PLUS(CPU_TYPE)`
   (mais correto semanticamente, funciona só com M68K_EMULATE_040 on).
3. `src/cpu/musashi/musashi_baremetal_config.h` — liga
   `M68K_EMULATE_040`.
4. Defaults 68000→68040 em `tools/harness/musashi_backend.c`,
   `src/cpu/musashi/musashi_backend.c`, `tools/launcher/tui.go`.
5. Verificação: regressão wb20.hdf; 68000/020 continuam trapando
   corretamente (comportamento esperado, sem dispatch); teste
   funcional novo (binário M68K pequeno exercitando os opmodes novos,
   comparado contra cálculo em Python/C host-side — nenhum crash de
   hoje bateu nos fatalerror que estamos removendo, não há repro
   natural); build limpo do harness + `bellatrix-variant.cmake`
   (BELLATRIX_CPU_BACKEND=musashi); build do launcher Go.

# Fora do escopo (follow-ups separados)

- Crash de memória do `new_aros.rom` (`ir=0xFFFF` em `0x200144`,
  ISSUE-0031) — não relacionado a FPU, provável bug do lado AROS.
- Stall do `aros.rom` headless (PC=0x00fe849a) — separado.
- CPU_TYPE do bare-metal selecionável em build-time via script/launcher
  (hoje o launcher só alimenta `HARNESS_CPU` do harness, não o
  firmware bare-metal, que é compilado com tipo fixo).
