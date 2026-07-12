# Multicore + Arbiter — backlog e separação de escopo

> **DOCUMENTO HISTÓRICO — SUPERSEDED em 2026-07-12.** Este backlog não é uma
> fila ativa. O único tracker operacional da frente é ISSUE-0052.

## Status: ativo (2026-07-10)

Persiste o backlog de trabalho iniciado na sessão de 2026-07-10 (antes só num
tracker de sessão, volátil). Separa o que é **específico do multicore** (branch
`wip/multicore-runtime`) do que é **específico do Emu68/JIT** (pertence à branch de
AROS-bootando-com-Emu68, onde as APIs públicas serão mergeadas). A fronteira é
tênue, mas útil: o JIT trava num bug JIT+FPU+Z3 comum a single e multicore, então
o trabalho de multicore avança com o backend **Musashi** (que roda estável).

Docs de detalhe: `[[issue_core0_arbiter_scheduler]]`,
`[[issue_emu68_pistorm_interrupt_contract]]`, `[[issue_multicore_runtime]]`,
`[[emu68_public_api]]`.

## Multicore-específico (branch wip/multicore-runtime)

| # | Item | Estado |
|---|---|---|
| 1 | **Contrapressão Core1↔Core2** (teto de backlog CCK) | ✅ feito e validado (Musashi): divergência ilimitada 452M → limitada ~0 |
| — | **Log-sink multicore-friendly** (buffer de linha por core em `console_log.c`) | ✅ feito: log garbled → limpo, destravou diagnóstico |
| 8 | **Reboot loop multicore Musashi + wb13** (Workbench não boota) | investigado, causa não fixada — ver abaixo |
| 4 | `rigel_next_event_tick()` — chipset expõe próximo evento | pendente (testável em Musashi) |
| 6 | **Arbiter por deadline** — epoch/rendezvous troca quantum fixo + lock por acesso | pendente (bloqueado por #4 e, pro caminho JIT, por #2) |
| — | Follow-ups da Fase 1: pico isolado de backlog (~753K); **kprintf sem lock entre cores** (log garble); afinar teto 8192 | pendente |

Aterrissado nesta branch (working tree, não commitado):
- `src/launcher/launcher.c` — `wait_ack` 400× menor (loop QEMU rápido)
- `src/cpu/emu68/bellatrix.c` — supervisor no Core 0 (`[CORE0-SUP]`) + log `Core0=Supervisor...`
- `src/runtime/core_chipset.c` — contrapressão (`s_chipset_cck` atômico, `CHIPSET_MAX_BACKLOG_CCK=8192`, SEV do Core 2)

## Emu68/JIT-específico (branch AROS-on-Emu68, não esta)

| # | Item | Estado |
|---|---|---|
| 5 | Verificar se o build Emu68/JIT sobe ROM | ✅ feito: **não sobe** |
| 7 | **Crash JIT: FMOVE FPU → 0xFFFFFFF6 (Z3)** | pendente — bug raiz do JIT |
| 2 | Emu68 `run_until` + saída cooperativa (API faltante) | pendente — prereq do arbiter no caminho JIT |
| 3 | Monopólio de IRQ do Emu68 / device IRQ áudio | pendente — Fase 6, guiado por medição |

### #7 detalhado (o bloqueador do JIT)
Emu68/JIT + KS13 + wb13.adf trava numa `FMOVE from SPECIAL` no M68K PC=0xFC1682
(ROM KS13, 68000) sob config 68040+FPU. **Não é multicore-específico** (bisect):
- multicore: data abort logado, `write to 0xFFFFFFF6`, ESR=0x96000046, CPU congela;
- single-core: trava **silenciosa** no mesmo FMOVE (sem fault logado).
Duas camadas: (a) alvo é Zorro III/32-bit >0xFFFFFF que o bus não expõe (ISSUE-0032);
(b) a FMOVE estendida (12B, emitida como store NEON de 16B) não é servível pelo
fault path de 1/2/4 bytes → "Unhandled". Direções: rotear/ignorar store estendido
não-mapeado como open-bus; ou expor Z3; ou investigar o EA selvagem.
Relacionado: ISSUE-0032, ISSUE-0034/0035, `bellatrix-musashi-fpu`.

### #8 detalhado (reboot loop — bug multicore atual)
Multicore Musashi + KS13 + wb13.adf: **re-init determinístico a cada ~2 frames
Amiga** (~5.4M CCK). Contrapressão segura, sem crash. Via RigelTrace:
- boot completo do host (banner, Waking CPU x3, EMU68-API) ocorre **1×**; só o
  miolo de `bellatrix_init` re-executa (rom_mapped x5, overlay, Rigel init →
  `[RIGEL-VBLK] frame` volta a 1, ADF re-inserido). **Não** é reboot completo.
- `bellatrix_init` tem caller único (`start.c:1542`) sem loop → gatilho é caminho
  de re-init parcial não fixado (candidatos: `bellatrix_machine_reset`
  `machine_rigel.c:258`, toggle de overlay, ou reset dirigido pelo M68K).
- **disco nunca lido** (sem `[RIGEL-FLOPPY]`); VBL dispara mas `intreq=0000`;
  vetores de exceção lidos como zero.
- fragilidade conhecida (`bellatrix.c:757-761`): VBL handler do KS13 instala vetor
  ruim em 0x6c se SysBase/(4).W estiver com lixo.
- **hipótese**: ordering de MMIO crítico CPU↔chipset sob multicore (a lacuna
  "MMIO crítico barrier" de `[[issue_multicore_runtime]]`) corrompe o early boot
  do KS13 → loop antes de interrupções/disco. Mesma classe que o arbiter (#6)
  ataca. Próxima sonda: (a) trace de PC do Musashi no instante do re-init;
  (b) instrumentar `bellatrix_machine_reset`/overlay; (c) implementar barreira de
  MMIO crítico/arbiter e re-testar.

### Checkpoint 2026-07-10 — ownership do Rigel, overlay e IPL corrigidos

A investigacao mostrou que o aparente re-init continha dois problemas distintos:

- O console multicore usava `volatile` para os indices do ring. O consumidor
  podia observar `head` antes do payload em AArch64, reproduzindo bytes antigos,
  NULs e blocos inteiros de boot. `head`/`tail` agora usam release/acquire.
- Havia uma violacao real de ownership: `bellatrix_machine_read/write()` chamava
  `machine_flush_for_bus()` tambem no multicore. Assim Core 1 executava
  `rigel_step()` concorrentemente com `rigel_step_until()` no Core 2. O flush
  sincrono agora e exclusivo do single-core; Core 2 participa do lock de acesso
  e e o unico stepper do Rigel.

Outras correcoes encontradas na mesma fronteira:

- Musashi lia CIA diretamente para decidir overlay. A escrita CIA-A PRA agora
  publica OVL no dispatch comum; KS13 deixa de reler o vetor ROM em `(4).W` e
  nao retorna mais ao reset `FC00D2`.
- Core 2 chamava `m68k_set_irq()` enquanto Core 1 executava Musashi. IPL agora e
  publicado atomicamente e consumido pelo Core 1 entre quanta. Escritas MMIO que
  mudam IPL tambem atualizam a publicacao, evitando reaplicar um IPL antigo e
  entrar em tempestade espuria no dispatcher `FC0D26`.
- O supervisor agora inclui `frames` e `pc`, tornando progresso de CPU, chipset
  e video observavel sem trace por acesso.

Resultado QEMU multicore Musashi + KS13 + wb13: uma unica entrada em
`bellatrix_init`, OVL 1->0, AutoConfig Z2 completo, clocks/backlog monotonicamente
coerentes, 551 frames e ~39M CCK em 180 s; o PC segue avancando por codigo ROM
valido e nao retorna ao reset. Ainda falta confirmar a primeira leitura DF0 e o
Workbench visivel em hardware. O caminho Core 2 ainda nao encaminha seus
`rigel_step_result` ao RigelTrace, portanto ausencia de `[RIGEL-FLOPPY]` no log
bare-metal atual nao prova ausencia de atividade de disco.

Validacao: build multicore bare-metal passa; suite harness/Rigel 34/34 passa.

## Decisão atual (2026-07-10)
Seguir **multicore com Musashi** (opção conservadora). #7 e a superfície JIT ficam
para a branch de AROS/Emu68. Ao mergear aquela branch, reconciliar com a API
pública (`[[emu68_public_api]]`) e reavaliar #2.

### Checkpoint 2026-07-10 — boot por ADF confirmado no multicore

Comparação alinhada por tempo emulado entre harness, bare-metal single-core e
bare-metal multicore, usando Musashi 68000 + KS1.3 + `Workbenc13.adf`:

- harness: assinatura de leitura correta é dupla escrita `DSKLEN=9cbe`, DMA de
  14.716 bytes e IRQ DSKBLK;
- single e multicore bare-metal seguem a mesma sequência de init, overlay,
  AutoConfig e identificação do drive até ~15M CCK;
- multicore arma a primeira DMA em ~25,59M CCK (`DSKPTR=00392c`, dupla escrita
  `DSKLEN=9cbe`) e repete transferências subsequentes;
- em 300 s de QEMU o multicore alcançou 58,4M CCK / 825 frames, com backlog
  limitado (~8,4K CCK), uma única entrada em `bellatrix_init` e execução de
  código carregado do disco (`PC=00FFxxxx`).

Conclusão: as hipóteses antigas "DF0 nunca é lido" e "divergência antes do
bootblock" estão descartadas. O timeout curto apenas encerrava antes da primeira
DMA. A próxima comparação deve mirar o estágio posterior ao bootblock/trackdisk
e ser alinhada por CCK/eventos, não por tempo de parede ou frames absolutos.

Foi adicionada uma sonda compilável com `BELLATRIX_FLOPPY_BOOT_PROBE=1` que
registra, com limites de volume, DSKPTR/DSKLEN/DMACON, DSKBYTR/DSKDATR e linhas
CIAB do drive. Manter desligada em builds normais.
