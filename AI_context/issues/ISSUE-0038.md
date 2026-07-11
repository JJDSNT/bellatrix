---
id: ISSUE-0038
title: "Emu68 JIT: regressao de liveness do chipset + corrupcao do vetor 0x6C no boot KS13"
status: doing
priority: high
type: bug
owner: agent
created_at: 2026-07-05
updated_at: 2026-07-11
tags:
  - emu68
  - jit
  - executionloop
  - liveness
  - interrupts
  - regression
related_files:
  - patches/0003-bellatrix-execution-loop.patch
  - patches/0020-emu68-stop-liveness.patch
  - emu68/src/ExecutionLoop.c
  - emu68/src/M68k_LINE4.c
  - src/cpu/emu68/bellatrix.c
  - src/cpu/cpu_bridge.c
---

# RETOMADA DO BRANCH (2026-07-10)

## 2026-07-11 — STOP deixa de ser inferido por opcode

A retomada encontrou um workaround não aceitável no checkout do Emu68:

- `EMIT_STOP` mantinha PC sobre o STOP, reexecutava a unit e somava oito
  instruções artificiais por passagem para fazer o chipset andar;
- o delivery de IRQ lia `*PC` e, se encontrasse `0x4e72`, avançava PC antes de
  montar o frame de exceção.

Isso acoplava dispatcher de interrupção a opcode/endereco de ROM e tornava a
semântica dependente da corrida entre o report hook e a unit STOP.

Substituído por protocolo embutível explícito:

1. STOP atualiza SR, avança PC uma vez, marca `M68KState.STOPPED` e encerra a
   janela;
2. API retorna `EMU68_STOP_STOPPED` enquanto IPL guest não supera a máscara SR;
3. scheduler Bellatrix contabiliza a janela solicitada como tempo ocioso, sem
   fabricar instruções JIT retiradas;
4. IPL persistente acima da máscara limpa STOPPED e usa o delivery normal, com
   PC pós-STOP já correto;
5. removida toda inspeção de opcode no `ExecutionLoop`.

Foi corrigida também uma violação ABI adjacente:
`emu68_api_dispatch_quantum_progress()` era chamado depois de
`M68K_LoadContext()`, podendo clobberar registradores 68k recém-restaurados.
Agora todas as chamadas C ocorrem com contexto salvo; somente depois a máquina
de registradores é restaurada.

API elevada para v2 e adicionadas métricas `stopped_return_count` e
`stopped_wake_count`. Patches 0003/0020 foram atualizados e passam verificação
de aplicação reversa sobre o checkout.

Validação inicial: Emu68 single-core compila; AROS em QEMU passa reset/overlay
e chega a `FNOP/FSAVE` sem crash imediato em 180 s. TCG é lento demais para
atingir boot screen nessa janela; próximo gate é Pi real.

O branch `wip/emu68-liveness` foi atualizado para o `main` em `ea4b474`. O antigo
commit WIP `10e3051` nao foi reaplicado porque seu conteudo ja havia sido incorporado e
superado por `0e113d2` e pelos commits posteriores da API publica e do multicore.
Uma referencia de seguranca preserva o estado antigo em
`backup/emu68-liveness-pre-main-20260710`.

O checkout aplicado de `emu68` ainda contem os hooks modernos esperados: preservacao
explicita de x12, re-zero de v28, `bellatrix_emu68_report_jit_progress()` e despacho de
quantum pela API publica. Alteracoes locais preexistentes nos submodulos foram mantidas;
por isso qualquer validacao deve primeiro garantir que cada submodulo esta no commit
esperado pelo superprojeto e distinguir patches Bellatrix de outros WIPs locais.

## Plano de retomada

1. **Normalizar e verificar o ambiente Emu68**: reaplicar/verificar os patches pelo
   `scripts/setup.sh`, confirmar os commits dos submodulos e produzir um build QEMU
   minimo com launcher ativo (necessario para selecionar ROM/ADF), sem USB/Bluetooth.
2. **Revalidar os baselines baratos**: DiagROM ate conclusao e KS13 sem disco ate a
   hand/insert-disk screen, sem TRACE invasivo. Registrar serial, frame e PC final.
3. **Revalidar boot por disco**: KS13 + ADF conhecido, confirmando VBL/CIA/trackdisk e
   progresso alem do strap. Comparar com Musashi single-core somente nos pontos de
   divergencia, nao como requisito de desempenho.
4. **Retestar AROS**: exigir pelo menos as 35 InitResidents ate `trackdisk.device`, que e
   o baseline ja observado em QEMU; depois medir progresso adicional com um limite de
   tempo reproduzivel.
5. **Auditar liveness/IRQ apenas se houver regressao**: correlacionar v30, quantum,
   `INT.IPL`, SR e INTREQ nos boundaries do JIT/STOP. Evitar `kprintf` dentro do
   MainLoop sem spill/reload completo do contexto pinado.
6. **Validar no Raspberry Pi real**: confirmar KS13+ADF e AROS, onde o JIT pode ser
   avaliado sem a penalidade extrema do TCG.
7. **Encerrar a issue**: remover ou reduzir a instrumentacao TRACE-gated depois da
   validacao em hardware e documentar a matriz final de ROM/disco/backend.

## Frente explicita: API publica Emu68

A liveness deve ser provada atraves do contrato publico de `ISSUE-0039`, nao por novos
atalhos para globais internos. A sequencia de trabalho e:

1. fazer `EMU68_BUS_SYNC_REQUIRED` encerrar a janela cooperativa como barreira real;
2. definir ownership/lifecycle de `create/reset/run/request_stop` e retirar gradualmente
   o bootstrap implicito por `M68K_StartEmu()`;
3. definir ranges e efeitos do bus externo (custom, CIA, autoconfig, overlay/ROM);
4. validar invalidacao JIT com codigo mutavel e mudancas de overlay;
5. provar `run_cycles()` e as barreiras no modelo multicore Core 1/Core 2.

## Frente explicita: contrato IRQ/IPL

O Emu68 **nao precisa de IRQ ARM fisica no Bellatrix** para receber interrupcoes do
Amiga. A linha guest e um nivel IPL persistente calculado pelo Rigel e publicado por
software em `M68KState.INT.IPL`; `INT32` torna a mudanca observavel e o MainLoop decide a
entrega comparando o nivel com a mascara do SR. `INT.ARM` e a traducao PiStorm de uma
linha fisica externa para nivel 6 e deve permanecer zero no variant Bellatrix.

IRQs fisicas de USB, Bluetooth e audio sao interrupcoes **do host**, nao IRQs Amiga.
Elas ficam fora do Emu68: hoje sao atendidas por polling no Core 3; futuramente podem
chegar ao arbitro/Core 0 se medicao justificar, mas nunca devem entrar no handler
PiStorm que hardcoda IPL 6 nem interromper o core que mantem registradores M68K pinados.

A validacao desta frente deve cobrir:

- niveis 1..7 e clear para zero via `emu68_set_irq_level()`;
- `INT.ARM == 0` no Bellatrix e ausencia de dependencia de IRQ/FIQ ARM;
- IPL persistente enquanto INTREQ/INTENA permanecer ativo, inclusive durante STOP;
- mascaramento pelo SR e entrega assim que o nivel superar a mascara;
- acknowledge/clear sem pulso perdido entre Core 2 e os boundaries do JIT;
- nenhuma chamada C no MainLoop/delivery sem preservar x12, x13-x17 e v28.

## Criterio de conclusao

- DiagROM completa no Emu68;
- KS13 alcanca hand screen e boota um ADF conhecido sem perda de VBL/IRQ;
- AROS alcanca ao menos `trackdisk.device` de forma repetivel;
- nenhuma corrupcao de registradores pinados ou vetor 0x6C com traces seguros;
- API publica encerra janela em `SYNC_REQUIRED` e tem lifecycle/ownership documentado;
- IRQ guest funciona apenas pela linha IPL virtual, sem IRQ ARM fisica no core do JIT;
- validacao equivalente no Pi real, seguida da limpeza da instrumentacao temporaria.

# RESOLUCAO FINAL (2026-07-06)

**AROS boota no emu68 COM liveness de chipset: 35 InitResidents ate trackdisk.device,
mesma profundidade do baseline, validado em QEMU (15 min TCG).** DiagROM roda completo
(banner+testes via serial, tela hires propria). A cadeia causal completa, do sintoma a raiz:

1. **Regressao datada** (`40adee1`, 18/06): removeu o avanco de chipset do MainLoop —
   deixou so no fault de MMIO. Sem MMIO (loops de RAM, Wait/STOP), o tempo congela.
   Provavelmente foi um remendo para crashes misteriosos cuja raiz verdadeira e o item 3.
2. **STOP dormia para sempre** no variant bellatrix (wfi sem ninguem armar timer ARM).
   Patch 0020: STOP nao dorme; re-despacha a unit sem avancar PC ate INT!=0, contando
   8 insns/passada no v30 (o tempo idle flui pelo delta-gate do hook).
3. **RAIZ DOS CRASHES: chamar C (machine/rigel) do contexto do MainLoop clobbera estado
   pinado do JIT** — em TRES camadas, todas agora tratadas no hook do patch 0003:
   - x13-x17 (D0-D3 do M68K, caller-saved no ABI) -> `M68K_SaveContext/LoadContext`
     em volta da chamada (padrao do proprio translation-miss path);
   - kprintf dos traces `[EXC-REQ]/[EXC-PC]` no delivery — mesmo problema, mesmo fix
     (era isso que corrompia GfxBase em D2/D3 -> trample dos vetores 0x60-0x7C ->
     reboot loop do KS13 SO em builds RIGEL_TRACE);
   - **x12 (cache da unit p/ fast dispatch LastPC==PC) e v28 (assumido zero pelo JIT)**:
     NAO cobertos por Save/LoadContext, e `-ffixed-x12` NAO impede o GCC de usar x12
     como scratch de prologo/epilogo em frames grandes (rigel `compose_line`, frame
     0x1110: `mov x12,#0x1200` = o pitch!). Prova: crash deterministico com
     ELR=0x1200=X12 no loop de checksum do AROS (fast path saltou para x12 clobberado).
     Fix: save/restore explicito de x12 + re-zero de v28 no hook.
4. **Mapa de memoria**: chip mapeado com exatamente BELLATRIX_CHIP_RAM_SIZE e
   0x100000-0x1FFFFF como MIRROR do chip (hardware-true: Agnus ignora A20) — resolve
   simultaneamente o sizing 2MB do KS13 (detecta wrap -> 1MB) e o SSP inicial do AROS
   (header ROM 0x11144EF9 -> 0x114EF9, que precisa ser RAM coerente com DMA).
   Obs: o mapa 1:1 generico do Emu68 fica por baixo — o mirror precisa ser mapeado
   explicitamente por cima (mmu_map phys=0 virt=0x100000).
5. **cmake**: `target_compile_options(rigel ... -ffixed-x12 -ffixed-q28..q31)` — o rigel
   e criado ANTES do add_compile_options global do Emu68 e nao herdava os flags
   (defesa em profundidade; sozinho nao basta pelo caveat do prologo do GCC).

Tecnica de diagnostico que fechou o caso (reutilizavel): QEMU monitor via socket para
amostrar PC do core (`info registers`) -> identificou wfi-halt e o loop pos-dump;
`x /Nbx` no ring do console (0xffffff80002c13e0) para ler o dump de excecao que o
kprintf bufferizado nunca drenou apos o halt; objdump no librigel.a para achar o
`mov x12,#0x1200` do compose_line.

Pendencias que continuam abertas (proximas sessoes):
- KS13 emu68: strap/hand screen — revalidar com o pacote final (o mistério "VERTB some do
  INTREQ" pode ter sido o mesmo clobber de x12/v28).
- AROS alem do trackdisk (o baseline tambem parava ai por tempo de TCG — validar no Pi
  real, ~50x mais rapido).
- API de integracao emu68 (plano do Jaime): os pontos pistorm-centric mapeados sao a fonte
  de IPL, a estrategia do STOP, o hook de progresso e o dono do mapa baixo — os patches
  0002/0003/0020 sao a espec de facto.
- Instrumentacao TRACE-gated em bellatrix.c ([EMU68-SAMPLE], [VEC6C-*], [SETINTVEC],
  vecpage trap) — manter ate o Pi real validar, depois remover.

# Contexto

"Um tempo atras a integracao emu68 chegava a rodar um pedaco do boot do AROS com avanco no
serial; agora nao mais." Investigacao em 2026-07-05 (QEMU raspi3b, KS13 e aros.rom,
launcher/USB/BT off).

# Regressao 1 (root cause datada): hook de progresso removido do MainLoop

`40adee1` (2026-06-18, "cpu: update emu68 bridge progress hooks") removeu o bloco BELLATRIX
do `MainLoop` (delta de v30 -> `bellatrix_bridge_cpu_progress`) do patch 0003 e deixou o
avanco do chipset APENAS no caminho de bus-fault (`vectors.c` ->
`bellatrix_emu68_report_jit_progress`). Consequencia: qualquer fase do guest que nao toca
MMIO (loops so-RAM, idle Wait/STOP) congela o tempo do chipset — sem VBL, sem CIA, sem IPL.
KS13 congelava logo apos o scheduler iniciar (FSAVE/FRESTORE do context switch, primeiro
Wait). AROS parava cedo no boot.

**Fix aplicado (patch 0003 regenerado):** bloco restaurado no topo do `while(1)` do
`MainLoop`, chamando `bellatrix_emu68_report_jit_progress(v30_abs, pc)` — mesmo acumulador
absoluto do fault hook (sem dupla contagem). Duas licoes de implementacao:

1. **Clobber de registradores**: o caminho C clobbera x13-x17 (registradores M68K pinados,
   caller-saved no ABI) — a chamada precisa ser embrulhada em
   `M68K_SaveContext(ctx)` / `M68K_LoadContext(getCTX())` (mesmo padrao do caminho de
   translation-miss). Sem isso: crash imediato (tela amarela KS13, PC=8, opcode 00fc).
   x12 e q28-q31 sao `-ffixed` globais no build (CMakeLists linha 192) — seguros.
   FP0-7 vivem em v8-v15 (callee-saved, so os 64 bits baixos) — seguros.
2. **Custo por iteracao**: Save/Load+advance a cada dispatch e caro demais (TCG ~15x mais
   lento). Gate: reporta quando delta v30 >= 64 insns (~512 ciclos ≈ 2 scanlines de
   latencia de IRQ; o fault hook continua sincronizando antes de MMIO) ou apos 64
   dispatches sem progresso.

# Regressao 2: STOP dorme para sempre no variant bellatrix

O variant bellatrix nao define PISTORM, entao `EMIT_STOP` emitia `wfi()`. O Emu68 nunca
arma o timer ARM (so le o counter p/ E-clock) -> `wfi` = sono eterno -> mesmo com o hook do
MainLoop restaurado, o STOP congelava tudo (PC parado em buffer JIT, PSTATE.I=0, confirmado
via QEMU monitor).

**Fix aplicado (patch 0020, novo):** para BELLATRIX, `EMIT_STOP` nao dorme: se `INT==0`
termina a unit SEM avancar o PC (o STOP e re-despachado a cada passada do MainLoop, que
avanca o chipset; a unit re-executada retira instrucoes, entao o delta-gate mantem o tempo
fluindo); quando `INT!=0`, avanca PC e o MainLoop despacha a IRQ. Semantica observavel
igual ao loop wfe do PiStorm, sem dormir. Resultado: KS13 passou do ponto de congelamento
(frames 256->768+ continuos).

# Regressao 3: janela low-RAM de 2MB no caminho legacy (mesma divergencia da ISSUE-0037)

`bellatrix.c` (legacy non-boards) mapeava `0x000000-0x1FFFFF` (2MB) como chip+"slow" RAM
via MMU. Alem disso, o mapa 1:1 generico do Emu68 continua por baixo — reduzir o
mmu_map para 1MB NAO basta: `0x100000-0x1FFFFF` continuava lendo DRAM crua. KS13
dimensionava chip como 2MB, punha SSP em 0x200000 e estruturas do exec em memoria que o
chipset/DMA nao enxerga -> frame de excecao ia para o void -> RTE lia lixo -> PC em runaway
por 0x08xxxxxx com IPL pendente ignorado.

**Fix aplicado:** chip mapeado com exatamente `BELLATRIX_CHIP_RAM_SIZE` + o buraco
`0x100000-0x1FFFFF` mapeado como pagina fault-driven (sem MMU_ACCESS, padrao das CIAs) ->
open bus 0xFF, igual harness/Musashi. Resultado: `old_sp=00100000` (chip = 1MB correto),
multiplas VBLs entregues corretamente.

# RESOLVIDO: corrupcao do vetor 0x6C era bug de INSTRUMENTACAO (trace build)

O escritor do trample de vetores foi identificado por eliminacao instrumentada
(watcher de chip[0x6C] com ring de PCs + dump de registradores no SetIntVector):
os prints `[EXC-REQ]`/`[EXC-PC]` do bloco de delivery de IRQ (patch 0003, gated em
BELLATRIX_RIGEL_TRACE_BUILD) chamavam `kprintf` sem spill de contexto. kprintf e C
de ABI normal: clobbera x14-x17 = **D0-D3 do M68K interrompido**. A task de init do
graphics tinha GfxBase em D2/D3 -> voltava da VBL com base 0 -> escrevia campos de
struct em offsets absolutos baixos (0x5A-0x8B) -> atropelava vetores 0x60-0x7C ->
VBL seguinte saltava para fc0000 -> reboot em loop. A prova: build com watchers
(TRACE_BUILD) mas SEM os prints de delivery (RIGEL_TRACE_BUILD off) nao corrompe:
`SetIntVector(6, 0x2005d4)` com ponteiro valido, zero trample, boot avanca.

**Fix:** os dois kprintf do delivery agora sao embrulhados em
`M68K_SaveContext`/`M68K_LoadContext` (patch 0003). Regra geral gravada: QUALQUER
chamada C a partir do MainLoop/delivery (fora do fault handler, que salva tudo)
precisa do spill/reload completo — x13-x17 sao caller-saved e carregam D0-D3;
x12/q28-q31 sao -ffixed globais; FP0-7 em v8-v15 (callee-saved).

# Bloqueio atual: boot KS13 instavel na fase do strap (timing-dependente)

Com todos os fixes acima, KS13/emu68 boota o exec/graphics init completo, entrega as
primeiras VBLs corretamente e chega a fase do strap — mas estaciona ali, com desfecho
diferente conforme o timing do build:

- Build TRACE-only: idle eterno em `pc=fc0f90` (exec idle), INTENA=602c ok, mas
  **INTREQ nunca mais recebe VERTB** (fica 0x0040=BLIT) — nenhuma IRQ entregue apos o
  init. Tela branca, BPLEN off (strap bloqueado em DoIO esperando timer/interrupcao).
- Build full-trace (prints seguros): guest **crasha num Alert** — loop de Guru em
  `pc=fc2ff0` (movem $180.w + assinatura 'HELP' em $0), INTEN off, VERTB+BLIT
  pendentes em INTREQ.

Ou seja: ha pelo menos mais um bug real na janela de entrega de IRQ / avanco de
quantum do caminho emu68 (classe parecida com ISSUE-0026 do harness: "mid-timeslice
IRQ loss"). Proxima frente de investigacao:

1. Auditar a janela INT32/IPL: quando PAL_IPL_Set escreve INT.IPL vs quando o
   MainLoop checa (unit boundaries) vs quando o STOP consome; procurar janela em que
   IPL sobe e desce sem o MainLoop ver (VERTB "perdido"), e entrega de IRQ com SR
   inconsistente (fonte do Alert).
2. Comparar com o caminho Musashi (bellatrix_singlecore_advance_cpu_cycles +
   m68k_set_ipl) que funciona no mesmo chipset.
3. So depois re-testar aros.rom (pre-fixes ele chegava a InitResident "mmu" com
   serial funcionando).

# Corrupcao do vetor 0x6C (analise historica da caca — mantida para referencia)

Com os tres fixes acima, KS13 boota ate a primeira VBL (entregue corretamente,
`vec_val=00fc0d14`, frame em `sp=000ffff8`), mas ~265 CCKs depois do dispatch o word baixo
de `chip[0x6C]` e zerado (`fc0d14 -> fc0000`) — padrao de write de 16 bits em `0x6E`.
A proxima VBL salta para `fc0000` (reset do ROM) -> boot em loop eterno (tela cinza).

Sequencia observada (build TRACE):
```text
[RIGEL-INT-W] reg=09a raw=c000 pc=00fc1232 intena=202c->602c ipl=0->3
[EXC-REQ] lvl=3 vec=006c old_pc=00fd3c2e old_sp=00100000 vec_val=00fc0d14   <- OK
[RIGEL-MMIO-W] reg=084/086 write=0000 pc=00fc6d68  (gfx VBL server: COP2LC=0)
[RIGEL-INT-W] reg=09c raw=0020 pc=00fc1352 (ExitIntr limpa VERTB)
[VEC6C-CHANGE] 00fc0d14->00fc0000 pc=00fc1696 (FindName; janela de 64 insns)
[EXC-REQ] lvl=3 vec=006c vec_val=00fc0000  -> salto p/ fc0000 = reboot
```

O escritor exato ainda nao foi identificado (writes de CPU em chip RAM sao MMU-diretos,
invisiveis). Diagnostico definitivo em andamento: build TRACE mapeia a pagina 0 como
fault-driven e loga `[VEC-W]` com PC exato — run longo (~45min TCG) necessario porque o
trap de pagina 0 desacelera o boot ~15x (todas as leituras de SysBase em $4 faultam).

Hipoteses: (a) gfx VBL server rodando cedo demais (mid-init, LOFlist=0 ja observado) com
alguma estrutura nula escrevendo em low RAM; (b) frame/RTE format mismatch no caminho de
delivery do patch 0003; (c) blitter (BLTEN ligado, dmacon=0250) com destino proximo de 0.

# Diagnostico/instrumentacao adicionados (remover depois)

- `[EMU68-SAMPLE]` em `bellatrix_emu68_report_jit_progress` (TRACE): pc/intena/intreq/ipl
  periodico.
- `[VEC6C-CHANGE]` watcher de `chip[0x6C]` (TRACE).
- `[VEC-W]` writes em addr<0x100 no `bellatrix_bus_access` (TRACE).
- Trap de pagina 0 em `apply_overlay_map` (TRACE) — diagnostico, lento.

# Estado AROS

Pre-fix, aros.rom chegava a `InitResident (100 01 "mmu")` via serial e parava (com chipset
vivo). Pos-fixes ainda nao re-testado alem do boot inicial (QEMU/TCG lento). Retestar apos
resolver a corrupcao do vetor.

# Proximos passos

1. Aguardar run longo do trap de pagina 0 -> identificar escritor exato de 0x6E.
2. Corrigir a causa (guest-visivel ou bug nosso de delivery/frame).
3. Retestar KS13 ate hand screen estavel; retestar aros.rom (serial deve passar de "mmu").
4. Validar no Pi real (mais rapido que TCG).
5. Remover instrumentacao TRACE listada acima.
