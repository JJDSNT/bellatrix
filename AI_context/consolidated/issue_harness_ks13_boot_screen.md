# Issue: Harness — Investigação da Tela de Boot KS1.3

## Status: CLOSED (2026-06-26)

Happy Hand e Workbench funcionando em hardware. LOF fix implementado no Rigel
(`beam.lof = 0` na init). O bitmap producer passou a rodar, bitplane buffers
populados, tela de boot visível. Esta investigação é histórica — serve como
referência de debug se regressão aparecer.

## Contexto

A tela de boot do Kickstart 1.3 ("insert disk" / "happy hand") nunca aparece no
harness Linux. Esta investigação rastreou o problema desde "ROM executando" até
identificar o bloqueador exato: um loop de polling LOF no agnus que nunca sai.

## Timeline da Investigação

| Sprint | Hipótese/Descoberta | Status |
|--------|--------------------|----|
| 14 | Copper COPJMP sem log; ROM em loop esperando DSKBLK | Resolvido |
| 15 | SERDATR TBE oscillação; VPOSR chip ID=0 | Resolvido |
| 16 | CIA timer múltiplos underflows; floppy direction invertida | Resolvido |
| 17 | Rendering batch vs raster-time (nplanes=0 em todas linhas) | Resolvido |
| 18 | CACR_IE não setado → exec_pc=0 | Resolvido |
| 19 | v30 corrompido por kprintf | Resolvido |
| 20 | Bridge serial não drenando TX | Resolvido |
| 21 | OVL fora do live path | Resolvido |
| 22 | CIA/Paula/Input completion | Resolvido |
| 23 | Chip RAM 512K vs 2MB; constantes centralizadas | Resolvido |
| 24 | DMA gating muito estrito (BPLEN=0 bloqueando fetch válido) | Parcialmente resolvido |
| 25 | Disk path: DF0 + ADF + DSKBLK verificados | Confirmado OK |
| 26 | LOF spin em 0xfc5a6c: `btst #$6, $dff002` aguarda LOF=0 | **Bloqueador ativo** |

## Estado Confirmado (Após Sprint 26)

### O que funciona no harness
- ROM carregada e executando (`[BELA] ROM @ 0xf80000: ...` correto)
- OVL toggle detectado
- VBL a 50Hz
- Copper list executada (raster-time)
- CIA timers funcionando
- IPL pipeline
- Disk path: DF0 inserido, Paula DMA, DSKBLK gerado, CPU ACK confirmado
- Display callback chain completa (14 steps via VBL)

### Estado da Tela no Harness (Sprint 26)

**Display setup callback chain** em `0x1892`:

| Step | PC | Role |
|------|-----|------|
| 1 | `0xfe8772` | Entry — instala self, alloc struct |
| 2 | `0xfe87f2` | Clears/inits sub-block A |
| 3 | `0xfe87fa` | Clears/inits sub-block B |
| 4 | `0xfe8810` | Seta campos pequenos |
| 5 | `0xfe8828` | Seta display-struct fields |
| 6 | `0xfe883a` | Wire Copper/display context |
| 7 | `0xfe8882` | Ativa display context |
| 8 | `0xfe8888` | Programa basic video registers |
| 9 | `0xfe888e` | UI/layout init |
| 10 | `0xfe889c` | Layout pass 2 |
| 11 | `0xfe88ac` | Layout pass 3 / Copper list commit |
| 12 | `0xfe88d2` | Bitplane pointer commit |
| 13 | `0xfe88e0` | Final struct wiring |
| **14** | **`0xfe891c`** | **Terminal — self-installs, NÃO avança** |

**Estado após step 14**:
```
dmacon  = 02d0   (DMA on, bitplane DMA enabled)
bplcon0 = 2302   (2 bitplanes, color mode active)
bpl1    = 0a892  (set pelo Copper, geometry correto)
bpl2    = 0c7d2
cop1    = 02368  cop2 = 10450
p0      = 00000000   ← buffers ainda zero
p1      = 00000000   ← buffers ainda zero
```

**Bitplane buffers**:
```
A5=0x0018b6
A5+24 = 0x0000a572  ← BPL1 buffer (0x1f40 bytes) — todo zero
A5+28 = 0x0000c4b2  ← BPL2 buffer (0x1f40 bytes) — todo zero
```

## Bloqueador Ativo: LOF Spin em `0xfc5a6c`

### O código
```
0xfc5a6c: btst #$6, $dff002.l   ; testa bit 6 = LOF de VHPOSR
0xfc5a72: bne  $fc5a6c           ; loop enquanto LOF=1 (long frame)
```

Esta barreira espera um **short frame** (LOF=0) antes de commitar o display list e
escrever o bitmap payload nos buffers `A5+24` e `A5+28`.

### Por que nunca sai
Agnus retorna LOF=1 permanentemente em `VHPOSR`. Em modo PAL não-interlace, LOF
deve ser **0** sempre (frame always short). O Kickstart aguarda LOF=0 → nunca
ocorre → bitmap producer nunca roda → buffers permanecem zero.

### Fix Necessário
Em `src/chipset/agnus/agnus.c` (ou onde VHPOSR/`0xDFF006` é construído):
- `beam.lof = 0` em modo não-interlace PAL normal
- Em interlace mode: `beam.lof ^= 1` por frame
- VHPOSR bit 15: LOF = `s->beam.lof`
- VHPOSR bit 7..0: `vpos >> 8`

**Este é o fix de mais alta confiança para desbloquear a tela de boot.**

## Investigação do Bitmap Producer (Sprint 24-25)

### Quem deveria popular `0xa572/0xc4b2`?
Disassembly do ROM path `0xfe8810` (step 4 da chain) mostra:
```asm
move.l ($24,A5), ($c,A0)    ; wire pointer BPL1
movea.l ($10,A5), A1
jsr (-$c6,A6)
...
move.l #$1f40, D0
jsr (-$1d4,A6)
```
Este código **wire pointers** em estruturas de controle. Não escreve pixels.

O helper em `0xfcc5c0` constrói **Copper list entries** (writes de 6 bytes: control word + argument). Não é bitmap fill.

Blitter activity: **zero** observado durante toda a fase video/boot → blitter não
é o producer no caminho observado, OR um subsystem missing impede de chegar ao
blitter path.

### Conclusão Sprint 25
Disk path confirmado OK. Próximo bloqueador não é "DSKBLK nunca disparo".
O bitmap producer está gated atrás do LOF barrier em `0xfc5a6c`.

## Instrumentação Útil no Harness

### Filtro recomendado
```bash
./run.sh harness | grep -E '1892-WATCH|BOOT-DISPLAY-SETUP|BPL-DIAG|VHPOSR|LOF'
```

### Checkpoints existentes
- `[1892-WATCH]` — writes para `0x1892..0x1895` (self-install chain)
- `[BOOT-DISPLAY-SETUP]` — PC range `0xfe8768..0xfe8960`
- `[BOOT-DSKBLK-ACK]` — ACK real do disk IRQ (INTREQ `raw=0002/1002`)
- `[BOOT-AFTER-DSK]` — trace window após disk completion
- `[BOOT-DISPLAY-BUFFER-W]` — writes nos buffers `A5+24/28`
- `[BPL-DIAG-FETCH/DONE]` — fetch real de bitplane

## Próximos Passos (Após Fix LOF)

1. **Fix LOF em Agnus** — `beam.lof = 0` em não-interlace
2. Verificar se bitmap producer roda (esperar `[BOOT-DISPLAY-BUFFER-W]` com values != 0)
3. Se pixels ainda não aparecem: verificar se `denise_render_line` recebe `nplanes > 0`
   e `fetched words != 0`
4. AROS ROM como path paralelo (source disponível em `external/aros`)

## Arquivos de Investigação
- `tools/harness/musashi_backend.c` — checkpoints e instrumentação
- `src/chipset/agnus/agnus.c` — `agnus_get_beam()`, `VHPOSR` — onde o fix deve ir
- `src/chipset/agnus/bitplanes.c` — `bitplanes_dma_allowed()`, linha latch
