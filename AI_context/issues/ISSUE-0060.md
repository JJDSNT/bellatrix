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
blockers:
  - "medição antes de otimizar o hook de vectors.c — não validar em hardware sem autorização"
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
| `0003` | **CORRIGIDO (ISSUE-0061, 2026-07-16):** esta linha descrevia originalmente "fault mode exclui loop gerenciado" — essa é exatamente a causa raiz da regressão de boot documentada na ISSUE-0061. O relatório de progresso do chipset (`bellatrix_emu68_report_jit_progress()`) deve ser **incondicional** no `MainLoop`, nunca excluído por `BELLATRIX_EMU68_FAULT_DRIVEN` ou qualquer outra flag de modo de roteamento. O patch `0003` atual reflete isso; ver `docs/fault_handler.md` para a distinção roteamento-vs-sincronização. |
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
- [x] Materializar a **decisão de roteamento** de 32 bits como classificação
  explícita: `cpu_bridge_classify()` (`AMIGA_LOW`/`Z3_EXTERNAL`/`OPEN_BUS`)
  substitui o antigo `addr_is_unmapped_32bit()` cru. A normalização de 24 bits
  do domínio baixo (`bellatrix_bridge_normalize_addr`) segue local ao AMIGA_LOW;
  removê-la do fast path multicore é o resíduo deste item.
- [~] Decompor gradualmente `bellatrix_bus_access()`/`BellatrixMachine` em
  rotas diretas. Primeiro corte feito para o espaço Z3: o **Super Buster** é
  agora o owner de decode do espaço de 32 bits (`superbuster_decode_z3()`), e o
  bridge roteia board Z3 EXTERNAL ao dono com endereço completo em vez de
  open-bus cego. As rotas do domínio baixo (custom/CIA/autoconfig) seguem via
  `machine_dispatch_*` como compatibilidade durante a migração.
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
- [x] Registrar a ROM `68040` nativa pelo lifecycle Bellatrix e validar no QEMU
  com Emu68 e Musashi 68040, sem alterar o fault handler original.
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
- 2026-07-15: a ROM nativa `68040` passou a ser a primeira board Z3 real do
  perfil, habilitada automaticamente para Emu68 e Musashi 68040. KS3.1 no QEMU
  atribuiu `$40000000` em ambos. Emu68 instalou a página pelo MMU; Musashi pelo
  bank `DIRECT`. A prova também revelou e corrigiu um toggle falso: o define
  fault-driven fazia o build Musashi entrar em `M68K_StartEmu()`. Agora Musashi
  possui o loop e não emite `[JIT]`/`[EMU68-LIVE]`. O console `$DEADBEEF` usado
  pela DiagArea preserva a semântica original de `vectors.c` via bridge apenas
  no backend Musashi; o handler Emu68 não foi modificado.
- 2026-07-17: **caminho Z3 EXTERNAL ligado, com o Super Buster como owner do
  decode** (o chip que arbitra o Zorro III no HW real). Antes, o bridge tratava
  todo endereço `> 0x00FFFFFF` como open-bus, então uma board Z3 cujos
  registradores são servidos por callback (EXTERNAL, não DIRECT/MMU) era
  inalcançável e o `bellatrix_zorro3_board_read8/write8` estava morto.
  Mudanças: (a) `superbuster_decode_z3()` classifica o espaço de 32 bits contra
  os slots Z3, com gate no `NBSTAB` (bus Z3 disponível); (b) o bridge ganhou
  `cpu_bridge_classify()` — `AMIGA_LOW`/`Z3_EXTERNAL`/`OPEN_BUS` — e roteia a
  board EXTERNAL ao dono com o **endereço completo**, sob o lock do chipset,
  sem mascarar para 24 bits (o mascaramento aliasava para chip RAM, bug
  não-determinístico de 2026-07-03); (c) `bellatrix_machine_z3_external_owns()`
  é a ponte fina cpu->machine (o bridge não inclui o Super Buster direto).
  **Shared vs específico:** a classificação/roteamento é compartilhada pelos dois
  backends via bridge; DIRECT (RAM/ROM) segue consumido antes, específico de
  cada backend (Emu68 MMU / Musashi bank). Novo teste host
  `bellatrix_unit_superbuster_z3` prova a cadeia Autoconfig->base->map->decode->
  read/write EXTERNAL + gate NBSTAB + teardown no reset. Sem regressão: os 4
  oráculos de ROM (KS1.3/KS2.0/KS3.1/AROS) + smoke/boot_adf/no_autoconfig
  passam. Como nenhuma board Z3 EXTERNAL é registrada no produto por padrão, o
  comportamento do perfil atual é idêntico (o novo caminho só ativa com uma
  board EXTERNAL registrada). Resíduo: (1) tirar a normalização de 24 bits do
  fast path multicore; (2) descoberta/validação de faixa Z3 por perfil — agora o
  Super Buster é o lar natural dela.
- 2026-07-17: **mecanismo de board auto-registrável estilo Emu68** (fundação da
  convergência; a abordagem Emu68 é o ALVO, não legacy — o legacy é o memory-map
  hardcoded da TUI do run.sh). Novo `src/machine/bus/board_registry.{h,c}`:
  `BellatrixBoard` é layout-compatível com `struct ExpansionBoard` do Emu68
  (rom_file, rom_size, map_base, is_z3, enabled, map()); a macro
  `BELLATRIX_REGISTER_BOARD_Z2/Z3` dropa o descriptor numa seção de linker
  ("dropar um .c e pronto", sem register_board() central); o walker de autoconfig
  espelha o `vectors.c` (config read do rom_file; base em 0x44/0x48 dispara
  `map()`; shutup 0x4C/0x4E pula). **Achados empíricos** (provados com gcc host):
  seção com nome de identificador-C dá `__start_/__stop_` automáticos sem linker
  script; funciona em qualquer ELF (host + AArch64). **Regra de disciplina:**
  boards têm que ser objetos DIRETOS no link — dentro de `.a` o membro é
  descartado (self-registration perdida, símbolos de fronteira somem). Harness e
  produto já linkam board direto, então ok. **O harness TESTA o mecanismo de
  produção:** novo `bellatrix_unit_board_registry` dropa 2 boards de teste e prova
  descoberta->autoconfig->map->acesso de ponta a ponta. Lado produto (a fazer na
  migração): backend Emu68 pode reusar as seções nativas `.boards.z2/.z3` + o
  walker do próprio `vectors.c` (fidelidade máxima); backend Musashi usa este
  walker. Nada foi ligado ainda ao bus vivo — é a fundação.
- 2026-07-17 (cont.): convergência afinada + limpeza. `board_registry` passou a
  incluir `emu68/include/boards.h` e caminhar o `struct ExpansionBoard` REAL do
  Emu68 (sem espelho `BellatrixBoard` a manter em sincronia; Jaime OK com header
  do Emu68 no harness por código mais limpo). **Deletado** o wrapper `z3_68040`
  (reexpunha o ROM 68040 nativo do Emu68, OFF por padrão, redundante com a board
  nativa) — fonte, registro guardado em `bellatrix.c` e opção/bloco CMake.
  **Inventário de boards (alvo é sempre o Emu68):** nativas do Emu68 (z2ram,
  68040, sdcard, emmc, unicam, devicetree, VideoCore.card=RTG) — mantidas, nunca
  wrappar; `rtg` nosso é lab que NUNCA funcionou (Emu68 tem VideoCore.card) —
  deleção adiada (raio grande: harness main/HARNESS_RTG, screenshot.c,
  rtg_rom_data via Docker, cards/bellatrix.card); **lide** (external/lide.device)
  é a ÚNICA board que o Emu68 não dá — suporte a **ISO e HDF**, board mista
  (ROM + registradores ATA/IDE por acesso via `expansion.c` bus_ops). **É o único
  motivo de a abordagem antiga (`expansion.c` + registries Zorro2/3) continuar
  viva** — documentado no topo de `src/machine/expansion.h`. `expansion.c` NÃO é
  órfão (usado por machine_rigel*, lide, plugin_loader). Aposentar a via antiga
  espera o lide ser reexpresso como board DIRECT estilo Emu68 + janela EXTERNAL
  (via Super Buster). Legacy real a retirar = memory-map hardcoded da TUI do
  run.sh. Verificação: suíte host completa verde (18/18 que rodam, incl. 4
  oráculos de ROM); o board_registry é provado por `bellatrix_unit_board_registry`;
  ainda não há boot de guest enumerando board EXTERNAL (não há board EXTERNAL no
  produto por padrão).
- 2026-07-17 (passo 1 da conclusão Z3): **board_registry LIGADO ao bus vivo**
  (branch `z3-board-registry-live`). O AutoConfig `$E80000` da máquina
  (`machine_rigel_bus.c`) passou a ser respondido pelo walker do
  `board_registry` (`bellatrix_boards_autoconfig_*`) em vez do sequenciador
  zorro2/zorro3. O **Z2 Fast RAM** foi migrado de `bellatrix_zorro2_register_board`
  para uma `struct ExpansionBoard` auto-registrável (`BELLATRIX_REGISTER_BOARD_Z2`);
  seu `map()` instala a região DIRECT usando globais (`bellatrix_machine_memory()`,
  `cpu_backend_selected()`), à la z2ram.c do Emu68. Call sites viraram
  `bellatrix_z2_fast_ram_configure(size)`. Reset da máquina faz teardown genérico
  das regiões DIRECT dos boards (ExpansionBoard não tem unmap; removível por
  `map_base`/`rom_size` via backend). **Validado no harness:** AROS mapeia o Fast
  RAM (`[Z2-RAM] mapped guest=00200000-009fffff`), enumera como 'Fast Memory',
  passa da lowlevel.library até dosboot.resource; `rom_boot_aros` + toda a suíte
  (17/17) verdes. **lide QUEBRADO de propósito** (autorizado): ele registra no
  zorro2, que deixou de ser o sequenciador — será readaptado no passo 2 (board
  DIRECT ROM + janela EXTERNAL). **Pendência do passo 2:** o Super Buster decodifica
  Z3 consultando `bellatrix_zorro3_in_board_window`; uma board Z3 via board_registry
  não entra nesse registro — reconciliar quando houver board Z3/EXTERNAL (hoje
  dormente, sem board Z3). test_z2_fast_ram removido (assunto migrado; cobertura em
  board_registry + rom_boot_aros).
