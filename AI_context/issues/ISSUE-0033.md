---
id: ISSUE-0033
title: "Harness: RTG (gfx board) para distros que não usam Denise — ArosOne desktop"
status: open
priority: high
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-18
tags:
  - harness
  - rtg
  - denise
  - aros
  - arosone
related_files:
  - tools/harness/musashi_backend.c
  - src/chipset/denise/denise.c
  - AI_context/consolidated/memory_model.md
  - AI_context/consolidated/history/ISSUE-0031.md
---

# Contexto

ArosOne-68K boota no harness (68040 + Z2 fast RAM, ISSUE-0031) mas o
desktop nunca aparece: a distro é configurada para tela RTG (perfil
WinUAE usa uaegfx com 512MB VRAM). O harness só renderiza via Denise.
O Emu68 tem RTG próprio no hardware; o harness não tem nada.

> **Direção retomada em 2026-07-18:** esta issue volta a ser o plano ativo de
> RTG. A decisão anterior de tratar `bellatrix.rtg` como lab destinado à remoção
> foi superada após separar a placa P96/framebuffer portátil do backend físico
> VideoCore. O histórico abaixo não foi reescrito; a decisão e o plano novos
> estão no final desta issue e em `docs/rtg_design.md`.

# Objetivos (em ordem de custo)

1. **Curto prazo, sem RTG**: forçar screenmode nativo PAL na distro
   editando prefs/startup direto no HDF (tools/hdf write) e validar que o
   desktop ArosOne aparece via Denise. Documentar a receita.
2. **Médio prazo**: avaliar um board RTG mínimo no harness — candidatos:
   - uaegfx/P96 API (o que AROS m68k já tem driver pronto)
   - framebuffer simples estilo Emu68 (reusar o driver que o Emu68 expõe?)
   Depende de Z3 ([[ISSUE-0032]]) para VRAM decente; um RTG Z2 ficaria
   limitado a ~8MB de janela.

# Critério de pronto (fase 2)

Desktop de uma distro RTG-only (ArosOne) renderizado pelo harness em
resolução > PAL, com screenshot pelo pipeline existente
(HARNESS_SCREENSHOT_FRAMES).

# Arquitetura: dois "RTGs" diferentes (não confundir)

- **Guest API (AROS/AmigaOS)**: RTG = driver `.card` (P96/CGX; uaegfx no
  UAE). É isso que o ArosOne espera encontrar.
- **Emu68**: fornece um `.card` cuja implementação fala DIRETO com o
  VideoCore do Raspberry (mailbox/framebuffer VC4) — não há "placa"
  emulada.
- Consequência para o Bellatrix:
  - **Hardware**: caminho natural é reusar/adaptar o driver VC4 do Emu68
    (o Bellatrix já usa VC4 para a saída da Denise; falta arbitrar
    Denise vs RTG na mesma saída).
  - **Harness**: aí sim seria preciso emular a API do card escolhido
    (VRAM + registradores/mailbox), pois não há VC4.

# Fontes localizadas (2026-07-03)

- **Driver guest com source**: `external/aros/arch/m68k-amiga/hidd/p96gfx/`
  — o "uaegfx" do AROS m68k. `p96gfx_card.c` mostra DOIS backends:
  1. `CardBase` = library `.card` P96 real (caminho do Emu68/VC4);
  2. `p96romvector` = traps uaegfx do UAE (host-side = picasso96.cpp do
     WinUAE, que NÃO está em referencias/winuae; gfxboard.cpp/
     framebufferboards.cpp copiados lá são emulação de placas físicas —
     útil como referência, mas é o outro caminho).
- **Plano preferido**: escrever um `bellatrix.card` m68k (temos toolchain
  docker: lide.rom/ODFS) falando com janela VRAM+registradores simples.
  Harness: framebuffer→SDL. Hardware: mesmo card→VC4 (como o Emu68).
  Um driver guest, dois backends.
- ArosOne não traz uaegfx em binário (Devs/Monitors = Compositor+SAGA);
  o p96gfx vem na ROM do AROS m68k — basta o FindCard achar uma placa.

# Notas

- Não distorcer o modelo do chipset: RTG é um board de expansão, não
  parte da Denise (princípio "Denise é instância explícita").
- Ver memory_model.md para a discussão de presets (evitar 512MB VRAM
  estilo UAE; 16-32MB bastam).

# Implementação fase 1 (2026-07-03)

Feito (ver docs/rtg_design.md):
- Board Zorro II `bellatrix.rtg` (src/machine/expansions/rtg/): janela
  4MB = 4KB registradores + ~4MB VRAM; formatos CLUT/R5G6B5/A8R8G8B8;
  render p/ RGBA em bellatrix_rtg_get_frame(). `HARNESS_RTG=1` registra
  o board e reduz o fast Z2 para 4MB (espaço Z2 compartilhado). O branch
  de fast RAM do backend agora respeita a janela real do board
  (bellatrix_zorro2_fast_ram_window) para não engolir o RTG.
- Screenshot do harness prefere o frame RTG quando ENABLE=1.
- `cards/bellatrix.card` (m68k, compila no docker; 1.5KB): FindCard via
  FindConfigDev(0x07DB/0x10) + valida magic 'BRTG'; InitCard preenche
  BoardInfo (BIF_NOBLITTER, formatos, função set completo p/ dumb fb).
  boardinfo.h/settings.h vindos do VideoCore.card (MPL-2.0).
- p96gfx popula os modos sozinho (tabela rtgmodes[] interna) — a card
  não precisa montar ResolutionsList.

Pendente (fase 2 = tornar a card residente e validar boot):
- p96gfx tem residentpri -10 (ANTES do dosboot) → card não pode vir de
  disco. Decisão (2026-07-03): DiagArea no PRÓPRIO board RTG
  (AC_TYPE_DIAGVALID) — não acoplar ao lide; subsistemas independentes.
- Depois: saída SDL ao vivo (hoje só screenshot), backend VC4 baremetal,
  arbitragem Denise×RTG pelo ENABLE.

# Implementação fase 2 (2026-07-03) — DiagArea + CardLoader

Adicionado `cards/bellatrix.card/bootrom/` (cardldr.S + reloc.S + defs.i,
Makefile próprio, `scripts/build-rtg-rom.sh`, embutido via CMake como
`rtg_rom_data` — mesmo padrão do `lide_rom_data`). Janela do board
redesenhada em três regiões (`rtg.h`): registradores 0x000–0x0FF, ROM
(DiagArea+card) 0x100–0x2FFF, VRAM a partir de 0x3000.

**Dois bugs reais encontrados e corrigidos durante a validação** (não
eram hipóteses — confirmados por instrumentação host-side, byte a byte):

1. **`ripple_bus_owns()` em lide_cdrom.c tinha escopo global demais**:
   usava `bellatrix_zorro2_in_board_window(addr)`, que é true para
   QUALQUER board Z2 configurado, não só o próprio lide. Com um único
   board Z2 (só o lide) isso nunca dava problema; com o RTG como
   segundo board, o lide passou a "roubar" os acessos ao nosso board
   (retornando lixo de open-bus). Corrigido para checar especificamente
   `bellatrix_zorro2_board_base("lide.cdrom")` + seu próprio window
   size. Bug estrutural — teria pego qualquer segundo board Z2 futuro,
   não só o RTG.
2. **`defs.i` (constantes NDK reescritas à mão) tinha os valores ERRADOS
   para DAC_BUSWIDTH/DAC_NIBBLEWIDE/DAC_BYTEWIDE/DAC_WORDWIDE** — usei
   bits baixos (0x00–0x03) quando o real (`libraries/configregs.h`) usa
   o nibble alto (0x00/0x40/0x80, máscara 0xC0). Consequência: o AROS
   decodificava nossa DiagArea como NIBBLEWIDE em vez de WORDWIDE,
   produzindo da_Size/da_DiagPoint/da_Name com lixo. DAC_BOOTTIME/
   DAC_CONFIGTIME coincidiam por acaso e mascararam o bug por um tempo.

**Confirmado funcionando (instrumentação host-side, harness, aros.rom +
arosone.hdf, 68040)**:
- `diag init` (romtag nativo do aros.rom) processa nossa DiagArea:
  `da_Config=90 Size=02e0 DiagPoint=000e ... Name='BellatrixRTG CardLoader'`
  — todos os valores batem exatamente com o cardldr.S compilado (736B).
- `romboot.c`'s `romtaginit()` encontra nosso Romtag e chama
  `InitResident`: `Diag board 00600000 InitResident ... 'BellatrixRTG
  CardLoader'`.
- Nosso `Init:` roda e `_relocate` lê o hunk do bellatrix.card **byte a
  byte, sequencial, exatos 1552/1552 bytes** (tamanho do binário) — sem
  gaps, sem overshoot. Evidência forte de relocação bem-sucedida.

**Ainda não confirmado**: nenhum sinal de que `InitRT` (scan de romtag
pós-relocação) ou o `FindCard`/`SetSwitch` do p96gfx tenham rodado —
`RTG_REG_ID` nunca foi lido (instrumentado, zero hits em 6000 frames).
p96gfx.hidd tem seu próprio InitResident chamado (prioridade -10), mas
seus `bug()` de debug parecem compilados fora nesta ROM (diferente de
diag.c/romboot.c, que têm debug ativo) — não há trace nativo para
confirmar o scan de "*.card" na LibList.

**Próximo passo concreto**: verificar se "bellatrix.card" chega à
LibList — via dump de memória guest (ExecBase+LibList, mecanismo já
existe em `src/debug/os_debug.c:os_debug_dump`, mas o gate por frame
fixo em main.c não bateu com o timing de boot do RTG; ajustar o frame
ou adicionar um gatilho por condição) ou breakpoint no LVO
`_LVOInitResident`/`_LVOOpenLibrary` para "bellatrix.card". Regressão
verificada: wb20.hdf sem HARNESS_RTG continua bootando normalmente.

# Sessao 2026-07-06 — ArosOne-Lite.hdf (aros.rom, 68040): cadeia RTG completa ate InitCard

Alvo: bootar `src/disks/ArosOne-Lite.hdf` (RDB + particao UDH0 SFS\0 com driver SFS
embutido em FSHD/LSEG) no harness com aros.rom e 68040.

## Fatos estabelecidos (com evidencia)

- **FPU nao e mais o blocker no harness**: zero `[F-LINE-TRAP]` em runs de ate 21000
  frames (o crash "Line 1111 Emulator" da foto new_aros_hdf68040+rtg.jpg e do caminho
  bare-metal/anterior a ISSUE-0034; no harness 68040+FPU nao reproduz).
- **O HDF boota fundo**: SFS monta (2 handlers UDH0 + SFS DosList handler), startup-sequence
  roda (AddUSBClasses, ConClip, RexxMast), **Wanderer sobe** (2 processos), Decorator roda.
  Instrumentacao [HDF-R] confirma leituras profundas na particao (lba 225k-257k).
- **Blocker real de display**: distro e RTG-only; a Denise mostra cinza. Aos 19500 frames:
  todas as tasks em WAIT, TaskReady vazio, IntuitionBase sem NENHUMA tela.

## Bug real corrigido: card_init com registradores errados

`card_init` declarava `__REGA0(base), __REGA1(seglist)`. A convencao do
InitResident/MakeLibrary AUTOINIT (AROS rom/exec/initresident.c, AROS_UFC3) e
**D0=library, A0=segList, A6=SysBase**. Consequencia: base recebia o segList (0 via
CardLoader), os campos da lib eram escritos na pagina zero e o retorno NULL fazia o exec
descartar a library — bellatrix.card nunca entrava na LibList (o "pendente" da fase 2).
Fix: `__REGD0(base), __REGA0(seglist)`.

## Infra de diagnostico adicionada (reutilizavel)

- `RTG_REG_DEBUG` (0x34): registrador write-only; host loga `[RTG-DBG] <valor>`.
  cardldr.S marca estagios (B0000001 init, ..02+seg reloc ok, ..03+rt romtag achado,
  ..04 InitResident voltou, EE/EF falhas); card.c marca CAFD0001 (FindCard ok) e
  CAFD0003 (InitCard entrou).
- Host loga primeiro write de cada registrador RTG (`[RTG] first write reg=..`).
- `[HDF-R]` no harness: reads do HDF (RDB esparso + acessos a particao + sumario).
- `HARNESS_OS_DEBUG_DUMP=<frame>` agora aceita o frame do dump (antes fixo em 3000).
- `is_ram_ptr` do os_debug aceita fast RAM (AROS realoca ExecBase p/ 0x200000+; antes o
  dump abortava com "ExecBase invalid").

## Cadeia confirmada funcionando (breadcrumbs, ArosOne-Lite, 8000 frames)

```
DiagArea probed -> CardLoader Init (B0000001) -> reloc ok (seg 0x29729c)
-> romtag @0x297660 -> InitResident volta (B0000004)
-> "bellatrix.card" NA LibList (dump) -> p96gfx FindCard: REG_ID probed,
   retorna 1 (CAFD0001) -> InitCard roda (CAFD0003)
```

## Blocker atual (proxima sessao)

`SetSwitch`/`SetGC`/`SetPanning` nunca sao chamados => nenhum OpenScreen chega ao driver
RTG; aos 19500 frames o sistema inteiro dorme em Wait (Wanderer espera bit 4; Intuition
espera 0x10000). Hipoteses, em ordem:

1. **Mode matching**: prefs de screenmode do ArosOne (perfil WinUAE/uaegfx) pedem um
   modeid que o p96gfx (tabela rtgmodes[] interna) nao casa; OpenScreen falha e o
   Wanderer fica em retry/espera silenciosa. Verificacao: instrumentar OpenScreen/
   BestModeID via LVO breakpoint host-side, ou editar prefs no HDF (bloqueado por SFS —
   ver ISSUE-0028).
2. **VBL interrupt do board**: p96gfx pode depender de bi->SetInterrupt + interrupt real
   do card para o refresh/arbitro de modos; nosso board nao gera interrupts (SetInterrupt
   e no-op). Se for isso: gerar INT2/INT6 por frame quando ENABLE=1 (rigel/paula ja tem
   INTREQ acessivel pelo machine layer).
3. p96gfx registra o monitor mas o AROS escolhe amigavideo como default e o ArosOne
   (RTG-only) nao abre nada — checar MonitorList/hidd via os_debug (extensao facil).

Nota 68040: rodado com `--cpu 68040` (FPU da ISSUE-0034). Sem F-line em nenhum run.

# Retomada 2026-07-18 — framebuffer P96 portátil inspirado no Minimig/MiSTer

## Identidade no log e bug de ativação pelo wrapper

- placa/`BoardName`: `Bellatrix RTG`;
- biblioteca P96 no guest: `bellatrix.card`;
- ativação pedida: `[HARNESS] RTG enabled: Bellatrix RTG (bellatrix.card), Zorro III 8MB`;
- modo efetivamente aberto: `[RTG] enable=1 ...`.

Foi reproduzido `HARNESS_RTG=1 ./run.sh harness` imprimindo `RTG disabled`.
Não era outra branch nem falha do AutoConfig: sem `KICKSTART` explícito,
`load_launcher_selection` carregava `BELLATRIX_RTG=0` do perfil e atribuía
esse valor a `HARNESS_RTG`, apagando a variável fornecida pelo usuário. A
precedência foi corrigida: `HARNESS_RTG` explícito vence o perfil persistido.

## Desempenho: VRAM direta

A janela continua EXTERNAL para preservar os efeitos dos registradores e da
ROM, mas o primeiro acesso ao framebuffer instala uma região DIRECT apenas em
`base+0x3000 .. base+0x7fffff`. Isso elimina o bridge e a decomposição byte a
byte dos acessos seguintes do P96. O mapeamento é alinhado a 4 KiB, removido no
reset e coberto pela integração AROS, que exige o log
`direct VRAM mapped: 40003000-407fffff` antes de aceitar o scanout.

O desenho linha a linha continuou visível com SDL confirmando VSync realmente
desligado. O profiler mostrou ~98,7% em `cpu_run`; portanto VSync não era o
blocker. O instruction hook do Musashi era registrado incondicionalmente apesar
de todas as sondas estarem desligadas. Agora ele só é instalado quando uma das
flags de trace/probe dependentes estiver ativa. A vazão medida no boot AROS de
1.000 frames subiu de ~24–25M para ~27–31M ciclos guest/s. Quantum 4096 também
foi validado, com ganho menor, e não virou default por afetar granularidade.

## Pesquisa no AROS local — plano do driver

Fonte: `external/aros/arch/m68k-amiga/hidd/p96gfx/`.

Achado decisivo: `p96gfx_bitmapclass.c` tenta `FillRect` para bitmaps em VRAM e
faz fill pela CPU se `AROSFlag` indicar não tratado. `p96gfx_hiddclass.c` faz o
mesmo com `BlitRectNoMaskComplete` antes do superclass. `p96gfx_rtg.c` instala
`RTGCall_Default` nos callbacks ausentes; esse default limpa `AROSFlag`. Nossa
card deixa os callbacks default e anuncia `BIF_NOBLITTER`. Assim, o desenho
progressivo observado é explicado pelo fallback CPU, não por falta de buffers
no SDL. `SetPanning` aparece no caminho de mostrar/trocar screen e só faz page
flip se o guest realmente fornecer outro `VideoData`.

Plano aprovado para execução incremental:

1. probes que contam operações/dimensões/formatos e recusam corretamente via
   `AROSFlag=0`;
2. command ABI síncrona host↔card com validação estrita de toda faixa VRAM;
3. `FillRect` nos três formatos, começando por máscara completa;
4. `BlitRect` overlap-safe e `BlitRectNoMaskComplete` COPY VRAM↔VRAM;
5. `InvertRect`; template/pattern/line somente conforme telemetria;
6. remover `BIF_NOBLITTER`/adicionar `BIF_BLITTER` apenas quando a promessa
   mínima estiver coberta por testes;
7. instrumentar `SetPanning` e aplicar page flip no VBlank se houver troca real
   de bitmap; não criar triple buffer host para mascarar fallback lento;
8. validar com unitários de bounds/overlap/formato, integração AROS,
   screenshots e benchmark de operações, preservando fallback software para
   todo opcode/máscara/minterm ainda não implementado.

## Correção de direção

O backend VideoCore não é requisito do RTG atual. Ele é uma possível solução
futura de apresentação no Raspberry, provavelmente integrada pelo próprio Emu68.
O alvo imediato é uma placa P96 mínima cuja VRAM e estado de scanout possam ser
consumidos tanto pelo harness quanto pelo hardware:

```
bellatrix.card → VRAM + registradores → scanout comum → SDL | Raspberry
```

Referência comportamental escolhida: `extra/rtg_driver/MiSTer.card.asm` do
Minimig-AGA MiSTer. O driver funcional usa 8 MB de VRAM linear e apenas endereço,
formato, enable, largura, altura, stride e CLUT. VBL interrupt, blitter, sprite e
clock real estão ausentes ou desabilitados. Isso invalida a hipótese de que seria
necessário resolver VC4 ou interrupções antes do primeiro desktop.

## Estado real de partida

- O RTG Bellatrix já é uma board Z3 EXTERNAL no `board_registry`.
- O host já implementa VRAM, registradores, paleta e conversão de três formatos.
- O harness já consegue capturar o frame RTG em screenshot.
- DiagArea → CardLoader → LibList → FindCard → InitCard foi comprovado no AROS.
- O primeiro bloqueio continua sendo a ausência de `SetGC`/`SetPanning`/
  `SetSwitch`, logo está acima do framebuffer host.
- Há lógica obsoleta no harness reduzindo Fast RAM Z2 quando `HARNESS_RTG=1`,
  apesar de o RTG ter migrado para Z3; deve ser removida na fase 0.

## Plano ativo

1. **Contrato e testes host:** comparar MiSTer/Bellatrix/AROS, congelar a spec e
   testar CLUT, RGB565, 32-bit, pan, stride e limites sem guest.
2. **P96 conhecido no harness:** alinhar callbacks ao MiSTer e validar primeiro
   com AmigaOS + Picasso96 carregando a `.card` por disco; apresentar via SDL.
3. **AROS base:** retomar a residência já funcional e identificar o motivo de o
   HIDD não selecionar modo depois de `InitCard`.
4. **ArosOne:** desktop RTG-only em resolução maior que PAL.
5. **Raspberry:** ligar o mesmo scanout a um presenter futuro, sem alterar a ABI.

## Próximo incremento de código

- criar testes unitários para o register file/scanout;
- retirar o acoplamento RTG Z3 → redução de Fast RAM Z2;
- corrigir divergências objetivas encontradas na comparação com MiSTer, começando
  pela semântica de retorno de `SetSwitch`, formatos e `SetPanning`;
- adicionar apresentação SDL contínua somente depois que os oráculos host estiverem
  verdes.

## Critério do primeiro marco

Um padrão escrito diretamente na VRAM, com modo programado pelos mesmos registros
que a `.card` usa, deve produzir frame correto nos três formatos e screenshot
determinística. Esse marco não depende de ROM, HDF, AROS, SDL nem Raspberry.

## Execução 2026-07-18 — primeiro oráculo concluído

- Extraído `rtg_scanout.c/.h`: núcleo puro, sem machine/AutoConfig/SDL/VC4, que
  contém estado, contrato de registradores, validação e conversão para RGBA.
- `rtg.c` agora é o adaptador Z3: serve a janela/ROM/VRAM e delega o scanout ao
  núcleo portátil.
- Adicionado `bellatrix_unit_rtg_scanout`, cobrindo ID/versão/VRAM, auto-incremento
  e wrap da paleta, VBL tick, CLUT, R5G6B5, A8R8G8B8, stride, pan e rejeição de
  stride curto ou frame fora da VRAM.
- Removida a redução de Fast RAM Z2 para 4 MB quando `HARNESS_RTG=1`; RTG é Z3 e
  não disputa essa janela.
- Build completo do harness passou, incluindo a reconstrução de `bellatrix.card`
  e `rtg.rom` (9512 bytes). O erro anterior não era do wrapper: o sandbox bloqueou
  o socket vsock do `wslc.exe`; com execução autorizada fora do sandbox o wrapper
  existente funcionou sem mudanças.
- Validação: 15/15 testes Bellatrix relevantes verdes, incluindo smoke, ADF e
  boots KS1.3/KS2.0/KS3.1/AROS.
- Primeira divergência da matriz corrigida: `SetSwitch` agora retorna o inverso
  do estado selecionado, como exigido pelo P96 e implementado por MiSTer.card e
  VideoCore.card. O caminho de display do AROS ignora esse retorno, então a
  correção é de contrato, não uma explicação suficiente para o bloqueio antigo.

**Próximo passo:** fechar a matriz callback/semântica MiSTer × bellatrix.card ×
AROS e corrigir divergências comprovadas antes de iniciar o teste AmigaOS/P96.

## Correção histórica 2026-07-18 — o blocker não era mode matching

**Suposição errada preservada acima:** após a sessão de 2026-07-06, concluímos que
`InitCard` funcionava, mas `SetGC`/`SetPanning`/`SetSwitch` não eram chamados, e
priorizamos mode matching, monitor default e VBL. Isso deixou de representar a
execução depois que o RTG foi migrado para Z3.

**Causa comprovada:** `tools/harness/musashi_backend.c` devolvia open bus para
qualquer endereço `> 0x00ffffff` antes de consultar `machine_dispatch`. O guest
configurava a board em `0x40000000`, mas nenhuma leitura/escrita posterior da
janela alcançava `rtg_bus_ops`. Antes da correção, `diag init` leu `da_Config=f0`
(open bus mascarado); não houve sequer o log da primeira leitura da ROM RTG.

**Correção:** consultar `bellatrix_boards_external_window_owner(addr)` antes do
fallback open bus. Se houver owner configurado e `is_z3`, encaminhar pelo bridge;
probes/endereço Z3 sem owner continuam retornando open bus e não sofrem máscara
para 24 bits.

**Evidência posterior, mesma `aros.rom`, 1000 frames:** `da_Config=90`, CardLoader
completa (`b0000004`), `FindCard` (`cafd0001`), `InitCard` (`cafd0003`), paleta,
`SetGC=640x480`, formato CLUT, stride 640, pan 0 e `enable=1`. Logo o AROS já
selecionava um modo corretamente; a hipótese de mode matching não explicava a
ausência dos callbacks.

**Segunda suposição corrigida:** os primeiros `_Static_assert` de `BoardInfo`
falharam e pareceram indicar ABI deslocada. O erro estava nos offsets esperados,
calculados 10 bytes à frente. Probe compilado pela toolchain m68k confirmou que a
struct existente casa o AROS (`PixelClockCount=254`, `SetSwitch=282`, `SetGC=294`,
`SetPanning=298`). Os asserts foram corrigidos e mantidos.

## Execução 2026-07-18 — Z3 destravado e primeiro modo programado

- Musashi agora encaminha janelas Z3 EXTERNAL configuradas ao bridge antes do
  fallback open bus. Probes Z3 sem owner continuam open bus.
- Janela RTG ampliada de 4 para 8 MB (`0x40000000–0x407fffff`, 8180 KB úteis),
  suficiente para `1440x900x32`; Fast RAM Z2 permanece 8 MB.
- `WaitVerticalSync` virou no-op como no MiSTer, removendo um deadlock potencial
  entre callback guest e tick do loop host.
- Adicionados `_Static_assert` m68k dos offsets P96 críticos da `BoardInfo`.
- `machine_present_frame_from_rigel()` agora prefere o frame RTG ativo no harness
  e converte seus bytes RGBA para a superfície RGB565 SDL; Denise continua sendo
  o fallback quando RTG está desabilitado.
- Corrigido o campo `format` não inicializado na captura RTG de `screenshot.c`.
- Novo teste `bellatrix_harness_rtg_aros`: prova Z3 8 MB + DiagArea 0x90 +
  CardLoader + FindCard + InitCard + `640x480 CLUT/stride 640/pan 0/enable 1`, e
  rejeita regressão em que `0x40000000` volte a open bus.
- Validação completa: 16/16 testes Bellatrix verdes, incluindo a nova integração
  RTG e boots KS1.3/KS2.0/KS3.1/AROS.

Uma screenshot sem mídia no frame 900 foi corretamente produzida como RTG
`640x480`, mas estava preta/uniforme. Isso comprova seleção e captura do scanout,
não ainda o critério final de conteúdo/desktop visível. O próximo alvo é bootar
mídia adequada e obter frame RTG não uniforme; depois validar apresentação SDL.
