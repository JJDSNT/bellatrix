---
id: ISSUE-0032
title: "Contrato Zorro III 32-bit: Autoconfig e mapeamento direto por backend"
status: open
priority: high
type: research
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-15
tags:
  - harness
  - zorro3
  - superbuster
  - memory
  - musashi
  - emu68
related_files:
  - AI_context/specs/SPEC-0001-cpu-memory-integration.md
  - tools/harness/musashi_backend.c
  - src/machine/bus/superbuster/superbuster.c
  - src/machine/bus/zorro3/zorro3.c
  - src/machine/machine_rigel_bus.c
  - src/cpu/cpu_bridge.c
  - src/cpu/emu68/emu68_machine_platform.c
  - emu68/src/aarch64/vectors.c
  - AI_context/consolidated/memory_model.md
---

# Rebaseline arquitetural (2026-07-15)

Esta issue não significa que Bellatrix já possua suporte Z3. Existem peças de
state machine, Super Buster e dispatch, porém elas ainda são incompletas e
inconsistentes. A orientação registrada em ISSUE-0058 redefine o objetivo:

- Autoconfig continua sendo uma transação externa pela janela baixa `$E80000`;
- ao atribuir a base, uma board RAM/ROM/VRAM instala uma região direta no
  backend em vez de manter callback por acesso;
- registradores com efeitos colaterais usam política explícita por páginas da
  board e convergem no mesmo serviço externo usado pelo chipset;
- Emu68 implementa região direta com `mmu_map()`; Musashi implementa a mesma
  região na sua tabela/bank de memória, sem fabricar um fault ARM;
- Rigel recebe somente acessos externos/MMIO e continua dono do estado dos
  dispositivos, não da Fast RAM acessada diretamente.

O Emu68 original demonstra esse mecanismo: a conclusão de Autoconfig chama a
callback `map` da board, e as boards Z3 existentes chamam `mmu_map()`. O fault
handler acima de 24 bits é fallback para acesso não mapeado, não datapath Z3.

# Contexto anterior ainda válido

O harness hoje só tem chip 2MB + slow 1.5MB + Z2 fast 8MB (ISSUE-0031).
A infra Z3 **existe apenas em parte**: `src/machine/bus/superbuster/` e
`src/machine/bus/zorro3/` (registro de boards, config read/write
despachado em machine_rigel_bus.c) — o Super Buster foi introduzido
justamente para permitir os dois cenários (Z2 e Z3) na mesma máquina.

# Bloqueios atuais

- `memory_map_decode()` e backends possuem normalização global de 24 bits;
- `cpu_bridge.c` classifica todo endereço de 32 bits como Z3 não implementada;
- `Z3_WINDOW_BASE=0x40000000` conflita com `BELLATRIX_Z3_BASE=0x10000000`;
- a conclusão da Autoconfig não possui callback comum de map/unmap por backend;
- callbacks byte a byte atuais não distinguem memória direta de registradores;
- ainda não existe board Z3 funcional registrada no runtime Bellatrix.

# Objetivo revisado

1. Definir dois namespaces de acesso: barramento Amiga baixo com normalização
   própria e endereço CPU 32-bit sem truncamento.
2. Definir lifecycle de board (`configure`, `map`, `unmap`, `reset`) e regiões
   declarativas (`DIRECT`, `EXTERNAL`, `UNMAPPED`).
3. Fazer Autoconfig atribuir a base e pedir ao backend que instale as regiões.
4. Implementar primeiro uma board Z3 Fast RAM direta; nenhum dispatch Rigel por
   byte no steady state.
5. Espelhar a semântica no Musashi com bank/buffer 32-bit e no Emu68 com MMU.
6. Só então implementar regiões mistas de RTG/VRAM e registradores.
7. Validar futuramente KS3.1/AROS e cenário Z2+Z3; 68000 continua 24-bit.

# Restrições

- Não remover indiscriminadamente toda máscara de 24 bits: o wrap continua
  correto em perfis e transações específicas; deve apenas sair do decoder
  universal.
- Não rotear toda Z3 por `vectors.c` ou Rigel. Apenas páginas externas/MMIO.
- Não chamar a infraestrutura atual de suporte Z3 antes de haver ao menos uma
  board registrada, configurada e mapeada de ponta a ponta.
- Preset alvo (memory_model.md): AROS "moderno" = chip 2MB + Z3 128MB
  (+ RTG, ver [[ISSUE-0033]])
