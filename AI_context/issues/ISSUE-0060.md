---
id: ISSUE-0060
title: "Contrato mínimo de vectors.c, fechamento de patches e caminho Z3"
status: doing
priority: critical
type: architecture
owner: agent
created_at: 2026-07-15
updated_at: 2026-07-15
tags: [emu68, vectors, mmio, zorro3, musashi, rigel, patches]
related_files:
  - AI_context/issues/ISSUE-0058.md
  - AI_context/issues/ISSUE-0032.md
  - AI_context/specs/SPEC-0001-cpu-memory-integration.md
  - emu68/src/aarch64/vectors.c
  - emu68/src/aarch64/start.c
  - src/cpu/emu68/bellatrix.c
  - src/cpu/emu68/emu68_direct_region.c
  - src/cpu/musashi/musashi_backend.c
  - src/cpu/cpu_bridge.c
  - src/cpu/direct_region.c
  - src/machine/bus/zorro_autoconfig.c
  - src/machine/bus/zorro3/zorro3.c
  - scripts/setup.sh
---

# Objetivo

Reduzir a integração Emu68 ao menor delta compatível com seu desenho nativo e
definir uma classificação esparsa de memória/MMIO consumida por Emu68, Musashi
e futuros backends. A meta não é uma `machine box`: cada região deve alcançar
diretamente o owner de sua semântica ARM. Core 0 é a baseline provisória de
estabilização; o contrato não pode depender de placement.

# Evidência atual

- O `start.c` original executa boot/JIT no Core 0 e estaciona os secundários.
- `vectors.c` já decodifica Data Abort, chama `SYSReadValFromAddr()` /
  `SYSWriteValToAddr()` e retoma o JIT. Este mecanismo não deve ser substituído.
- O patch `0002` acrescentou um bloco Bellatrix grande e duplicou parte do
  tratamento de boards/Autoconfig que já existe no Emu68.
- O seam desejado é um adapter de plataforma pequeno, estático e selecionado em
  compile time, chamado pelo caminho nativo; não uma tabela de function pointers
  nem um novo loop cooperativo.
- O adapter classifica a região e chama seu owner semântico;
  `BellatrixMachine`/`bellatrix_bus_access()` são pontes transitórias, não ABI.
- Musashi deve compartilhar classificação e owners, sem fabricar Data Abort ARM.
- Rigel é owner dos dispositivos e de seu tempo; não deve virar intermediário
  de RAM `DIRECT`.

# Fechamento de patches Emu68

| Patch | Decisão atual |
|---|---|
| `0001` | manter variant Bellatrix |
| `0002` | manter provisoriamente; extrair/reduzir o adapter e remover duplicação |
| `0003` | separar preservação nativa de IPL do loop da API; fault mode exclui loop gerenciado |
| `0007` | manter para o baseline, auditando cada diferença de startup |
| `0008`–`0010` | manter, sujeitos à auditoria de console/config/Z2 |
| `0019` | manter como hardening independente |
| `0020` | manter STOP nativo no fault mode; excluir branch gerenciada |
| `0022`–`0023` | manter IRQ normal/host-only, sem publicar `INT.ARM` por IRQ de host |
| `0025`–`0034` | retirar do setup de produto; são resíduo da API pública/JIT reescrito |
| `0035` | manter provisoriamente porque o timing atual consome ciclos modelados; auditar separado |

Os arquivos `0025`–`0034` permanecem apenas como histórico. Desde 2026-07-15,
`scripts/setup.sh` não os aplica no fechamento padrão.

# Situação Z3 observada

- `src/machine/bus/zorro3/zorro3.c` existe e Rigel o inicializa.
- O bridge de CPU ainda trata endereços acima de `0x00ffffff` como open bus.
- O caminho Musashi ainda mascara globalmente o endereço para 24 bits.
- Emu68 não fixa base de board: usa o high word atribuído pelo guest e chama
  `map()`. `0x40000000` é política AROS; `0x10000000` era constante sem uso.
- Não há board Z3 Bellatrix funcional de ponta a ponta.
- Boards nativas do Emu68 podem mapear Z3 pelo callback próprio; isso é um
  domínio separado e precisa convergir no lifecycle comum de regiões.
- A janela `$E80000` agora possui um owner compartilhado: apresenta as boards
  Z2 pendentes e depois as Z3, sem transformar o restante do mapa numa machine
  box. A atribuição Z3 por palavra em `$E80044` chama `map()` e só publica a
  board como configurada após sucesso; falha preserva a board pendente.

# Contrato alvo

1. Preservar `SYSHandler` e a semântica de fault/retomada do Emu68.
2. Classificar endereço CPU de 32 bits em `DIRECT`, `EXTERNAL` ou `UNMAPPED`.
3. Mapear `DIRECT` pelo backend (MMU/bank), sem hook no steady state.
4. Encaminhar `EXTERNAL` diretamente ao owner semântico da região, sem alocação
   e sem function pointer no hot path Emu68.
5. Fazer Musashi compartilhar classificação e owners, não uma caixa de máquina.
6. Definir lifecycle transacional `map/unmap/reset/shutup` antes da primeira
   board Z3.
7. Não implementar o hook definitivo antes de fixar descritores, ownership,
   endian/lane semantics e a base/faixa Z3.

# Ordem de trabalho

- [x] Retirar patches `0025`–`0034` da sequência padrão.
- [x] Corrigir detecção cumulativa de `0022`/`0023` no setup.
- [x] Gerar checkout limpo somente com o fechamento atual e comparar o delta.
- [x] Inventariar cada branch Bellatrix em `SYSReadValFromAddr()` e
  `SYSWriteValToAddr()` contra o HEAD original.
- [x] Especificar a primeira matriz esparsa região -> owner na SPEC-0001.
- [x] Fixar semântica de endereço, width, endian e resultados do dispatcher.
- [ ] Materializar assinaturas mínimas após remover a normalização universal.
- [ ] Decompor gradualmente `bellatrix_bus_access()`/`BellatrixMachine` em
  rotas diretas; mantê-los apenas como compatibilidade durante a migração.
- [x] Remover a máscara 24-bit redundante do backend Musashi de produto; o
  `CPU_ADDRESS_MASK` do próprio Musashi continua definindo 24/32 bits por CPU.
- [x] Confirmar no Emu68 que a base Z3 é atribuída pelo guest e entregue a
  `board->map()`, sem janela fixa no host.
- [x] Unificar o owner da janela de Autoconfig Z2/Z3 e reproduzir a atribuição
  Z3 por palavra em `$E80044`.
- [x] Introduzir o primeiro lifecycle transacional Z3 `map/unmap/reset`, com
  rollback de falha, ainda sem definir o mapping específico de cada backend.
- [x] Materializar o registro esparso de regiões `DIRECT` usado somente no
  lifecycle/Musashi, com validação de página, overlap e rollback do backend.
- [ ] Definir descoberta e validação de faixa por perfil, sem promover o mapa
  específico do AROS a contrato universal.
- [x] Provar no harness o contrato `DIRECT` com uma board Z3 ROM mínima/read-only inspirada
  nas boards nativas do Emu68, sem torná-la requisito do perfil de produto.
- [ ] Preservar Fast RAM como Z2 no baseline Emu68; só reconsiderar Z3 RAM se
  surgir um requisito independente de capacidade ou perfil.
- [ ] Medir antes de otimizar o hook; não validar em hardware sem autorização.

# Critérios de aceite

- Um setup limpo reproduz exatamente o fechamento documentado.
- O fault handler e a retomada JIT permanecem semanticamente nativos.
- Endereço CPU não sofre máscara global de 24 bits.
- Emu68 e Musashi compartilham classificação e owners semânticos, sem
  compartilhar detalhes de exceção ARM ou depender de uma machine box.
- Z2 Fast RAM não regride e qualquer board Z3 de prova permanece invisível
  antes de Autoconfig.
- Nenhuma interface pública ou interna fixa Emu68 no Core 0.

# Log

- 2026-07-15: os 12 patches vigentes aplicaram limpos sobre um worktree do HEAD
  original, sem `0025`–`0034`.
- 2026-07-15: `0002` foi reduzido de um bloco Bellatrix de 174 linhas em
  `vectors.c` para um include de adapter mantido no Bellatrix. O adapter é
  compilado na mesma translation unit, preservando o call shape nativo e sem
  function pointers.
- 2026-07-15: direção corrigida para arquitetura esparsa. `BellatrixMachine` e
  `bellatrix_bus_access()` são pontes de migração, não o contrato final.
- 2026-07-15: removida a máscara global adicional do adapter Musashi. CPUs
  24-bit continuam mascaradas no core Musashi; 68020+/68040 chegam ao bridge
  com 32 bits e recebem open bus enquanto Z3 não estiver implementada.
- 2026-07-15: AROS foi inicialmente usado para concluir uma faixa fixa, mas a
  revisão pelo Emu68 corrigiu essa interpretação. Emu68 aceita a base escrita
  pelo guest em `$E80044` e a board faz `mmu_map()` diretamente. AROS permanece
  apenas como caso de compatibilidade. `BELLATRIX_Z3_BASE=0x10000000` e
  `s_next_base` continuam removidos por não participarem desse mecanismo.
- 2026-07-15: a janela baixa de Autoconfig passou a ter um sequenciador comum
  que mantém Z2 primeiro e apresenta Z3 em seguida. O contrato Z3 agora usa a
  palavra escrita em `$E80044`, chama `map()` fora do hot path e desfaz mapping
  em reset/remoção. O teste de contrato cobre ordem, base guest-assigned,
  rollback de `map()` e `unmap()`; ainda não existe uma board Z3 Bellatrix
  funcional de ponta a ponta.
- 2026-07-15: a auditoria do primeiro `DIRECT` separou reserva de backing de
  mapping guest. Z3 RAM deve retirar backing do topo de `sys_memory` (e refletir
  a redução no device tree) antes de instalar MMU/bank. O heap TLSF local não é
  apropriado para dezenas de MiB, e o allocator de page tables precisa dividir
  o mesmo topo de forma ordenada.
- 2026-07-15: prioridade corrigida após rever as boards nativas. O Emu68 usa
  Z2 para Fast RAM; suas boards Z3 são ROMs read-only mapeadas diretamente.
  Portanto Z3 Fast RAM deixou de ser objetivo presumido. A primeira prova Z3
  deve ser uma ROM mínima e descartável, usada para validar o contrato sem
  decidir a composição final do produto.
- 2026-07-15: criado `cpu/direct_region` como contrato neutro de lifecycle.
  Emu68 o usará somente para instalar/remover MMU; seu steady state não consulta
  a tabela. Musashi poderá consultá-la nos callbacks. O teste cobre ROM
  read-only, alinhamento, overlap, fronteira e rollback map/unmap.
- 2026-07-15: escolhida `emu68/src/boards/68040.c` como referência da prova no
  harness POSIX. Ela é uma ROM Z3 de 4 KiB, read-only/executable, em janela de
  64 KiB e não depende de FDT ou de serviços AArch64. A prova não copia o
  conteúdo real, não substitui os adapters MMU/Musashi e não habilita uma board
  no produto. `devicetree.c` permanece referência futura para múltiplas regiões
  no backend Emu68, não para o harness.
- 2026-07-15: o contrato foi ligado aos backends. Emu68 traduz install em
  `mmu_map`; remove restaura um mapeamento sem `MMU_ALLOW_EL0`, devolvendo acessos
  68k ao Data Abort nativo sem depender do `mmu_unmap()` upstream, que é um stub.
  Nenhuma lookup foi adicionada a `vectors.c` ou ao fault path. Musashi e o
  harness consultam o bank esparso antes do fallback/open bus e preservam
  endian big-endian e ROM read-only.
  Ainda não há board Z3 registrada no perfil de produto.
