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
  - src/cpu/cpu_bridge.c
  - src/machine/bus/zorro3/zorro3.c
  - scripts/setup.sh
---

# Objetivo

Reduzir a integração Emu68 ao menor delta compatível com seu desenho nativo e
definir uma fronteira única de memória/MMIO que possa ser consumida por Emu68,
Musashi e futuros backends. Core 0 é a baseline provisória de estabilização; o
contrato não pode depender de placement.

# Evidência atual

- O `start.c` original executa boot/JIT no Core 0 e estaciona os secundários.
- `vectors.c` já decodifica Data Abort, chama `SYSReadValFromAddr()` /
  `SYSWriteValToAddr()` e retoma o JIT. Este mecanismo não deve ser substituído.
- O patch `0002` acrescentou um bloco Bellatrix grande e duplicou parte do
  tratamento de boards/Autoconfig que já existe no Emu68.
- O seam desejado é um adapter de plataforma pequeno, estático e selecionado em
  compile time, chamado pelo caminho nativo; não uma tabela de function pointers
  nem um novo loop cooperativo.
- Musashi deve chamar diretamente o mesmo serviço de transação externa, sem
  fabricar Data Abort ARM.
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
- Existem bases Z3 concorrentes (`0x10000000` e `0x40000000`).
- Não há board Z3 Bellatrix funcional de ponta a ponta.
- Boards nativas do Emu68 podem mapear Z3 pelo callback próprio; isso é um
  domínio separado e precisa convergir no lifecycle comum de regiões.

# Contrato alvo

1. Preservar `SYSHandler` e a semântica de fault/retomada do Emu68.
2. Classificar endereço CPU de 32 bits em `DIRECT`, `EXTERNAL` ou `UNMAPPED`.
3. Mapear `DIRECT` pelo backend (MMU/bank), sem hook no steady state.
4. Encaminhar `EXTERNAL` a um serviço Bellatrix síncrono, sem alocação e sem
   function pointer no hot path Emu68.
5. Fazer Musashi chamar a mesma implementação de serviço.
6. Definir lifecycle transacional `map/unmap/reset/shutup` antes da primeira
   board Z3.
7. Não implementar o hook definitivo antes de fixar descritores, ownership,
   endian/lane semantics e a base/faixa Z3.

# Ordem de trabalho

- [x] Retirar patches `0025`–`0034` da sequência padrão.
- [x] Corrigir detecção cumulativa de `0022`/`0023` no setup.
- [ ] Gerar checkout limpo somente com o fechamento atual e comparar o delta.
- [ ] Inventariar cada branch Bellatrix em `SYSReadValFromAddr()` e
  `SYSWriteValToAddr()` contra o HEAD original.
- [ ] Especificar a ABI interna do adapter e matriz completa do mapa.
- [ ] Reconciliar base Z3, Autoconfig e Super Buster.
- [ ] Implementar Z3 Fast RAM `DIRECT` como primeiro caso, primeiro no contrato
  de backend e depois em Emu68/Musashi.
- [ ] Medir antes de otimizar o hook; não validar em hardware sem autorização.

# Critérios de aceite

- Um setup limpo reproduz exatamente o fechamento documentado.
- O fault handler e a retomada JIT permanecem semanticamente nativos.
- Endereço CPU não sofre máscara global de 24 bits.
- Emu68 e Musashi compartilham classificação e serviço externo, sem compartilhar
  detalhes de exceção ARM.
- Z2 não regride e a primeira Z3 Fast RAM é invisível antes de Autoconfig.
- Nenhuma interface pública ou interna fixa Emu68 no Core 0.
