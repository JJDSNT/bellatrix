---
id: ISSUE-0052
title: "Operational tracker — fast compute program"
status: doing
priority: critical
type: task
owner: agent
created_at: 2026-07-12
updated_at: 2026-07-15
parent: ISSUE-0051
tags: [execution, aros, ks31, multicore, emu68, performance]
---

# Objetivo operacional

Executar ISSUE-0051. Este é o único tracker operacional vigente para a frente
de performance/multicore. A antiga ISSUE-0050 está em
`AI_context/consolidated/history/ISSUE-0050.md` como histórico da campanha
PiStorm temporal concluída/absorvida.

Pendências encontradas em issues antigas só entram nesta fila quando ainda são
necessárias para o objetivo novo. Checklist histórico não é backlog implícito.

# Ordem corrente

- [x] **P0/P1: executar ISSUE-0058 — congelar branches divergentes e auditar o
  contrato original `start.c`/`vectors.c`/IRQ por core.** Concluído
  2026-07-15 (ISSUE-0058 P0/P1 totalmente `[x]`).
- [ ] **P2: reestabelecer Emu68 Core 0 + fault handler como baseline de
  hardware antes de qualquer otimização multicore.** Baseline QEMU
  funcionando desde a ISSUE-0061 (2026-07-16): a regressão de boot que travava
  todo build Emu68 foi raiz-causada (progress driver do `MainLoop` deletado
  por um refactor) e corrigida; `frame_counter` avança normalmente com
  IPL/interrupção real em teste multicore. Ainda não fechado: validação em
  hardware real (adiada por decisão do usuário) e a auditoria fina do
  adaptador (ISSUE-0058 P2 remanescente).
- [ ] Congelar baseline AROS Musashi 68040 multicore: milestones wall + SysInfo.
- [ ] Congelar baseline KS3.1 Musashi 68040 multicore com o mesmo protocolo.
- [ ] A/B Bellatrix lock wake-on-waiter e event-stream/empty-step.
- [ ] Depois de ISSUE-0058, construir/validar a topologia Emu68 que respeite o
  contrato provado; Core 1 não é mais premissa.
- [ ] Medir bus/MMIO/STOP/IRQ do baseline fault-driven e atacar o domínio dominante.
- Otimização do Rigel está fora desta fila e pertence somente à ISSUE-0053.

Ver `AI_context/issues/ISSUE-0061.md` para o detalhe do fix de regressão que
desbloqueou o item P2 acima.

# Gates permanentes

- ISSUE-0058 é o primeiro gate e prevalece sobre a fila anterior.
- AROS/KS3.1 acelerados não regridem silenciosamente.
- Core 0 nunca throttla CPU para melhorar RT%.
- Nenhuma métrica de frame/presenter representa velocidade geral.
- KS1.3/Battle/68000 são regressão secundária, não baseline de produto.
- Toda mudança mantém rollback e matriz funcional.

# Evidência inicial

- Musashi 68040 multicore AROS chega rapidamente ao OS no Pi, observação do
  usuário; baseline quantitativo de milestones/SysInfo ainda pendente.
- AROS baseline 16:23:15: produced=presented, coalesced=0; responsividade não
  vem do presenter.
- Protocolo PiStorm implementado: beam publicado, fila postada, hot state,
  event stream e Core 2 self-paced/hybrid.
- 39/39 testes host/Rigel/TSAN verdes antes da promoção deste programa.
