---
id: ISSUE-0061
title: "Emu68 boot regression root-caused; public-machine-API retired; frame-100 stall still open"
status: doing
priority: critical
type: bug
owner: agent
created_at: 2026-07-16
updated_at: 2026-07-16
tags: [emu68, jit, executionloop, stop, liveness, regression, public-api]
related_files:
  - AI_context/archive/emu68-public-machine-api-2026-07.md
  - AI_context/consolidated/emu68_routing_vs_synchronization.md
  - AI_context/consolidated/history/ISSUE-0038.md
  - AI_context/issues/ISSUE-0058.md
  - patches/0003-bellatrix-execution-loop.patch
  - patches/0020-emu68-stop-liveness.patch
  - patches/0035-emu68-modeled-cycles.patch
  - scripts/build.sh
  - scripts/setup.sh
---

# Contexto

Sessão longa de 2026-07-16 investigando "hangs after [JIT] Let it go..." em
single-core emu68. Duas descobertas principais, ambas confirmadas por A/B
testing (não especulação):

## 1. Causa raiz da regressão (RESOLVIDA em parte)

O refactor da "public machine API" (commits `c8599c8`..`f28b21a`, 07-13, e
o rebaseline `7b4f7c9`, 07-15 que virou o default) **removeu a chamada
`bellatrix_emu68_report_jit_progress()` do `MainLoop`** — o único mecanismo
que avança o clock do chipset (Rigel) quando o CPU não toca memória
mapeada (chip RAM é MMU-direto, nunca falta). Substituída por uma API nova
nunca provada, e o bloco inteiro passou a ser excluído quando
`BELLATRIX_EMU68_FAULT_DRIVEN=1` (default desde então). Resultado: qualquer
loop só-RAM (como o idle loop do Exec) travava o tempo do chipset pra
sempre.

**Fix aplicado:** `patches/0003` restaurado (chamada incondicional, sem
exclusão por FAULT_DRIVEN). `patches/0020` (STOP) também restaurado pra
versão original do ISSUE-0038 (0e113d2: checa INT, credita ciclos falsos no
v30 se INT==0, sem dormir em wfi/wfe).

**Resultado verificado:** de "trava total, frame_counter nunca sai de 0"
pra progresso real. Progresso reproduzido de forma determinística em
múltiplos rebuilds.

**CORRIGIDO (mesmo dia):** o "trava depois do frame=100" registrado antes
NÃO é um stall de verdade — era só o teste single-core não ter rodado
tempo suficiente pra passar do primeiro checkpoint. Um teste multicore
emu68 no mesmo dia mostrou `frame=100 → frame=500` avançando normalmente,
com `int32`/`ipl` mudando de estado (interrupção real chegando). O fix do
progress driver está funcionando; não há bloqueio conhecido remanescente
por trás desse sintoma específico.

## 2. Limpeza da "public machine API" (parcialmente feita)

Por pedido do Jaime: a API pública nunca foi sobre substituir o fault
handler (que só roteia endereço) — era sobre sincronizar progresso, e essa
sincronização foi erroneamente acoplada à decisão fault-driven vs API.

Removido e arquivado (`AI_context/archive/emu68-public-machine-api-2026-07.md`):
- Patches 0025-0034 (classificação explícita de acesso por opcode line —
  já estavam excluídas do build ativo desde `3f43bca`, 07-15, antes desta
  sessão).

Mantido (por necessidade empírica, não por preferência):
- **Patch 0035** (`emu68-modeled-cycles`, cycle accounting). Removê-la
  causou regressão real e reproduzível (frame_counter volta a travar em
  0), mesmo com o fix de 0003/0020 aplicado. Testado duas vezes em cada
  direção, determinístico. Hipótese não confirmada: adicionar/remover
  código em `EmitINSN` (M68k_Translator.c) muda o layout compilado da
  função de um jeito que reativa uma classe de bug já documentada
  (ISSUE-0038: GCC usa x12/v28 como scratch de prólogo/epílogo em frames
  grandes, sensível ao tamanho exato do frame). NÃO é a contagem de
  CYCLE_COUNT em si (dado inerte, não relacionado ao v30 que o STOP usa).
- `src/cpu/emu68/emu68_machine_*.{c,h,S}` (implementação Bellatrix da API).
  Ficaram porque `emu68_backend.c` ainda depende deles pra compilar
  (init/get/log são chamados incondicionalmente de `bellatrix.c`, mesmo
  que o `.run()` nunca seja exercido em modo fault-driven — ver
  `bellatrix_cpu_backend_owns_execution_loop()`, que retorna 0 pra
  fault-driven, e o comentário "the public machine driver remains compiled
  only as an A/B rollback option"). Remover isso de verdade exige reescrever
  `emu68_backend.c`/`cpu_backend.c` pra não depender mais da abstração
  CpuBackend pro caso emu68 — trabalho maior, não feito nesta sessão.

# Próximos passos

1. ~~Investigar o que trava depois do frame=100~~ — descartado, não era
   stall de verdade (ver correção acima). Lembrar em investigações futuras:
   `BELLATRIX_RIGEL_TRACE_BUILD=1` continua PERIGOSO em emu68 (corrompe
   D0-D3 via os prints `[EXC-REQ]`/`[EXC-PC]` de `ExecutionLoop.c`, já
   documentado, redescoberto nesta sessão) — usar QEMU monitor externo
   (`info registers`, técnica validada no ISSUE-0038) em vez de kprintf/trace
   quando precisar de visibilidade sem contaminar o guest.
2. **Confirmar ou descartar a hipótese do patch 0035** antes de tentar
   removê-la de novo — comparar o disassembly compilado de `EmitINSN` com
   e sem o patch.
3. **Terminar a remoção de `emu68_backend.c`/`emu68_machine_*`** se/quando
   fizer sentido: precisa reescrever a seleção de CpuBackend pra emu68 não
   depender mais dessa abstração, já que ela só serve o modo "public"
   (também abandonado) e o A/B rollback que não é mais necessário.
4. **Cycle accounting (patch 0035) para fins de teste:** o Jaime propôs
   manter só essa parte da API como feature isolada e útil. Dado o achado
   #2 acima, ela JÁ ESTÁ mantida — mas vale reavaliar se merece virar um
   mecanismo de primeira classe (não mais "resíduo da API abandonada") ou
   se deve ser reimplementada do zero de forma mais limpa/deliberada,
   depois que a hipótese de layout for confirmada ou descartada.
5. Validar tudo em Pi real quando o boot single-core emu68 estiver
   completo — TCG é ~15-50x mais lento, conforme já registrado em
   ISSUE-0038.
