---
id: ISSUE-0031
title: "Harness Musashi sem FPU: crasha Line-F em 020/030/040 — bloqueia visual do ArosOne"
status: open
priority: high
type: bug
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
tags:
  - harness
  - musashi
  - fpu
  - aros
  - hdf
related_files:
  - tools/harness/main.c
  - external/musashi
  - patches/0013 (FSAVE/FRESTORE addressing modes)
---

# Sintoma

Com o HDF gerado da aros-amiga-m68k.iso (iso2hdf, ISSUE-0029):

- `HARNESS_CPU=68020`: boota até o Workbench Screen, depois **Software
  Failure — Line 1111 (F) Emulator/Coprocessor error** no task
  "Lib & Dev Loader Daemon" (binário do CD usa instruções FPU).
- `HARNESS_CPU=68030`: mesmo Line-F (vector 0x2c), cai no SAD.
- `HARNESS_CPU=68040`: harness **sai com exit=1 sem mensagem** logo após
  `InitCode leave (0x04)` / setpatch.library — abort silencioso, sem
  "[HARNESS] Done". Investigar separadamente (pode ser trap não tratado
  no backend Musashi 040).

# Objetivo

- Habilitar FPU no Musashi para 020(+68881)/030/040 no harness
  (M68K_EMULATE_FPU / softfloat do Musashi), validando com o boot completo
  do desktop AROS a partir do HDF.
- Root-cause do abort silencioso em 68040.

# Reprodução

```
python3 tools/hdf/hdf.py create aros.hdf 400Mi
python3 tools/hdf/hdf.py iso2hdf src/disks/aros-amiga-m68k.iso aros.hdf
KICKSTART=src/roms/aros.rom HDF=aros.hdf HARNESS_CPU=68020 FRAMES=1500 ./run.sh harness
```

# Progresso (2026-07-03, sessão HDF)

- O crash Line-F/condição-inválida era **red herring de FPU**: o dump ao
  redor do PC (0x00C1C102) mostrou tabela de ponteiros (0x00F8xxxx ROM,
  0x00C1C064 RAM) — a CPU saltou para dados, provável LVO/ABI errado.
- **Causa raiz do crash com aros.rom**: mismatch de versão. `aros.rom` é
  build 30.7.2025; `aros-amiga-m68k.iso` é build 12.5.2026 (mesma data do
  `new_aros.rom`). Nightlies do AROS não têm ABI estável entre builds.
- Com **new_aros.rom + aros_iso.hdf (68040)** boota até o shell de boot e a
  Startup-Sequence roda; para em `FixFonts: Could not open "stdc.library"`
  (a lib ESTÁ no HDF, 382KB — suspeita de falta de RAM: só 2MB chip +
  1.5MB slow; `bellatrix_zorro2_enable_fast_ram()` existe mas só é chamada
  no backend emu68, não no harness). Próximo passo: fast RAM no harness.
- MaxTransfer do RDB ajustado para 0x1FE00 no aros_iso.hdf (não era a
  causa, mas é o valor seguro; considerar default na tool).
- Patch **0014-musashi-fpu-test-condition-mask**: predicados >=0x20
  passam a mascarar para 5 bits com warning em vez de exit(1) do host
  (o abort silencioso do 68040 era esse exit(1) via fatalerror).

# Progresso 2 (2026-07-03, fast RAM)

- Harness ganhou **8MB de Zorro II fast RAM** via autoconfig (default ON,
  `HARNESS_FASTRAM=0` desliga). Registrado em tools/harness/main.c;
  backing no próprio backend (tools/harness/musashi_backend.c).
- Bug encontrado e corrigido: a janela de RAM respondia antes do
  autoconfig → exec duplicava a faixa na memlist ("Sanity check on memory
  list failed", "MemHeader 0x00200000 (0xFFFFFFFF)"). Regra consolidada em
  AI_context/consolidated/memory_model.md.
- `dir2hdf` adicionado ao tools/hdf (árvore de diretório → HDF, sanitização
  em cópia). ArosOne-68K (494MB) → arosone.hdf 700MB.
- ArosOne boota no 68040 com fast RAM sem erros de memória, carrega a
  stack USB da distro, mas desktop não aparece — suspeita: distro
  configurada para RTG (WinUAE usa uaegfx + 512MB VRAM); forçar screenmode
  PAL nativo é o próximo experimento.

# Confirmado bloqueando o objetivo do RTG (2026-07-03)

Rodando `arosone.hdf` + `aros.rom` (68040) com o board RTG registrado
(fase 2, ISSUE-0033), o boot chega a tentar carregar bibliotecas/daemons
e crasha em loop:

```
Software Failure!
Task : ... - Lib & Dev Loader Daemon
Error: 0x8000000B - Line 1111 (F) Emulator/Coprocessor error
PC   : 0xFFFFFD60
```

O daemon reinicia, crasha de novo, indefinidamente. Cada crash tenta
desenhar o alerta de Software Failure na tela — isso aciona
`AmigaVideoBM__Root__New` (bitmap do Denise nativo), que também falha
("superclass failed to instantiate a suitable bitmap"), então nem o
alerta aparece visualmente (só via serial). **O bitmap failure é
sintoma do crash tentando se renderizar, não causa raiz** — não é bug
do RTG nem do AmigaVideo em si.

**Conclusão**: o gap de FPU do Musashi é hoje o bloqueio primário para
ter feedback visual do ArosOne (objetivo da ISSUE-0033), não o
FindCard do p96gfx incerto. Mesmo com RTG 100% funcional, esse loop de
crash provavelmente impediria uma tela estável. Prioridade elevada.

Descartado como causa nesta investigação: aliasing de endereço Z3
(ISSUE-0032) — tinha um bug real e não-determinístico (corrigido,
commit 8e9ccbd), mas não é o que causa este crash especificamente
(reproduz igual com o fix aplicado).

# Mecanismo raiz: AROS assume FPU 68040 nativa, sem fallback de software (2026-07-03)

Achado em `external/aros/arch/m68k-amiga/boot/cpu_detect.S` +
`boot/start.c:GetAttnFlags()`:

- `cpu_detect_fpu_asm` sonda FPU via `fnop` + `fsave (a0)`, lendo o byte
  de formato do frame salvo. Se a sonda tiver sucesso (nenhum trap
  Line-F durante `fnop`/`fsave`), AROS conclui que HÁ FPU.
- `GetAttnFlags()`: se CPU é 68040/68060 E FPU foi detectada, seta
  `AFF_FPU40` (assume FPU **integrada nativa**) — `AFF_68881`/
  `AFF_68882` (que indicariam *emulação por software*, carregando
  `mathieeedoubbas.library` ou similar) só são setados nos ramos SEM
  68040/68060.
- **Consequência**: como o Musashi já implementa `FSAVE` (patch 0013),
  a sonda passa, `AFF_FPU40` liga, e o AROS nunca carrega fallback de
  software. Qualquer instrução FPU que o Musashi não implemente depois
  disso vira crash fatal direto — sem rede de segurança. Isso explica
  por que só agora (68040, RTG fase 2 destravando mais boot) o gap
  apareceu: em outros caminhos/CPUs o software fallback cobria.

**Não é uma correção fácil de "fingir sem FPU"**: exigiria interceptar
especificamente essa sonda sem quebrar `FSAVE` real (usado em outros
pontos, motivo do patch 0013 existir). Caminho correto continua sendo
identificar e implementar o(s) opcode(s) FPU específico(s) faltando.

## Diagnóstico em andamento

Instrumentado `m68ki_exception_1111()` (external/musashi/m68kcpu.h,
edição local não commitada ainda) para logar `[F-LINE-TRAP] pc=... ir=...
opcode_bytes=...` no fault real. Confirmado: nenhum dos 35 `fatalerror()`
dentro de `m68kfpu.c` foi atingido em nenhum teste (mataria o processo,
exit≠0 — nunca aconteceu) — ou seja, o crash não é um caso não-tratado
DENTRO do dispatch de FPU (`fpgen_rm_reg` etc.), é um opcode Line-F que
**nem chega** a ser reconhecido como FPU pela tabela do Musashi (só
`0xF2xx`/`0xF3xx` roteiam para `m68040_fpu_op0/op1`; qualquer outro
padrão cai direto em `m68ki_exception_1111`). Suspeita: instruções de
cache do 68040 (CINVL/CPUSHL, também Line-F) usadas por loaders de
biblioteca ao invalidar i-cache — não aritmética de FPU de verdade.

Reprodução automatizada (headless) trava ANTES do crash real (loop de
poll determinístico em PC=0x00fe849a, não relacionado — resolve sozinho
depois de muitos frames, >45000 mas <150000 aparentemente). Run longo
em andamento para capturar o opcode exato.

# Reprodução confiável encontrada + separado do RTG (2026-07-03, tarde)

Reprodução rápida e determinística (~1min, headless):
```
KICKSTART=src/roms/new_aros.rom HDF=src/disks/arosone.hdf HARNESS_CPU=68040 FRAMES=4000 ./run.sh harness
```
(`HARNESS_RTG=1` opcional — **crash idêntico com ou sem RTG**, confirmado
por teste de controle. Não é bug do RTG.)

Achado real: `[F-LINE-TRAP] pc=00200144 ir=ffff opcode_bytes=ffff 00fa`.
`ir=0xFFFF` **não é opcode de FPU real** — é padrão de barramento
aberto/memória não escrita. Confirmado (instrumentação `[FR-DBG]`) que a
leitura passa corretamente pela janela de fast RAM configurada
(`fr_ok=1 match=1`) — ou seja, o conteúdo REAL de `fast_ram[0x144..0x145]`
é `0xFF 0xFF`. Fast RAM é `memset(0)` na inicialização (memory.c:88), não
`0xFF` — então algo grava `0xFF` ali durante o boot, ou o AROS pula para
um endereço nunca populado com código real.

Hipótese mais provável: `new_aros.rom` é build nightly/dev; padrão 0xFF
é assinatura comum de poison-fill de alocador debug (tipo AROS_MUNGWALL)
em memória liberada — sugere possível use-after-free ou load de
biblioteca que falha silenciosamente, do lado do AROS. Não necessariamente
corrigível pelo nosso lado.

Variação: com `HARNESS_MSGPORT_OWNER_FIX=1` (env específico do
new_aros.rom, ver bellatrix-workflow-preferences), o sintoma muda para
`Illegal instruction` em `Exec Bootstrap Task`, incluindo um crash
secundário com `PC=0x00000010` (endereço do próprio vetor de exceção —
sugere possível corrupção da vector table ou double-fault). Não
investigado a fundo ainda.

**Conclusão da sessão**: RTG (board+card+DiagArea, ISSUE-0033) está
validado até o ponto que testamos; o bloqueio para feedback visual do
ArosOne não é causado pelo RTG — é esse crash separado e anterior no
boot do `new_aros.rom`. Com `aros.rom` (build mais antigo) o boot trava
em ponto diferente e não relacionado (loop de poll em PC=0x00fe849a,
resolve sozinho em modo interativo após muitos frames, nunca em
headless — ver ISSUE-0033).

# Referências para a lacuna maior de FPU (fora do escopo do crash acima)

- https://github.com/nonarkitten/femu — emulador de FPU 680x0 em
  assembly, guest-side (roda NO Amiga, como mathieeedoubbas). Licença
  "uso pessoal", alpha (v0.14). Não é código pra portar pro Musashi;
  seria uma biblioteca a carregar no lado guest — caminho alternativo a
  "implementar opcodes no Musashi", mas exigiria mexer no boot do AROS
  para carregá-la (ou empacotar no nosso HDF).
- `emu68/src/M68k_FPU.c` (1069 linhas) — o PRÓPRIO Emu68 tem emulação de
  FPU em C, testada em produção (roda software Amiga real no Raspberry
  Pi). Candidato natural a referência/reaproveitamento para preencher
  os gaps do m68kfpu.c do Musashi, muito mais maduro que escrever do
  zero. Avaliar antes de qualquer trabalho futuro de FPU no harness.
