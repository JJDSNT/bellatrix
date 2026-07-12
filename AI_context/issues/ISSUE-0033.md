---
id: ISSUE-0033
title: "Harness: RTG (gfx board) para distros que não usam Denise — ArosOne desktop"
status: open
priority: low
type: enhancement
owner: unassigned
created_at: 2026-07-03
updated_at: 2026-07-03
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
