---
id: ISSUE-0023
title: "CD/ISO no harness: KS2.0/KS3.1 precisam de atenção; AROS não chega ao workbook com ISO; boot KS20 trava em ROM"
status: open
priority: medium
type: bug
owner: unassigned
created_at: 2026-07-01
updated_at: 2026-07-02
tags:
  - cdrom
  - odfs
  - lide.device
  - ks20
  - ks31
  - aros
  - harness
  - audio
related_files:
  - external/ODFileSystem/platform/amiga/romtag.c
  - external/ODFileSystem/platform/amiga/startup.S
  - external/ODFileSystem/core/cache_block.c
  - src/machine/expansions/lide_cdrom/atapi_cdrom.c
  - src/host/posix/pal_posix.c
  - patches/0012-odfs-cd01-romtag.patch
---

# ESTADO ATUAL (2026-07-02, fim de dia)

**Os ícones do CD aparecem corretamente em KS2.0 + wb13.adf + ISO**
(validado interativamente pelo usuário). Mount único (CD0:), handler ODFS
enumera. Perf de leitura do CD degradada (setor a setor, sem read-ahead) —
aceito por ora. A issue permanece aberta apenas para: read-ahead corrigido,
AROS+ISO workbook (ABERTO 2) e ExNext O(n²).

# RESOLUÇÃO 2026-07-02 (tarde) — regressão dos ícones/CD encontrada

**Causa raiz da regressão: binário ODFS STALE embutido no harness.**
O CMake só tinha `handler_main.c`, `mount.c` e `iso9660.c` como DEPENDS do
build do ODFS. O revert do read-ahead em `core/cache_block.c` NÃO regenerou
o binário — o handler bugado (read-ahead) continuou embutido, fazendo o
revert parecer "insuficiente" e mandando a investigação para pistas falsas
(features ISO, patch 0011, PVD, rigel). Após `rm -rf build/amiga` + rebuild,
o mesmo conjunto de features volta a funcionar (mount + enumeração; usuário
confirmou o comportamento desejado de volta, interativo).

**Prevenção aplicada**: (1) `tools/harness/CMakeLists.txt` — DEPENDS agora
cobre todos os fontes do ODFS (glob core/backends/platform/include/Makefile);
(2) `scripts/build-odfs.sh` — `rm -rf build/amiga` antes de todo build
(objetos stale = binário misto que imita regressão).

**Ferramenta nova**: ODFS serial debug funciona no harness — editar
build-odfs.sh com `-DODFS_SERIAL_DEBUG=1 -DODFS_FEATURE_LOG=1` e
`SERIAL_DEBUG=1`; sai como `[SERIAL]` no stdout. ATENÇÃO: o 2º banco da ROM
serve NO MÁXIMO 32768 bytes — o binário normal tem 31632 (96,5% do limite);
com logging é preciso desligar features (ex.: ROCK_RIDGE/JOLIET) para caber.
Binário >32KB é truncado silenciosamente e o handler quebra de formas
bizarras.

**Perf conhecida e aceita por ora**: sem read-ahead o handler lê setor a
setor (count=1) — CD lento. Reintroduzir read-ahead SÓ com o bug corrigido
(underflow lba>=media_sectors era só parte; ver seção REVERTIDO) e com o
processo de build já blindado acima.

# Atualização 2026-07-02

**Causa dos "dois disquetes" ENCONTRADA e corrigida** (`ata_ide.c`):
o device ATAPI respondia como master E slave → lide criava 2 units → CD
montava duas vezes (CD0: + CD1:, confirmado no os_debug). Fixes:
(1) CD responde só como slave (device 1; master vazio → ABRT limpo, ou HD
quando HDF anexado — ver [[ISSUE-0025]]); (2) DEVICE RESET preservar o bit
DEV do dev_head — zerá-lo desviava o IDENTIFY PACKET para o master, pois o
lide usa shadow register e não re-seleciona.

**Hipótese "handler não roda" DERRUBADA**: com o fix, os 8 reads viram 4
(eram 2 units × 4) e são o mount completo do handler: PVD (16) + root dir
(105, 5754). O handler CD0 fica vivo em WAIT. O que nunca acontece é alguém
pedir disk.info/enumeração depois — investigar anúncio do volume ao
Workbench (AddVolume/diskchange).

**Reverts (pedido do usuário, 2026-07-02)**: read-ahead do cache ODFS
(cache_block.c etc.) e pacing de áudio do harness (pal_posix.c) foram
revertidos; patch 0012 regenerado só com o romtag CD01 (romtag.c +
startup.S + Makefile). Áudio: melhoria futura em issue própria.

# Contexto (sessão 2026-07-01)

Trabalho do dia sobre suporte a CD (ISO) no harness. Estado consolidado do que
FUNCIONA e do que está ABERTO.

## Consolidado (mantido, no patch 0012 + working tree)

1. **ROMtag CD01 no ODFileSystem** (`romtag.c` + `startup.S`): o bootldr do
   lide.device agora encontra um Resident no 2º banco da ROM e o init registra
   `FileSysEntry` DosType 'CD01' no FileSystem.resource (cria o resource se
   não existir). Sem isso, `FindCDFS()` nunca retornava true e o CD0: jamais
   montava (causa raiz da ISSUE-0022).
2. **Read-ahead no block cache do ODFS** (`cache_block.c`): miss busca até 8
   setores contíguos num único READ_10 (casa com DMA_BUF_SECTORS=8 do
   handler). Cuidado com `lba >= media_sectors` (underflow — já corrigido).
   Testes host todos passando, incl. novo `cache_readahead`.
3. **PVD "AMIGA BOOT" agora é opt-in** (`HARNESS_CD_BOOTABLE=1`,
   `atapi_cdrom.c`): por default o ISO NÃO é marcado bootável (bootPri=-1),
   para o fluxo "boot por ADF, CD como volume de dados".
4. **Logs por-comando do CD gateados** por `HARNESS_CD_TRACE=1` (default off).
5. **Pacing de áudio do harness** (`pal_posix.c`): WSLg/RDP suspende sinks
   silenciosos (~20s) e a fila SDL para de drenar; o throttle
   `SDL_Delay(excesso)` então colapsava a emulação para <1 fps (confirmado
   com `HARNESS_LOOP_PROF=1`: >1s/s de wall dentro do push de áudio).
   Política atual: delay limitado a 100ms; stall = 30 pushes (~3s) sem drenar
   com device despausado → drop+re-arm; 5 re-arms sem NENHUMA drenagem →
   desabilita áudio de vez. Qualquer drenagem observada zera os contadores
   (evita falso positivo no arranque — regressão reportada e corrigida).
   **ATUALIZAÇÃO (2026-07-02, sessão seguinte): essa política NÃO tinha sido
   commitada** — o `pal_posix.c` em HEAD só tinha o `SDL_Delay(excesso)` sem
   cap (commit cf9f573 "keep audio paced at realtime"). Sintoma reobservado:
   fps bom no início degradando até <1 fps, MESMO SEM ISO/HDF (lide
   comprovadamente isolado: `lide_cdrom_register()` só roda via
   insert/attach; romtag ODFS só carrega pelo bootldr da placa). Perfil
   confirmou: `audio=` crescendo de 0.07s/s até 1.9s/s de wall dentro do
   push. A política stall-aware (cap+watchdog) foi tentada e REJEITADA pelo
   usuário ("hacky", som pior e ainda degradava). **Resolução final: áudio
   revertido ao comportamento original pré-cf9f573** (fila SDL simples, sem
   pacing/throttle) com um único acréscimo: device abre pausado e só
   despausa na primeira amostra não-silenciosa (amostras de silêncio antes
   disso são descartadas) — sem áudio de fato, nenhum stream abre. Validado
   120s com HARNESS_LOOP_PROF: iters estáveis ~15-16k/s, audio=0.002s/s,
   sem degradação.
   **Diagnóstico final do som ruim (2026-07-02)**: modelo trocado para
   callback SDL + ring bounded (~743ms) com jitter cushion de 186ms
   (re-prime após underrun) — emulação nunca bloqueia, latência limitada.
   Instrumentação (`HARNESS_AUDIO_TRACE=1`) mostrou produção saudável
   (38-44k amostras/s) mas o callback SDL parando de ser chamado após ~21s.
   **Reproduzido com SDL puro (senoide 440Hz) fora do harness: o RDPSink do
   WSLg morre após ~21s de playback** (43 cb/s → ~1/s). Não é o
   module-suspend-on-idle (descarregado via libpulse, stall persiste);
   PULSE_LATENCY_MSEC não muda nada; só existe pulseaudio no WSL (sem ALSA/
   pipewire). Reabrir o device SDL ressuscita o sink mas ele re-morre a
   cada ~3s. Ambiente: WSL 2.9.3.0, WSLg 1.0.79. É bug do host — candidato
   a `wsl --update`; qualquer fix no harness seria paliativo (reopen
   automático em stall).
6. **Ferramentas novas** (env-gated, default off): `HARNESS_LOOP_PROF=1`
   (breakdown por segundo do loop interativo), frame= no log de READ_10.

## REVERTIDO — não reintroduzir sem resolver o namefix

Otimização O(n) do ExNext (resume offset no walk do iso9660/joliet + cache de
iteração por lock): **incorreta**. O `namefix` (renomeação determinística de
colisões) é stateful desde o offset 0 do diretório; começar o walk no meio
gera nomes/chaves inconsistentes entre chamadas (sintoma: KS2.0 mostrando
ícones quebrados). O caminho correto é um iterador por lock que carregue o
estado do namefix (e RR) entre chamadas. Enquanto isso, ExNext é O(n²) por
diretório — janela do CD abre lento (~0.8s de guest por entrada em dirs
grandes; medido ~5.6M ciclos/ExNext no live CD do AROS).

## ABERTO 1 — KS20 com ISO: boot trava em ROM (PC=0x00f813a8)

Reprodução headless:
```
./out/harness-rigel/harness src/roms/KS20.rom --adf src/disks/wb31.adf \
  --iso src/disks/aros-amiga-m68k.iso --headless --frames 6000
```
- EXEC-DUMP frame=3000: PC=0x00f813a8 (ROM), e fica aí até 6000.
- Só 8 READ_10 (16,16,16,105,5754,16,105,5754 = fase do mounter) e a
  enumeração nunca começa. TURs seguem 1/s (iotask vivo).
- Numa rodada ANTERIOR do mesmo dia (log `ks20cd`), com fonte ODFS
  aparentemente idêntica, o boot progrediu (PC=0x00c1071a em RAM no frame
  3000) e houve 315 READ_10 de enumeração. **Não foi identificada a variável
  que mudou.** Testado e descartado: edits de trace em ata_ide/atapi_cdrom
  (stash → mesmo stall), HARNESS_CD_BOOTABLE=1 (mesmo stall).
- Sintoma visual reportado: CD aparece como "dois disquetes" que não abrem.
- **Determinístico**: repetir o comando exato da rodada boa (13000 frames)
  reproduz o stall (8 reads, PC=0x00f813a8). Algo persistente mudou entre a
  rodada boa e as atuais — ainda não identificado. Já descartados: fonte do
  ODFS (revert byte-equivalente), edits de trace em src/machine (stash →
  mesmo stall), PVD on/off, ADF/ROM (mtimes/intactos).
- Hipótese do usuário: o volume de CD monta **sem o disk.info** → Workbench
  mostra ícone default (parece disquete) e não abre. Compatível com o dado:
  as 8 leituras param antes de qualquer leitura do handler (nem root dir),
  ou seja o handler do ODFS não chega a rodar/ler nada — sem root dir não há
  disk.info. Investigar por que o processo do handler não inicia (seglist do
  FileSysEntry? stack? crash silencioso no startup?).
- Próximo passo sugerido: os_debug (HARNESS_OS_DEBUG_DUMP=1 no frame 3000)
  para ver em que task/wait o exec está; comparar BootNodes; bisect real dos
  binários ODFS (guardar artefatos .bin de cada build para md5 — começar a
  arquivar `build/amiga/ODFileSystem` com hash a cada build).

## ABERTO 2 — AROS não chega ao workbook com ISO habilitado

`aros.rom + aros.adf + ISO`: headless chega a "InitCode: leave
InitCode(0x04,0)" e workbook.resource é chamado, mas o usuário reporta que o
workbook (desktop) não aparece no modo interativo. Sem ISO, chega. Pode ter a
mesma raiz do ABERTO 1. Investigar com os_debug e comparar contra rodada
sem ISO.

## Notas

- `cmd 55` no log = ATAPI MODE SELECT(10) (lide.device configurando mode
  pages). Não implementado no nosso modelo; devolve erro; inofensivo.
- KS13 mal exercita o CD (2 READ_10 em 13000 frames) — mount não ativa de
  verdade em V34; comportamento melhor em KS2.0/KS3.1 é esperado. Se CD em
  KS13 virar alvo, precisa mount "à moda antiga" no bootldr/mounter.
- Regenerar o patch: `cd external/ODFileSystem && git add -N
  platform/amiga/romtag.c && git diff > ../../patches/0012-odfs-cd01-romtag.patch`
  (depois `git reset -- platform/amiga/romtag.c`).
