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
- a antiga constante sem uso `BELLATRIX_Z3_BASE=0x10000000` foi removida;
  `0x40000000..0x7fffffff` é política observada no AROS, não requisito Emu68;
  descoberta e faixa válida precisam ser definidas pelo contrato de perfil;
- o callback comum de lifecycle Z3 `map/unmap` existe, mas ainda não há
  implementação de mapping `DIRECT` específica para Emu68 ou Musashi;
- callbacks byte a byte atuais não distinguem memória direta de registradores;
- ainda não existe board Z3 funcional registrada no runtime Bellatrix.

# Avanço de infraestrutura (2026-07-15)

- `$E80000` agora é atendido por um owner comum que apresenta Z2 antes de Z3;
- a atribuição Z3 segue o Emu68: palavra em `$E80044`, base `value << 16`;
- `map()` é chamado antes de publicar a board; falha faz rollback e a mantém
  pendente;
- reset, re-registro e remoção chamam `unmap()`;
- teste host cobre a cadeia Z2→Z3, base escolhida pelo guest, rollback e reset.
- o registro `cpu/direct_region` valida mapping page-aligned, impede overlap e
  só publica a região após sucesso do backend; falhas de unmap preservam estado.
- a prova host modela a board `68040` nativa como ROM Z3 read-only/executável e
  verifica rollback, invisibilidade antes/depois e base escolhida pelo guest.

Isso fecha o sequenciamento e o esqueleto de lifecycle, mas não constitui
suporte Z3: falta a board ROM de prova e o mapping direto nos backends.

A auditoria de uma eventual board RAM também confirmou que a identidade física
criada por `start.c` não concede `MMU_ALLOW_EL0`. Isso permite manter a reserva
acessível ao ARM e invisível ao 68k até Autoconfig; o `map()` da board instala
a tradução guest com permissão EL0. Musashi deve registrar o mesmo buffer como região
direta. A API pública antiga já contém uma primitiva MMU semelhante, mas ela
deve ser extraída para uma fronteira esparsa, não reutilizada como machine box.
Esse trabalho de reserva não bloqueia a prova ROM atual; só volta à prioridade
se uma Z3 RAM for escolhida explicitamente.

# Objetivo revisado

1. Definir dois namespaces de acesso: barramento Amiga baixo com normalização
   própria e endereço CPU 32-bit sem truncamento.
2. Definir lifecycle de board (`configure`, `map`, `unmap`, `reset`) e regiões
   declarativas (`DIRECT`, `EXTERNAL`, `UNMAPPED`).
3. Fazer Autoconfig atribuir a base e pedir ao backend que instale as regiões.
4. Implementar primeiro uma board Z3 ROM mínima/read-only como prova do mapping
   direto observado no Emu68; nenhum dispatch Rigel por byte no steady state.
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
- Fast RAM do baseline permanece Z2, como em `emu68/src/boards/z2ram.c`.
- O antigo preset AROS com Z3 128 MiB é cenário de pesquisa, não arquitetura
  escolhida nem requisito para fechar esta issue.

# Escolha do primeiro caso `DIRECT`

As referências nativas do Emu68 separam claramente os casos:

- `z2ram.c`: Fast RAM Z2, mapping identidade, read/write;
- `sdcard.c`, `68040.c`, `emmc.c` e `unicam.c`: ROM Z3 read-only;
- `devicetree.c`: ROM Z3 e dados adicionais, também read-only.

Assim, a primeira prova Bellatrix usa `68040.c` como referência porque o harness
é POSIX e executa Musashi 68040: uma ROM Z3 simples não depende da plataforma
AArch64. Ela prova Autoconfig, base escolhida pelo guest, `map/unmap`, rollback e
permissão read-only sem copiar o conteúdo ou habilitar a board no produto.
`devicetree.c` permanece referência para uma futura prova multi-região no Emu68.
Não se deve reservar uma grande região de `sys_memory` antes de existir decisão
explícita por Z3 RAM.
