---
id: ISSUE-0064
title: "Single-core emu68: CPU trava em exec idle (pc=0xfc0f90) — VERTB nunca entregue, IRQ nunca chega"
status: doing
priority: critical
type: bug
owner: agent
created_at: 2026-07-17
updated_at: 2026-07-17
tags: [emu68, jit, irq, vertb, single-core, regression-or-longstanding]
related_files:
  - AI_context/consolidated/history/ISSUE-0038.md
  - AI_context/issues/ISSUE-0061.md
  - AI_context/issues/ISSUE-0063.md
  - src/cpu/emu68/bellatrix.c
  - src/machine/machine_rigel.c
  - src/machine/machine_rigel_step.c
  - emu68.txt
---

# Resumo

Boot single-core emu68 (imagem `emu68/install-bellatrix-rigel/Emu68.img`,
2026-07-17, `BELLATRIX_PROFILE=1`, KS1.3, `megademoA.adf`) avança em boot
normalmente até o scheduler do Exec (FSAVE/FMOVEM/FMOVE/FRESTORE, contexto
de troca de tarefa), e então **a CPU 68k para de progredir**. Captura do
Jaime em `emu68.txt` (raiz do repo).

# Evidência

```
[EMU68-LIVE] frame=100 pc=00fc0f90 saved_pc=00fc0f90 sr=2000 stopped=0 int32=00000000 ipl=0 insn=4358475
[DIAG] faults=12529 min=00000060 max=00e8004a last=00dff09c ...
...
[EMU68-LIVE] frame=500 pc=00fc0f90 saved_pc=00fc0f90 sr=2000 stopped=0 int32=00000000 ipl=0 insn=129491341
[DIAG] faults=22129 min=00000060 max=00e8004a last=00dff09c ...
```

Entre os dois checkpoints (400 frames de chipset, ~125M instruções
executadas): **`pc` idêntico, `saved_pc` idêntico, `int32=0` nas duas,
`ipl=0` nas duas**. `last` fault address idêntico (`0xdff09c` =
**INTREQ**). A CPU está presa num loop de poll de INTREQ — nunca recebe a
interrupção que está esperando (VERTB), apesar do contador de frame do
chipset avançar normalmente (100→500, ou seja, o Rigel está gerando VBL
por baixo).

# Por que isso não é surpresa nova — é um bug antigo nunca fechado

`AI_context/consolidated/history/ISSUE-0038.md:385-387` já documentou
exatamente este sintoma:

> Build TRACE-only: idle eterno em `pc=fc0f90` (exec idle), INTENA=602c
> ok, mas **INTREQ nunca mais recebe VERTB** (fica 0x0040=BLIT) — nenhuma
> IRQ entregue apos o init.

Mesmo PC (`0xfc0f90`), mesmo sintoma. O ISSUE-0038 deixou isso como
**próxima frente de investigação, não resolvida**:

> Ha pelo menos mais um bug real na janela de entrega de IRQ / avanco de
> quantum do caminho emu68.

`AI_context/issues/ISSUE-0061.md:50-54` testou o mesmo padrão
`frame=100 → frame=500` — mas em **multicore**, onde `int32`/`ipl`
**mudavam** de estado normalmente (IRQ chegando de verdade), e concluiu
"não há bloqueio conhecido remanescente por trás desse sintoma". Essa
conclusão não vale pra single-core — nunca foi retestada lá.

# Diagnóstico

O bug parece ser **específico do caminho single-core**, não uma
regressão desta sessão nem do backend Emu68 em geral:

- Multicore (ISSUE-0061, 16/07): IRQ entregue corretamente, frame avança
  com `int32`/`ipl` mudando.
- Single-core (esta sessão, 17/07): IRQ nunca entregue, CPU trava em
  exec idle indefinidamente.

Isso aponta pro caminho de entrega de IPL específico de single-core
(`bellatrix_machine_sync_ipl`, `PAL_IPL_Set`, o hook de
`bellatrix_emu68_report_jit_progress`/`bellatrix_emu68_publish_cpu_progress`
em `bellatrix.c`) como suspeito — não o Rigel em si (que está gerando VBL
normalmente, per frame counter avançando).

# Impacto

**Bloqueia ISSUE-0063 (investigação de performance) em single-core**: não
há como medir custo de sincronização CPU↔chipset de forma significativa
se a CPU nunca sai do idle loop — qualquer captura de profiler feita
depois desse ponto só mediria o custo do próprio loop de poll de INTREQ,
não uma carga real de jogo/OS. A Fase 1/2 do ISSUE-0063 deveria ser
refeita em **multicore** até este bug ser corrigido, ou este bug precisa
ser resolvido primeiro.

# Fragilidade arquitetural exposta (observação do Jaime)

O fato de esse bug existir **só** em single-core e não em multicore não é
só "onde o bug mora" — é sintoma de um problema estrutural mais profundo:
**a entrega de IRQ não é um caminho único e topology-agnostic**. Existem
dois mecanismos de entrega de IPL diferentes — um para single-core (hook
direto de `bellatrix_emu68_report_jit_progress` no mesmo core que roda a
CPU) e outro para multicore (publish cross-core via bridge) — em vez de
uma única rota testada, com o número de cores sendo só um detalhe de
agendamento por baixo. Isso é o oposto do princípio já registrado em
`CLAUDE.md` ("Paula owns INTREQ/INTENA... a chipset owns observable
time"): a entrega deveria ser uma decisão só do chipset/host, indiferente
a quantos cores físicos existem.

Consequência prática já demonstrada: o ISSUE-0038 achou o sintoma
(`pc=fc0f90`, VERTB nunca entregue), o ISSUE-0061 retestou o padrão
`frame=100→500` **só em multicore**, viu IPL mudando normalmente, e
encerrou a investigação como "sem bloqueio remanescente" — sem perceber
que só tinha validado metade dos dois caminhos existentes. O bug ficou
escondido por dias porque não havia uma única rota pra validar as duas
topologias de uma vez.

**Implicação pra correção**: quando isso for resolvido, o alvo certo não
é só "consertar o caminho single-core" — é avaliar se dá pra unificar os
dois caminhos de entrega de IPL (ou, na pior hipótese, garantir que os
dois sejam exercidos pelos mesmos testes/critérios de aceite sempre,
nunca um sozinho) pra que esse tipo de divergência silenciosa não se
repita.

# Hipóteses descartadas (por leitura de código / teste, não suposição)

1. **IPL-write não-fiado em single-core** — DESCARTADO. `machine_publish_ipl`
   (`machine_rigel_trace.c:818`) chama `m->cpu_backend->set_ipl`;
   `m->cpu_backend` É setado (`bellatrix.c:802,812` →
   `bellatrix_machine_init`), `s_backend.ready=1` roda em fault-driven
   (`emu68_backend.c:381`, log confirma `[EMU68-MACHINE] native
   fault-driven execution selected`), e `emu68_machine_set_ipl` →
   `emu68_machine_platform_set_ipl` escreve `__m68k_state->INT.IPL` +
   `sev`. Path completo.
2. **`M68K_LoadContext` revertendo `INT.IPL`** — DESCARTADO.
   `M68K_LoadContext` (`emu68/src/aarch64/start.c:1560`) não toca
   `INT32`/`INT.IPL` (só CACR/USP/ISP/MSP/INSN_COUNT/FP*/D*/A*/PC/SR). O
   write de IPL dentro de `report_jit_progress` (entre Save/LoadContext)
   sobrevive.
3. **Gate de entrega `INT32 != 0`** — o bloco inteiro de entrega de IRQ no
   `MainLoop` (patch 0003) é gated por `if (unlikely(ctx->INT32 != 0))`.
   Como `INT.IPL` é byte 2 de `INT32` (union em `M68k.h:108-116`), escrever
   IPL≠0 torna `INT32 != 0` → o gate dispara. Então o write, SE acontece,
   é entregue. Não é o furo.
4. **Submódulo emu68 com resíduo de patch stale (API 0025-0034 removidos)**
   — DESCARTADO empiricamente. Reverter todos os patches emu68 atuais
   deixa a árvore do submódulo IDÊNTICA ao commit pinado (`git diff` vazio).
   Não há código de API órfão compilado. (Bom instinto do Jaime dado o
   padrão de estado-velho da sessão — build dir stale, flag de profile —
   mas aqui não procede.)

## Divergência estrutural confirmada single vs multicore

- **Multicore**: IPL entregue via `core_chipset.c:934
  core_chipset_set_pending_ipl → PAL_IPL_Set` (caminho NATIVO — escreve
  `INT.IPL` + `dmb ish` + `dsb sy` + `sev`). O `machine_publish_ipl →
  set_ipl` da API é secundário/redundante.
- **Single-core**: IPL entregue SÓ via `machine_quantum_step →
  machine_publish_ipl → cpu_backend->set_ipl → emu68_machine_platform_set_ipl`
  (caminho da API). Nunca passa por `PAL_IPL_Set`.

Os dois writes atingem o mesmo `__m68k_state->INT.IPL`; a diferença é
barreira (single-core não deveria precisar) — por isso não consegui
provar POR leitura por que o caminho da API falha. Instrumentação
`[IPL-DIAG]` adicionada pra medir em QEMU se `machine_publish_ipl` é
sequer CHAMADO com IPL≠0 em single-core (distingue "Rigel nunca levanta
IRQ" de "levanta mas não entrega"). Alinhado com a direção do Jaime
("jogar a API no lixo, manter só cycle stats"): se confirmar que o caminho
da API é o problema, a correção é single-core usar `PAL_IPL_Set` nativo
como o multicore.

## Medição em QEMU (frame 100, single-core) — [IPL-DIAG]

```
[EMU68-LIVE] frame=100 pc=00fc0f90 ... stopped=0 int32=00000000 ipl=0 insn=4310145
[IPL-DIAG] publish_calls=649 nonzero=123 max_ipl=3 cur_INT.IPL=0
```

**Decisivo**: o IPL É publicado não-zero 123 vezes (max=3 = VERTB). Então
NÃO é "Rigel nunca levanta IRQ" — o IPL é gerado. Fica entre "publicado mas
não entregue" (contador `delivered` vai dizer).

## PC parado é `STOP #$2000`, não busy-loop

Disassembly do KS1.3 em `0xfc0f90` (offset ROM `0x40f90`):
`4e72 2000` = **`STOP #$2000`** (idle Wait do Exec), seguido de `60e6` =
`BRA.S` de volta. O `stopped=0` do checkpoint é ESPERADO: o `EMIT_STOP` do
BELLATRIX (patch 0020, `M68k_LINE4.c`) de propósito NÃO seta a flag
`STOPPED` ("leaving STOPPED unset here is inert"). O guest está de fato no
STOP, re-despachando.

Lógica do STOP (patch 0020): `ldr INT32; cbnz → retira STOP e deixa
MainLoop entregar; senão credita 8 insns falsas e re-despacha`. Como
`INT.IPL` é byte de `INT32`, publicar IPL=3 torna `INT32 != 0` → o próximo
dispatch do STOP deveria acordar e entregar. Se `delivered` ≈ 0 apesar de
`nonzero=123`, o handshake STOP↔IPL está furado em single-core (o
`cbnz INT32` nunca coincide com `INT32 != 0`, provável se publish(3) e
publish(0) acontecem na mesma passagem do hook antes do STOP re-despachar).

## CAUSA RAIZ (confirmada por medição): STOP não credita CYCLE_COUNT

> **Superseded pela reavaliação de 2026-07-17 ao final deste arquivo.** A
> medição posterior mostrou `CYCLE_COUNT` avançando; esta hipótese não explica
> sozinha a regressão single-core.

Medição com contador de entrega em QEMU (frame 100, single-core):
```
[IPL-DIAG] publish_calls=649 nonzero=123 max_ipl=3 cur_INT.IPL=0 delivered=14
```
Entrega ACONTECE mas STARVED: 14 exceções tomadas em 100 frames rigel
(deveria ser ~100+, uma VBL por frame). Não é "não entrega" nem "não
levanta" — é **inanição**.

**Mecanismo**:
1. Guest executa `STOP #$2000` (idle Wait do Exec em `0xfc0f90`).
2. O avanço do chipset (`bellatrix_emu68_report_jit_progress`,
   `bellatrix.c:457`) computa o quanto avançar a partir do **delta de
   `CYCLE_COUNT`** (`__m68k_state->CYCLE_COUNT`).
3. Mas `EMIT_STOP` (patch 0020, `M68k_LINE4.c`) credita **`INSN_COUNT`
   (v30)**, NÃO `CYCLE_COUNT`. Enquanto o guest está em STOP, `CYCLE_COUNT`
   fica CONGELADO → delta=0 → `publish_cpu_progress(0)` retorna cedo →
   **o chipset não avança durante o STOP**.
4. Sem avanço de chipset, sem VBL. Sem VBL, o `STOP` (que só acorda com
   IPL>0) nunca acorda. Deadlock de inanição.
5. Só "vaza" (14×) porque cada handler entregue roda instruções reais
   (CYCLE_COUNT avança um pouco), cruzando a próxima VERTB — mas devagar
   demais.

**Por que multicore funciona**: Core 2 avança o Rigel por relógio de
parede (250 kHz), INDEPENDENTE de a CPU estar em STOP. VBL sempre chega.

**Regressão introduzida por**: o refactor de modeled-cycles (patch 0035 /
commit `532fcd3`, "account modeled cycles at instruction boundaries") —
exatamente a "parte de estatística de ciclos" que o Jaime queria manter —
trocou o driver do chipset de INSN_COUNT para CYCLE_COUNT. Mas o
`EMIT_STOP` (do fix antigo do ISSUE-0038, `0e113d2`) ficou creditando
INSN_COUNT. Os dois se desacoplaram silenciosamente. Isso valida a
suspeita do Jaime de que o trabalho de API/cycle-stats introduziu a
regressão de single-core.

## Fix aplicado (C-side, seguro — sem tocar na emissão de JIT)

`bellatrix_emu68_report_jit_progress` (`src/cpu/emu68/bellatrix.c`): quando
`delta` de CYCLE_COUNT é 0 mas houve delta de INSN_COUNT (o crédito idle do
STOP), sintetiza `delta = insn_delta * 8` (modelo "8 CCK/insn", coerente
com o comentário do próprio EMIT_STOP "8 insns = 64 M68K cycles"). Mantém
CYCLE_COUNT (a estatística de ciclos) intacto e restaura o handshake
STOP↔chipset. Não mexe no EMIT_STOP (cujos offsets de branch hardcoded o
próprio patch avisa serem perigosos de editar).

# O que falta fazer

1. Confirmar se o mesmo teste em multicore (`BELLATRIX_MULTICORE_BUILD=1`)
   com a imagem atual (pós ISSUE-0063, `BELLATRIX_PROFILE=1`) ainda
   entrega IRQ corretamente — repetir a validação do ISSUE-0061 com o
   estado de código atual.
2. Seguir a frente de investigação já apontada pelo ISSUE-0038: auditar a
   janela `INT32`/`IPL` — quando `PAL_IPL_Set` escreve `INT.IPL` vs
   quando o `MainLoop` checa (fronteiras de translation unit) vs quando o
   `STOP` consome. Procurar a janela em que IPL sobe e desce sem o
   MainLoop ver (VERTB "perdido").
3. Comparar com o caminho Musashi single-core (`m68k_set_ipl`), que
   segundo o ISSUE-0038 "funciona no mesmo chipset" — se Musashi
   single-core entrega VERTB corretamente e Emu68 single-core não, isola
   o bug pro lado do hook de progresso/IPL específico do Emu68, não do
   Rigel/chipset.

# Reavaliação após retirada do adaptador da API (2026-07-17)

O adaptador Emu68 foi reduzido novamente à semântica nativa pré-API:
`get_pc` lê `__m68k_state`, IPL usa `PAL_IPL_Set()` e a execução/reset
continuam pertencendo ao próprio Emu68. Isso **não** reintroduz o memory map
antigo: as regiões diretas continuam instaladas e removidas pelo mecanismo
moderno baseado em `BellatrixDirectRegion`. Em QEMU, a Z2 Fast RAM confirmou
esse caminho:

```
[Z2-RAM] mapped ... guest=00200000-009fffff
```

A medição também corrigiu duas conclusões prematuras deste documento:

- `CYCLE_COUNT` não está congelado durante a janela observada
  (`CYCLE_COUNT=13080438`, `INSN_COUNT=2005485` no frame 100). O retorno ao
  modelo pré-API de `INSN_COUNT delta * 8` é necessário para a semântica do
  STOP, mas sozinho não restaura o boot single-core.
- o contador de entrega inicialmente publicado como zero não estava sendo
  incrementado no `ExecutionLoop.c` gerado. Depois de ligar a instrumentação
  tanto no patch fonte quanto no arquivo gerado, a baseline direta confirmou
  27 interrupções realmente aceitas até o frame 100.

O cenário confirmado é uma perda de borda no avanço síncrono: até o frame
100 o chipset publicou IPL não-zero 254 vezes (máximo 3), mas MainLoop aceitou
somente 27. Uma primeira tentativa de interromper o laço de quanta ao observar
`current_ipl != 0` não alterou a medição e foi removida. Isso mostra que o
assert/deassert pode ocorrer dentro do próprio passo entregue ao Rigel, antes
de a checagem externa recuperar o controle.

Um handshake experimental no adaptador nativo elevou a medição de 27 para 51
entregas no frame 100, sem regressão no mapeamento Z2, mas não alcançou o
checkpoint de frame 500 nem demonstrou boot. Ele foi posteriormente retirado
após a evidência cruzada com Musashi abaixo mostrar que não poderia atingir a
causa compartilhada. O ISSUE permanece aberto e não se deve registrar o
single-core como restaurado.

## Evidência cruzada Musashi (informação do Jaime)

O single-core também está quebrado com Musashi. Isso descarta a classificação
do defeito como específico do Emu68, do seu fault handler ou do handshake
interno do `MainLoop`. O handshake experimental descrito acima foi retirado:
ele atacava um sintoma observável do Emu68, mas não poderia corrigir a causa
compartilhada.

O conjunto mínimo comum passa a ser:

```
CpuBackend::run/progress
  -> bellatrix_machine_advance_cpu_cycles()
  -> bellatrix_machine_advance()
  -> machine_step_components()
  -> Rigel / timeline / publicação de IPL
```

A próxima comparação deve instrumentar esse corte uma única vez e executar a
mesma ROM com Musashi e Emu68. Diferenças internas dos adaptadores deixam de
ser hipótese primária; diferenças single-core versus multicore no avanço da
máquina/timeline passam a ser o foco.

## Causa compartilhada confirmada: duas autoridades de tempo

A instrumentação comum `[SC-PROGRESS]` reproduziu Musashi single-core e
mostrou que ele não estava parado no backend: avançou de `PC=$fc00de` para
`PC=$fc060e`. Entretanto, quando a CPU havia fornecido apenas 1.151.654 ciclos
(aproximadamente 575 mil CCK após a conversão 2:1), o Rigel já estava em
21.252.441 CCK e no frame 300 — cerca de 37 vezes à frente.

O motivo estava no caminho compartilhado:

1. `cpu_backend_run_selected()` / o report hook Emu68 avançava Rigel
   sincronicamente por `bellatrix_machine_advance_cpu_cycles()`;
2. `PAL_Runtime_Poll()` chamava também `bellatrix_runtime_chipset_step()`;
3. com timeline `realtime`, essa segunda rota levava o mesmo Rigel até o
   horizonte de parede, mesmo que a CPU guest ainda estivesse no início.

Assim, single-core tinha simultaneamente uma autoridade CPU-driven direta e
uma autoridade realtime do runtime. A chamada de chipset foi retirada de
`PAL_Runtime_Poll()`; ele continua responsável pelo host reactor/IO. Multicore
continua avançando pelo loop próprio do Core 2. Essa correção é comum a
Musashi e Emu68 e não altera fault routing nem memory map.

### Validação após retirar a segunda autoridade

Musashi single-core passou a apresentar a relação esperada:

```
frame=100 cpu_cycles=13874541 tick=7082400 pc=00fd16e4
```

Ele saiu do delay inicial, desligou overlay e configurou a Z2 Fast RAM. Antes
da correção, no mesmo frame 100 havia somente 384.860 ciclos de CPU e o PC
continuava em `$fc00de`. Portanto a regressão compartilhada de tempo está
confirmadamente corrigida. Jaime também confirmou em hardware que o Musashi
single-core voltou a funcionar com essa correção; ela não é apenas um
resultado do QEMU.

Emu68 também passou a apresentar a relação correta e configurou Z2:

```
frame=100 cpu_cycles=14156112 tick=7085805 pc=00fc0f90 IPL=3
```

Ele ainda não sai do STOP do Exec em QEMU: IPL 3 permanece pendente, SR tem
máscara 0 e somente 28 entregas foram confirmadas até o frame 100. Portanto,
depois de remover a regressão comum, resta uma segunda falha realmente
específica da entrega STOP/IPL do Emu68. As duas causas não devem voltar a ser
misturadas.

# Observações

Não commitar `emu68.txt` (arquivo de captura solto na raiz do repo,
untracked) — mantido como referência da sessão, mas é log bruto, não
pertence à árvore versionada.
